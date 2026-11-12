"""Capture the loaded solid-voxel scene through dk-run or Editor IPC."""
import argparse
import json
from pathlib import Path
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--client", type=Path, required=True, help="Path to dk-ctl.exe")
    parser.add_argument("--output", default="voxel.ppm", help="Project-relative PPM or path-traced linear JSON; parent must exist")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--eye", type=float, nargs=3, default=[0, 0, -2])
    parser.add_argument("--target", type=float, nargs=3, default=[0, 0, 0])
    parser.add_argument("--directional", action="store_true")
    parser.add_argument("--unlit-inspection", action="store_true", help="Explicitly omit scene atmosphere/clouds")
    parser.add_argument("--pathtrace", action="store_true")
    parser.add_argument("--media", action="store_true", help="Include Scene air/clouds in the path integrator")
    parser.add_argument("--restir", choices=("initial", "temporal", "spatial", "joint"))
    parser.add_argument("--spp", type=int, default=3)
    parser.add_argument("--orthographic", action="store_true")
    args = parser.parse_args()
    if (args.media or args.restir) and not args.pathtrace:
        parser.error("--media/--restir require --pathtrace")
    if args.pathtrace and (args.directional or args.unlit_inspection):
        parser.error("Path tracing uses palette materials and --media; raster display options cannot be combined")
    client = Client(args.endpoint, args.client, timeout=60)
    scene = client.call("scene.query").value
    camera = dict(eye=args.eye, target=args.target, near=.05, far=10000)
    if args.orthographic:
        camera.update(projection="orthographic", ortho_height=3)
    params = dict(guard=guard(scene["state"]), output=args.output, width=args.width, height=args.height,
                  camera=camera, validation="required")
    if args.pathtrace:
        path = dict(version=3, medium_mode="scene_media" if args.media else "surface_only", target_spp=args.spp,
                    direct_lighting="restir_di" if args.restir else "classic_mis", max_bounces=2, rr_start=2, seed=7,
                    environment_rgb=[.3, .4, .6], sun=False)
        if args.restir:
            path["restir"] = dict(initial_candidates=8)
            if args.restir in ("temporal", "joint"): path["restir"]["temporal"] = {}
            if args.restir in ("spatial", "joint"): path["restir"]["spatial"] = {}
        params.update(renderer="pathtrace", format="linear_json" if args.output.endswith(".json") else "ppm", pathtrace=path)
    else:
        params.update(voxel_shading="directional" if args.directional else "unlit", voxel_light_direction=[0, 0, -1], unlit_inspection=args.unlit_inspection)
    ticket = client.call("render.capture", params).value
    while True:
        job = client.call("jobs.wait", dict(id=ticket["job_id"], timeout_ms=1000)).value["job"]
        if job["state"] in ("failed", "cancelled"):
            raise RuntimeError(json.dumps(job, ensure_ascii=False))
        if job["state"] == "succeeded":
            print(json.dumps(job["result"], ensure_ascii=False, indent=2))
            break


if __name__ == "__main__":
    main()
