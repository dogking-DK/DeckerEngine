"""Real runner discovery, CPU environment example and independent process reload."""
import argparse
import json
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--example", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    root = Path(args.work).resolve() / str(uuid.uuid4())
    root.mkdir(parents=True)

    def run(path, expected=0):
        result = subprocess.run([args.runner, "--project-root", str(root), "--batch", str(path),
                                 "--auto-guard"], capture_output=True, encoding="utf-8", timeout=20)
        assert result.returncode == expected, (result.stdout, result.stderr)
        assert not result.stderr, result.stderr
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        (root / (Path(path).stem + "-responses.json")).write_text(json.dumps(rows, indent=2), encoding="utf-8")
        return rows

    def batch(name, commands, expected=0):
        path = root / (name + ".jsonl")
        path.write_text("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n"
                                for i, (m, p) in enumerate(commands)), encoding="utf-8")
        return run(path, expected)

    created = run(Path(args.example).resolve())
    assert all(row.get("result", {}).get("status") == "succeeded" for row in created), created
    initial = created[-1]["result"]["value"]
    scene_path = root / "scene.json"
    persisted_bytes = scene_path.read_bytes()
    persisted = json.loads(persisted_bytes)
    assert persisted["version"] == 2
    assert persisted["environment"] == initial["environment"]
    assert initial["environment"]["enabled"] and not initial["state"]["dirty"]
    commands = [("commands.list", {}), ("commands.describe", {"name": "environment.query"}),
                ("commands.describe", {"name": "environment.set"}),
                ("scene.load", {"manifest": "project.json"}), ("environment.query", {}),
                ("environment.set", {"environment": initial["environment"]}),
                ("environment.query", {}), ("history.status", {})]
    loaded = batch("reload-discovery", commands)
    assert all("result" in row for row in loaded), loaded
    descriptors = loaded[0]["result"]["value"]
    names = {d["name"]: d for d in descriptors}
    assert names["environment.set"]["undoable"] and names["environment.set"]["effect"] == "memory_edit"
    assert names["environment.query"]["effect"] == "query"
    query_schema = loaded[1]["result"]["value"]["result"]["properties"]["environment"]
    set_schema = loaded[2]["result"]["value"]["parameters"]["properties"]["environment"]
    assert query_schema == set_schema
    restored = loaded[4]["result"]["value"]
    assert restored["environment"] == initial["environment"]
    for field in ("scene_id", "revision", "dirty", "entity_count"):
        assert restored["state"][field] == initial["state"][field]
    assert restored["state"]["document_id"] != initial["state"]["document_id"]
    assert loaded[6]["result"]["value"] == restored
    assert loaded[7]["result"]["value"]["undo_count"] == 0
    assert scene_path.read_bytes() == persisted_bytes
    # Cloud content is authored and reloaded on the pure CPU runner in distinct processes.
    cloud_env = json.loads(json.dumps(initial["environment"]))
    assert cloud_env["version"] == 2
    cloud_env["clouds"].update(enabled=True, seed=4294967295, coverage=0.8,
                                bottom_height_m=1000, top_height_m=6000,
                                wind_velocity_mps=[-11, 23])
    edited = batch("cloud-edit", [("scene.load", {"manifest": "project.json"}),
                                 ("environment.set", {"environment": cloud_env}),
                                 ("scene.save", {}), ("environment.query", {})])
    assert edited[-1]["result"]["value"]["environment"] == cloud_env
    cloud_reload = batch("cloud-reload", [("scene.load", {"manifest": "project.json"}),
                                          ("environment.query", {})])
    assert cloud_reload[-1]["result"]["value"]["environment"] == cloud_env
    assert not cloud_reload[-1]["result"]["value"]["state"]["dirty"]
    # A Scene v2 / environment v1 is migrated only on disk reads.
    persisted["environment"]["version"] = 1
    del persisted["environment"]["clouds"]
    scene_path.write_text(json.dumps(persisted), encoding="utf-8")
    atmo = batch("atmosphere-migration", [("scene.load", {"manifest": "project.json"}),
                                         ("environment.query", {}), ("scene.save", {})])
    assert atmo[1]["result"]["value"]["environment"]["clouds"]["enabled"] is False
    assert json.loads(scene_path.read_bytes())["environment"]["version"] == 2
    # Separate legacy input: exact v1 shape, same identity/revision, migration is explicit on save.
    persisted["version"] = 1
    del persisted["environment"]
    scene_path.write_text(json.dumps(persisted), encoding="utf-8")
    legacy = batch("migration", [("scene.load", {"manifest": "project.json"}),
                                 ("environment.query", {}), ("scene.save", {})])
    state = legacy[1]["result"]["value"]
    assert state["environment"]["enabled"] is False
    assert state["state"]["revision"] == initial["state"]["revision"]
    assert json.loads(scene_path.read_bytes())["version"] == 2
    print(f"Environment CPU example, discovery, cloud v2 cross-process roundtrip and Scene/environment v1 migrations passed: {root}")


if __name__ == "__main__":
    main()
