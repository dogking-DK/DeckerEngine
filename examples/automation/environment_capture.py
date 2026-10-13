"""Enable a reproducible atmosphere on the render-disk fixture and capture it via IPC."""
import argparse
import json

from decker import Client, guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--project-root", required=True)
    args = parser.parse_args()
    client = Client(args.pipe, args.ctl, timeout=30)
    query = client.call("environment.query").value
    environment = query["environment"]
    environment["enabled"] = True
    environment["atmosphere"]["planet_center_m"] = [0, -environment["atmosphere"]["ground_radius_m"]-1000, 0]
    environment["sun"]["direction"] = [0, .2, 1]
    environment["sun"]["irradiance"] = [4, 4, 4]
    state = client.call("environment.set", {"guard": guard(query["state"]), "environment": environment}).value["state"]
    capture = client.capture({"guard": guard(state), "output": "environment-python.ppm", "width": 128, "height": 96,
                              "camera": {"eye": [0, 0, 4], "target": [0, 0, 0], "fov_y": 65, "near": 1, "far": 10000},
                              "exposure": 1, "profile": "unlit_preview", "validation": "required"}, timeout=120)
    capture.collect(args.project_root)
    print(json.dumps(capture.job, ensure_ascii=True))


if __name__ == "__main__":
    main()
