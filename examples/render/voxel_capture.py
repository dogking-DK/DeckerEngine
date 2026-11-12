"""Capture the loaded solid-voxel scene through dk-run or Editor IPC."""
import argparse
import json
from pathlib import Path
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--client", type=Path, required=True, help="Path to dk-ctl.exe")
    parser.add_argument("--output", default="voxel.ppm", help="Project-relative PPM; parent must exist")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--eye", type=float, nargs=3, default=[0, 0, -2])
    parser.add_argument("--target", type=float, nargs=3, default=[0, 0, 0])
    parser.add_argument("--directional", action="store_true")
    parser.add_argument("--unlit-inspection", action="store_true", help="Explicitly omit scene atmosphere/clouds")
    args = parser.parse_args()
    client = Client(args.endpoint, args.client, timeout=60)
    scene = client.call("scene.query").value
    ticket = client.call("render.capture", dict(guard=guard(scene["state"]), output=args.output,
        width=args.width, height=args.height, camera=dict(eye=args.eye, target=args.target, near=.05, far=10000),
        voxel_shading="directional" if args.directional else "unlit", voxel_light_direction=[0, 0, -1],
        unlit_inspection=args.unlit_inspection, validation="required")).value
    while True:
        job = client.call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=1000)).value["job"]
        if job["state"] in ("failed", "cancelled"):
            raise RuntimeError(json.dumps(job, ensure_ascii=False))
        if job["state"] == "succeeded":
            print(json.dumps(job["result"], ensure_ascii=False, indent=2))
            break


if __name__ == "__main__":
    main()
