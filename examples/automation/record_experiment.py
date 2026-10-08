"""Record seeded scene edits and optional capture on a fresh, exclusive pipe host."""

import argparse
import json

from decker import Client, Command, guard
from decker.replay import Recorder


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--recording", required=True)
    parser.add_argument("--engine-build", required=True)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    recorder = Recorder.start(Client(args.pipe, args.ctl), args.project_root, seed=args.seed,
                              engine_build=args.engine_build, context={"experiment": "seeded translation", "units": "metres"})
    query = recorder.call("scene.query").value
    state = query["state"]
    if query["entities"]:
        entity = query["entities"][0]["id"]
    else:
        created = recorder.call("entity.create", {"guard": guard(state)}).value
        entity, state = created["created_id"], created["state"]
    state = recorder.transaction(guard(state), [
        Command("entity.set_name", {"id": entity, "name": f"seed={args.seed}"}),
        Command("entity.set_transform", {"id": entity, "transform": {
            "translation": [recorder.rng.uniform(-0.1, 0.1), -0.125, 0.25],
            "rotation": [0, 0, 0, 1], "scale": [0.5, 0.5, 1]}}),
    ]).value["state"]
    if args.capture:
        recorder.capture({"guard": guard(state), "output": "experiment.ppm", "width": 128, "height": 128,
                          "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60}})
    recorder.call("scene.save", {"guard": guard(state)})
    path = recorder.save(args.recording)
    print(json.dumps({"recording": str(path), "seed": args.seed}, ensure_ascii=True))


if __name__ == "__main__":
    main()
