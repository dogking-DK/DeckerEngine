"""Edit up to eight entities in an existing scene, wait for capture, collect metadata."""

import argparse
from dataclasses import asdict
import json
from uuid import uuid4

from decker import Client, Command, guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    client = Client(args.pipe, args.ctl)
    query = client.call("scene.query", {"limit": 8}).value
    commands = [Command("entity.set_name", {"id": entity["id"], "name": f"Python 实验 {index}"})
                for index, entity in enumerate(query["entities"])]
    state = query["state"]
    if commands:
        state = client.transaction(guard(state), commands).value["state"]
    capture = client.capture({"guard": guard(state), "output": f"python-{uuid4().hex}.ppm",
                              "width": 256, "height": 256,
                              "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60}},
                             timeout=args.timeout)
    artifact = capture.collect(args.project_root)
    print(json.dumps({"capture": capture.job, "artifact": asdict(artifact)}, default=str, ensure_ascii=True))


if __name__ == "__main__":
    main()
