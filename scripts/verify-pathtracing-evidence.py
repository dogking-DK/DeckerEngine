"""Replay archived M16.11 quality/performance gates and check original byte hashes (no GPU)."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location("path_benchmark", Path(__file__).with_name("benchmark-pathtracing.py"))
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def verify(directory):
    archive = json.loads((directory/"archive.json").read_text(encoding="utf-8"))
    expected = {"manifest.json", "summary.json", "quality.json.gz", *[f"round-{i}.json.gz" for i in range(1, 4)]}
    benchmark.need(archive["schema"] == 1 and set(archive["files"]) == expected, "Incomplete archive")
    contents = {}
    for name, entry in archive["files"].items():
        data = (directory/name).read_bytes()
        benchmark.need(hashlib.sha256(data).hexdigest() == entry["sha256"], f"Changed archive: {name}")
        raw = gzip.decompress(data) if name.endswith(".gz") else data
        benchmark.need(len(raw) == entry["raw_bytes"] and hashlib.sha256(raw).hexdigest() == entry["raw_sha256"], f"Changed raw bytes: {name}")
        contents[name.removesuffix(".gz")] = json.loads(raw)
    manifest, saved = contents["manifest.json"], contents["summary.json"]
    benchmark.need(manifest["schema"] == 1 and manifest["status"] == "passed", "Failed manifest")
    names = ["quality", "round-1", "round-2", "round-3"]
    benchmark.need([r["name"] for r in manifest["runs"]] == names, "Missing/duplicated round")
    for row in manifest["runs"]:
        name = row["name"]+".json"
        benchmark.need(row["status"] == "passed" and row["raw"] == name
                       and row["sha256"] == archive["files"][name+".gz"]["raw_sha256"], "Manifest/raw hash mismatch")
    original = contents["quality.json"]
    reference = benchmark.quality(original)
    aggregate = dict(schema=1, status="passed", quality=reference, rounds=[])
    for i in range(1, 4):
        data = contents[f"round-{i}.json"]
        benchmark.need(data["device"] == original["device"] and [c["input"] for c in data["cases"]]
                       == [c["input"] for c in original["cases"]], "Changed device/input")
        aggregate["rounds"].append(dict(round=i, device_queue_ms=data["device_queue_ms"], compile_pipeline_ms=data["compile_pipeline_ms"],
                                         cases=benchmark.performance(data, reference)))
    benchmark.need(aggregate == saved, "Summary does not reproduce from raw measurements")
    return aggregate


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = verify(args.directory)
    print(f"passed: {len(result['quality'])} quality fixtures, {len(result['rounds'])} full performance rounds")
