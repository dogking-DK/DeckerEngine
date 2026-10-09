"""The same finite-task numerical contract on the editor's independent second queue."""
import argparse
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import time
from uuid import uuid4

from SimulationTaskTest import exercise

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("response", ROOT / "scripts/benchmark-simulation-response.py")
response = importlib.util.module_from_spec(spec)
spec.loader.exec_module(response)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    args.gpu, args.ctl, args.gui_validation = True, args.client, "required"
    root = args.work_dir / ("editor-" + uuid4().hex)
    report = {"gpu": True, "timings_ms": {}, "results": []}

    @contextmanager
    def editor_host(args, directory, endpoint):
        with response.host(args, "editor", directory) as (client, process):
            yield client
            start = time.perf_counter_ns()
            client.call("runtime.shutdown")
            assert process.wait(timeout=10) == 0
            report["warm_exit_ms"] = (time.perf_counter_ns() - start) / 1e6

    exercise(args, root, report, editor_host, existing_scene=True)
    output = (root / "stdout.log").read_text(encoding="utf-8")
    report["shared_device"] = "simulation_device=shared queues=2" in output
    (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not report["shared_device"]:
        print("Independent-device fallback passed; no second queue available")
        return 77
    print(f"Editor shared device: exact 300 steps at three sizes, controls, export and warm exit passed: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
