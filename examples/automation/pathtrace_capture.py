"""Capture the active small scene from sample zero to 3 spp through the Python client."""
import argparse
import json
from decker import Client, guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--media", action="store_true", help="Trace scene air/clouds at fixed time zero (v2)")
    args = parser.parse_args()
    client = Client(args.pipe, args.ctl, timeout=30)
    state = client.call("scene.query").value["state"]
    path = dict(version=1, target_spp=3, seed=7, max_bounces=4, environment_rgb=[.3, .4, .6])
    if args.media:
        path.update(version=2, medium_mode="scene_media", time_seconds=0, cloud_albedo=.9, cloud_anisotropy=.6)
    capture = client.capture({"guard": guard(state), "renderer": "pathtrace", "format": "linear_json",
                              "output": "path-python.json", "width": 48, "height": 32, "validation": "required",
                              "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "near": .05, "far": 10000},
                              "pathtrace": path}, timeout=150)
    assert capture.job["progress"]["completed"] == 3
    print(json.dumps(capture.job, ensure_ascii=True))


if __name__ == "__main__":
    main()
