"""Set an Editor view's absolute spp/ReSTIR and orbit, then observe its completed image.

Each write carries a guard explicitly obtained from a query or successful receipt.
Writes are sequential, not a transaction: an earlier accepted change survives a
later failure. No write retry, automatic guard refresh, scene save, or shutdown.
"""
import argparse
import json
import math
import sys
import time
from decker import Client, ClientError


def configure(client, target_spp, yaw_deg, pitch_deg, timeout, accepted):
    if type(target_spp) is not int or not 1 <= target_spp <= 4294967295:
        raise ValueError("target-spp must be an integer in 1..4294967295")
    if not all(math.isfinite(v) for v in (yaw_deg, pitch_deg, timeout)) or not 0 < timeout <= 120:
        raise ValueError("angles must be finite; timeout must be in (0,120] seconds")
    if not -360 <= yaw_deg <= 360 or not -180 <= pitch_deg <= 180:
        raise ValueError("orbit yaw must be in [-360,360], pitch in [-180,180] degrees")
    deadline = time.monotonic() + timeout

    def call(method, params=None):
        remaining = deadline - time.monotonic()
        if remaining < .001:
            raise TimeoutError("Observation deadline reached; accepted changes remain applied")
        return client.call(method, params, timeout=min(10, remaining)).value

    required = {"editor.viewport.query", "editor.viewport.set", "editor.camera.query", "editor.camera.orbit"}
    names = {entry["name"] for entry in call("commands.list")}
    if not required <= names:
        raise RuntimeError("Connect to an Editor host; ordinary dk-run has no editor.* commands")
    view, camera = call("editor.viewport.query"), call("editor.camera.query")
    if view["guard"] != camera["guard"]:
        raise RuntimeError("Editor changed during preflight; inspect it before running again")
    if view["blocked_reason"] or camera["blocked_reason"]:
        raise RuntimeError("Editor is blocked: " + (view["blocked_reason"] or camera["blocked_reason"]))
    if not view["capabilities"]["pathtracing"]:
        raise RuntimeError("Path tracing unavailable: " + view["capabilities"]["unavailable"])
    if camera["settings"]["navigation_mode"] != "orbit" or view["settings"]["paused"]:
        raise RuntimeError("Select Orbit and resume sampling before running this example")
    for method in ("editor.viewport.set", "editor.camera.orbit"):
        description = call("commands.describe", {"name": method})
        if description["effect"] != "control" or description["undoable"]:
            raise RuntimeError("Unexpected Editor command contract: " + method)

    view = call("editor.viewport.set", {"guard": camera["guard"], "renderer": "pathtrace",
                "direct_lighting": "restir_di", "target_spp": target_spp})
    accepted.append({"method": "editor.viewport.set", "guard": view["guard"]})
    camera = call("editor.camera.orbit", {"guard": view["guard"], "yaw_deg": yaw_deg, "pitch_deg": pitch_deg})
    accepted.append({"method": "editor.camera.orbit", "guard": camera["guard"]})
    expected = camera["guard"]
    while True:
        camera = call("editor.camera.query")
        if camera["guard"] != expected:
            raise RuntimeError("Editor changed while observing; no guard refresh or write retry was attempted")
        shown = camera["presentation"]
        # A receipt can still carry the previous frame's failure while the owner
        # has not observed these newly accepted settings. Only judge our observation.
        if shown["observed_guard"] == expected and (shown["error"] or shown["phase"] == "Failed"):
            raise RuntimeError("Rendering failed: " + shown["error"])
        if (shown["has_image"] and shown["current"] and shown["renderer"] == "pathtrace"
                and shown["document_id"] == expected["document_id"]
                and shown["camera_revision"] == camera["render_revision"]
                and shown["target_spp"] == target_spp and shown["spp"] >= target_spp):
            return {"status": "passed", "accepted": accepted, "camera": camera}
        time.sleep(min(.05, max(0, deadline - time.monotonic())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipe", required=True)
    parser.add_argument("--ctl", default="dk-ctl")
    parser.add_argument("--target-spp", type=int, default=12)
    parser.add_argument("--yaw-deg", type=float, default=15)
    parser.add_argument("--pitch-deg", type=float, default=20)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    accepted = []
    try:
        result = configure(Client(args.pipe, args.ctl), args.target_spp, args.yaw_deg, args.pitch_deg, args.timeout, accepted)
    except (ClientError, ValueError, RuntimeError, TimeoutError) as error:
        print(json.dumps({"status": "failed", "accepted": accepted, "error": str(error)}, ensure_ascii=True), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
