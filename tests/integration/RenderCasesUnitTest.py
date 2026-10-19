"""Pure artifact/manifest checks; no GPU, process or external account."""
import copy
import json
import struct
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from decker.render_cases import camera, compare, run_cases, static_variation, validate_manifest, verify_sequence, verify_submission


def fixture():
    cam = camera(dict(eye=[0, 0, -2], target=[0, 0, 0]))
    sample = dict(camera=cam, time_seconds=0, camera_cut=False)
    options = dict(mode="reference", cloud_shadows=True, shadow_center_m=[0, 0, 0], shadow_extent_m=8192, shadow_resolution=32)
    case = dict(id="reference", preset="sparse", width=1, height=1, exposure=1, render_options=options, warmup_frames=0, samples=[sample])
    manifest = dict(schema_version=1, project="project.json", cases=[case], comparisons=[])
    provenance = dict(environment={"clouds": {"seed": 1729}}, camera=cam, time_seconds=0, exposure=1, profile="unlit_preview",
                      shader_fingerprint=None, render_options=options, warmup_frames=0, sample_count=1, quality="cloud_reference_v1")
    ticket = dict(document_id="30000000-0000-4000-8000-000000000001", scene_id="30000000-0000-4000-8000-000000000002",
                  revision=1, frame=1, width=1, height=1, output="sequence.json", provenance=provenance)
    result = copy.deepcopy(ticket)
    result.update(kind="render_sequence")
    result["provenance"]["shader_fingerprint"] = "fnv1a64:0123456789abcdef"
    frame = dict(index=0, **sample, rgba8=[1, 2, 3, 255], linear_rgba=[[1., 2., 3., 1.]],
                 linear_summary=dict(min=[1, 2, 3], max=[1, 2, 3], mean=[1, 2, 3]), wall_ms=1.2, gpu_ms=None,
                 history=dict(available=False, reset="none", source_sequence=0, candidate_sequence=0), shadow_generation=1)
    report = copy.deepcopy(result)
    report.update(schema_version=1, kind="decker.render_sequence", status="succeeded", frames=[frame],
                  device=dict(name="fixture", driver="fixture"), wall_ms=2, gpu_ms=None)
    return manifest, ticket, result, report


class RenderCasesTests(unittest.TestCase):
    def test_orthographic_sources_match_effective_lens_and_reject_ambiguous_inputs(self):
        raw = dict(eye=[0, 0, -2], target=[0, 0, 0], projection="orthographic", ortho_height=3)
        expected = dict(raw, up=[0, 1, 0], near=.05, far=100, primary_ray_origin="near_plane")
        self.assertEqual(camera(raw), expected)
        for bad in (dict(raw, fov_y=65), dict(raw, ortho_height=0), dict(raw, ortho_height=float("nan")),
                    dict(raw, projection="perspective"), dict(raw, projection="unknown"),
                    dict(raw, primary_ray_origin="eye"), dict(raw, near=0)):
            with self.assertRaises(ValueError):
                camera(bad)
        manifest, ticket, result, report = fixture()
        samples = [dict(camera=raw, time_seconds=0, camera_cut=False)]
        manifest["cases"][0]["samples"] = samples
        validate_manifest(manifest)
        for value in (ticket, result, report):
            value["provenance"]["camera"] = copy.deepcopy(expected)
        report["frames"][0]["camera"] = copy.deepcopy(expected)
        verify_sequence(report, ticket, result, samples)
        for field, value in (("primary_ray_origin", "eye"), ("ortho_height", 4), ("projection", "perspective")):
            bad = copy.deepcopy(report)
            bad["frames"][0]["camera"][field] = value
            with self.assertRaises(ValueError):
                verify_sequence(bad, ticket, result, samples)

    def test_manifest_requires_explicit_bounded_replay_inputs(self):
        manifest, _, _, _ = fixture()
        validate_manifest(manifest)
        for field, value in (("warmup_frames", 1), ("samples", []), ("width", 65536)):
            bad = copy.deepcopy(manifest)
            bad["cases"][0][field] = value
            with self.assertRaises(ValueError):
                validate_manifest(bad)
        bad = copy.deepcopy(manifest)
        bad["cases"].append(copy.deepcopy(bad["cases"][0]))
        with self.assertRaises(ValueError):
            validate_manifest(bad)
        bad = copy.deepcopy(manifest)
        bad["cases"][0]["samples"][0]["time_seconds"] = float("nan")
        with self.assertRaises(ValueError):
            validate_manifest(bad)

    def test_identity_time_missing_pixels_and_summary_tampering_are_rejected(self):
        manifest, ticket, result, report = fixture()
        samples = manifest["cases"][0]["samples"]
        verify_sequence(report, ticket, result, samples)
        for field, value in (("frames", []), ("revision", 2), ("status", "failed")):
            bad = copy.deepcopy(report)
            bad[field] = value
            with self.assertRaises(ValueError):
                verify_sequence(bad, ticket, result, samples)
        for field, value in (("rgba8", []), ("linear_rgba", [[float("inf"), 0, 0, 1]]), ("time_seconds", 1),
                             ("history", {}), ("shadow_generation", -1),
                             ("gpu_ms", 0), ("linear_summary", dict(min=[1, 2, 3], max=[1, 2, 3], mean=[0, 0, 0]))):
            bad = copy.deepcopy(report)
            bad["frames"][0][field] = value
            with self.assertRaises(ValueError):
                verify_sequence(bad, ticket, result, samples)

    def test_submission_rejects_changed_seed_mode_and_scene_identity(self):
        manifest, ticket, _, _ = fixture()
        case = manifest["cases"][0]
        params = dict(case, output=ticket["output"])
        environment = ticket["provenance"]["environment"]
        verify_submission(ticket, params, ticket, environment)
        params["exposure"] = 1.1
        ticket["provenance"]["exposure"] = struct.unpack("<f", struct.pack("<f", 1.1))[0]
        verify_submission(ticket, params, ticket, environment)
        for field in ("environment", "render_options"):
            changed = copy.deepcopy(ticket)
            changed["provenance"][field] = {}
            with self.assertRaises(ValueError):
                verify_submission(changed, params, ticket, environment)
        changed = dict(ticket, revision=2)
        with self.assertRaises(ValueError):
            verify_submission(changed, params, ticket, environment)

    def test_static_variation_requires_matching_camera_time_and_no_cut(self):
        _, _, _, report = fixture()
        self.assertIsNone(static_variation(report)["maximum_nrms"])
        report["frames"].append(copy.deepcopy(report["frames"][0]))
        self.assertEqual(static_variation(report)["maximum_nrms"], 0)
        report["frames"][1]["linear_rgba"][0][0] += 1
        self.assertGreater(static_variation(report)["maximum_nrms"], 0)
        report["frames"][1]["camera_cut"] = True
        self.assertEqual(static_variation(report)["static_pair_count"], 0)

    def test_comparison_requires_matching_physics_and_records_actual_error(self):
        _, _, _, report = fixture()
        self.assertEqual(compare(report, report)["status"], "passed")
        changed = copy.deepcopy(report)
        changed["frames"][0]["linear_rgba"][0][0] = 100
        self.assertEqual(compare(report, changed)["status"], "failed")
        changed = copy.deepcopy(report)
        changed["provenance"]["environment"]["clouds"]["seed"] += 1
        with self.assertRaises(ValueError):
            compare(report, changed)
        changed = copy.deepcopy(report)
        changed["frames"][0]["camera"]["eye"][0] += 1
        with self.assertRaises(ValueError):
            compare(report, changed)

    def test_missing_artifact_remains_failed_without_fabricated_images_or_zero_time(self):
        manifest, ticket, result, _ = fixture()
        class MissingClient:
            def call(self, method, params=None):
                if method == "environment.query":
                    value = dict(state=ticket, environment=ticket["provenance"]["environment"])
                elif method == "environment.preset":
                    value = dict(version=1, environment={})
                elif method == "environment.set":
                    value = dict(state=ticket)
                else:
                    ticket["output"] = params["output"]
                    result["output"] = params["output"]
                    value = dict(ticket, job_id="30000000-0000-4000-8000-000000000003")
                return SimpleNamespace(value=value)
            def wait_job(self, *args, **kwargs):
                return dict(result=result)
        with tempfile.TemporaryDirectory(prefix="dk-render-cases-") as directory:
            root = Path(directory)
            runner = root / "runner.exe"
            runner.write_bytes(b"test binary")
            report = run_cases(MissingClient(), root, manifest, "attempt", runner)
            case = report["cases"][0]
            self.assertEqual(report["status"], "failed")
            self.assertEqual(case["error"]["type"], "FileNotFoundError")
            self.assertIsNone(case["wall_ms"])
            self.assertEqual(case["artifacts"], [])
            self.assertEqual(json.loads((root/"attempt/index.json").read_text())["status"], "failed")


if __name__ == "__main__":
    unittest.main()
