"""Real dk-ctl/runner acceptance; owned child processes only, no endpoint scanning."""

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
from uuid import uuid4

from decker import BatchError, Client, Command, JobError, RpcError, TransportError, guard


def require(condition, message):
    if not condition:
        raise AssertionError(message)


@contextmanager
def server(args, root, endpoint):
    # Files avoid pipe backpressure while the server lives. Only this child is reaped.
    with (root / f"{uuid4().hex}.stdout").open("wb") as output, (root / f"{uuid4().hex}.stderr").open("w+b") as errors:
        extra = ["--pipe-timeout-ms", str(args.pipe_timeout_ms)] if hasattr(args, "pipe_timeout_ms") else []
        process = subprocess.Popen([args.runner, "--project-root", str(root), "--pipe", endpoint, *extra],
                                   stdin=subprocess.DEVNULL, stdout=output, stderr=errors,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        client = Client(endpoint, args.client)
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    client.hello(timeout=0.2)
                    break
                except TransportError:
                    require(process.poll() is None, "Runner exited during startup")
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            yield client
            client.call("runtime.shutdown")
            require(process.wait(timeout=20) == 0, "Runner shutdown failed")
            require(output.tell() == 0, "Pipe runner wrote non-protocol stdout")
            errors.seek(0)
            diagnostics = errors.read().decode("utf-8")
            # The installed optional AMD switchable layer reports this loader warning.
            # All other diagnostics (including validation errors) still fail acceptance.
            allowed = ("[graphics.device] Loader Message: Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 "
                       "which is older than the application specified API version of 1.4. May cause issues.")
            require(all(args.gpu and line == allowed for line in diagnostics.splitlines()),
                    f"Unexpected runner diagnostics: {diagnostics}")
            if diagnostics:
                print(f"Known optional driver-layer warning ({len(diagnostics.splitlines())} occurrences): {allowed}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


def cpu(args, root, endpoint):
    with server(args, root, endpoint) as client:
        hello = client.hello()
        require(hello["protocol"] == 1, "Wrong IPC version")
        state = client.call("scene.new", {"name": "Python 中文"}).value
        params = {"guard": guard(state)}
        created = client.call("entity.create", params)
        retried = client.call("entity.create", params, retry=created.ticket)
        require(retried.raw == created.raw, "Retry changed original TaskId/result")
        require(client.task(created.task_id)["status"] == "succeeded", "TaskId lookup failed")
        entity = created.value["created_id"]
        state = created.value["state"]
        try:
            client.call("entity.create", params)
            raise AssertionError("Stale guard succeeded")
        except RpcError as error:
            require(error.code == -32007 and error.task_id, "Conflict metadata lost")
        state = client.transaction(guard(state), [
            Command("entity.set_name", {"id": entity, "name": "中文 'quoted' \"entity\""}),
            Command("entity.set_transform", {"id": entity, "transform": {
                "translation": [1, 2, 3], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}}),
        ]).value["state"]
        before = client.call("scene.query").value
        try:
            client.transaction(guard(state), [Command("entity.set_name", {"id": entity, "name": "must rollback"}),
                                            Command("entity.delete", {"id": str(uuid4())})])
            raise AssertionError("Invalid transaction succeeded")
        except RpcError:
            require(client.call("scene.query").value == before, "Transaction leaked partial edits")
        try:
            client.batch([Command("entity.set_name", {"id": entity, "guard": guard(state), "name": "批量 committed"}),
                          Command("missing.method"), Command("scene.new")])
            raise AssertionError("Batch did not fail")
        except BatchError as error:
            require(error.failed_index == 1 and len(error.completed) == 1, "Batch partial results lost")
            require(isinstance(error.cause, RpcError) and error.cause.code == -32601, "Batch error lost")
            state = error.completed[0].value["state"]
        require(client.call("entity.get", {"id": entity}).value["name"] == "批量 committed", "Batch rolled back")
        replies = client.batch([Command("scene.save", {"guard": guard(state)}),
                                Command("project.save", {"guard": guard(state), "manifest": "project.json"})])
        require(len(replies) == 2, "Save batch incomplete")
        # Real asynchronous CPU Job; GPU acceptance also exercises a worker failure.
        imported = client.call("assets.import", {"source": "assets/mask.gltf"})
        finished = client.wait_job(imported.value["job_id"])
        require(finished["state"] == "succeeded", "CPU import did not finish")
        # Structural error and exact uint64 never silently turn into a float.
        try:
            client.call("entity.create", {"guard": {"document_id": state["document_id"], "revision": 2**64-1}})
            raise AssertionError("Invalid revision unexpectedly matched")
        except RpcError as error:
            require(error.code == -32007, "uint64 value was narrowed or rounded")
    with server(args, root, endpoint) as client:
        try:
            client.call("entity.create", params, retry=created.ticket)
            raise AssertionError("Old session replayed")
        except TransportError as error:
            require(error.execution == "not_sent", "Old-session rejection lost execution state")
        client.call("scene.load", {"manifest": "project.json"})
        require(client.call("entity.get", {"id": entity}).value["name"] == "批量 committed", "Reload lost edits")
        require(len(client.call("scene.query").value["entities"]) == 1, "Retry duplicated edit")
    try:
        Client("absent-" + uuid4().hex, args.client).hello(timeout=0.1)
        raise AssertionError("Absent endpoint succeeded")
    except TransportError as error:
        require(error.execution == "not_sent", "Absent endpoint marked as executed")


def gpu(args, root, endpoint):
    with server(args, root, endpoint) as client:
        require(client.call("runtime.capabilities").value["render_capture"], "Capture not compiled")
        state = client.call("scene.load", {"manifest": "project.json"}).value
        camera = {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60}
        params = {"guard": guard(state), "width": 64, "height": 64, "camera": camera,
                  "output": "first.ppm", "validation": "required"}
        first = client.capture(params)
        artifact = first.collect(root)
        require(first.job["result"]["draw_count"] == 3, "Capture did not draw fixture")
        payload = artifact.path.read_bytes().split(b"\n", 3)[3]
        require(len({payload[i:i+3] for i in range(0, len(payload), 3)}) >= 3, "Uniform image")
        disk_hash = hashlib.sha256((root / "scene.json").read_bytes()).hexdigest()
        entities = client.call("scene.query").value["entities"]
        commands = [Command("entity.set_name", {"id": item["id"], "name": f"Python 参数 {index}"})
                    for index, item in enumerate(entities)]
        commands += [Command("entity.set_transform", {"id": "20000000-0000-4000-8000-000000000001",
                       "transform": {"translation": [3, -0.125, 0.25], "rotation": [0, 0, 0, 1], "scale": [0.5, 0.5, 1]}})]
        state = client.transaction(guard(state), commands).value["state"]
        params.update(guard=guard(state), output="edited.ppm")
        second = client.capture(params)
        edited = second.collect(root)
        require(first.job["result"]["revision"] < second.job["result"]["revision"], "Revision did not advance")
        require(artifact.sha256 != edited.sha256, "Edited parameters did not change image")
        require(hashlib.sha256((root / "scene.json").read_bytes()).hexdigest() == disk_hash, "Capture saved scene")
        example = Path(__file__).resolve().parents[2] / "examples/automation/batch_capture.py"
        demo = subprocess.run([sys.executable, str(example), "--pipe", endpoint, "--ctl", args.client,
                               "--project-root", str(root)], capture_output=True, timeout=70,
                              creationflags=subprocess.CREATE_NO_WINDOW)
        require(demo.returncode == 0, f"Example failed: {demo.stderr.decode('utf-8', errors='replace')}")
        report = json.loads(demo.stdout)
        require(report["artifact"]["width"] == 256 and Path(report["artifact"]["path"]).is_file(),
                "Example did not collect artifact")
        params["guard"] = guard(client.call("scene.query").value["state"])
        # A failed Job preserves the existing artifact and the accepted submission metadata.
        (root / "assets/mask.png").unlink()
        try:
            client.capture(params)
            raise AssertionError("Missing texture capture succeeded")
        except JobError as error:
            require(error.job["state"] == "failed" and error.submission.value["job_id"] == error.job["id"],
                    "Capture failure lost submission")
        require(second.collect(root).sha256 == edited.sha256, "Failed job replaced prior artifact")
        (root / "artifacts.json").write_text(json.dumps([
            {"path": str(a.path), "width": a.width, "height": a.height, "size_bytes": a.size_bytes, "sha256": a.sha256}
            for a in (artifact, edited)], indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    # Leave room for asset cache atomic-save temporary paths under MAX_PATH.
    root = Path(args.work_dir) / ("中文 space-" + uuid4().hex[:12])
    shutil.copytree(args.fixtures, root)
    endpoint = "python-" + uuid4().hex
    (gpu if args.gpu else cpu)(args, root, endpoint)
    print(f"Python {'GPU capture' if args.gpu else 'CPU IPC'} acceptance passed: {root}")


if __name__ == "__main__":
    main()
