"""CPU example artifacts: exact sample provenance, repeatability and linear/PPM data."""
import json
import math
import pathlib
import subprocess
import sys

exe, work = sys.argv[1], pathlib.Path(sys.argv[2])
outputs = []
for label in ("first", "repeat"):
    destination = work / label
    subprocess.run([exe, str(destination), "8"], check=True, timeout=120)
    report = json.loads((destination / "reference.json").read_text(encoding="utf-8"))
    assert report["schema_version"] == 1 and report["spp"] == 8 and report["first_sample"] == 0
    assert report["seed"] == 42 and report["max_bounces"] == 4 and report["rr_start"] == 3
    assert report["renderer"] == "cpu_lambert_reference_v2" and report["model_version"] == 2 and report["gpu_ms"] is None
    assert report["triangles"] == 12 and report["rays"] <= 24 * 24 * 8 * 9
    assert len(report["linear_rgb"]) == 24 * 24
    assert all(len(p) == 3 and all(math.isfinite(c) and c >= 0 for c in p) for p in report["linear_rgb"])
    assert max(max(p) for p in report["linear_rgb"]) > 0
    ppm = (destination / "reference.ppm").read_bytes()
    assert ppm.startswith(b"P6\n24 24\n255\n") and len(ppm) == len(b"P6\n24 24\n255\n") + 24 * 24 * 3
    report.pop("cpu_seconds")
    outputs.append((report, ppm))
assert outputs[0] == outputs[1]
old = (work / "first" / "reference.ppm").read_bytes()
assert subprocess.run([exe, str(work / "first"), "0"], timeout=30).returncode != 0
assert (work / "first" / "reference.ppm").read_bytes() == old
print("CPU path reference artifacts, repeated samples and invalid-input preservation passed")
