"""Apply a fixed cloud preset and render a reproducible temporal sequence via IPC."""
import argparse
import json
from decker import Client, guard

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--pipe", required=True)
parser.add_argument("--ctl", default="dk-ctl")
args = parser.parse_args()
client = Client(args.pipe, args.ctl, timeout=30)
query = client.call("environment.query").value
environment = client.call("environment.preset", {"name": "sparse"}).value["environment"]
environment["atmosphere"]["planet_center_m"][1] -= 500
environment["sun"]["direction"] = [.2, .8, -.3]
environment["clouds"].update(weather_resolution=16, base_resolution=16, detail_resolution=8, coverage=.85, extinction_per_m=.0003)
state = client.call("environment.set", dict(guard=guard(query["state"]), environment=environment)).value["state"]
camera = dict(eye=[0, 0, -2], target=[0, 0, 0], up=[0, 1, 0], fov_y=65, near=1, far=10000)
ticket = client.call("render.sequence", dict(guard=guard(state), output="cloud-python.json", width=16, height=16,
                    render_options=dict(mode="temporal", cloud_shadows=True, shadow_resolution=32), warmup_frames=2,
                    samples=[dict(camera=camera, time_seconds=t, camera_cut=False) for t in (0, .03)], validation="required")).value
job = client.wait_job(ticket["job_id"], timeout=150)
print(json.dumps(job))
