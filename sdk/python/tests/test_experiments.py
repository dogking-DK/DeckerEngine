import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

from decker.experiments import (ExperimentError, canonical, expand_plan, f32, read_json, topology)
from decker.experiment_index import (METRICS, _open_read, create_index, environment, register_artifacts, verify_index)


def plan():
    return {"format": "DeckerExperimentPlan", "version": 1,
            "base": {"steps": 3, "fixed_dt_ns": 10000000, "cloth": {}},
            "sweep": {}, "seeds": [42], "solvers": ["xpbd_cpu"],
            "output": {"image": False, "width": 16, "height": 12},
            "budget": {"max_experiments": 4096, "max_total_steps": 100000, "max_work_units": 10**10}}


def bundle(root, entry, name="result", run_id=None):
    """Synthetic evidence for failure injection, not claimed as engine execution."""
    target = root / name
    target.mkdir()
    config = entry["config"]
    particle_count, constraint_count = topology(config["cloth"])
    metrics = {k: 0.0 for k in METRICS}
    metrics.update(particle_count=particle_count, constraint_count=constraint_count, color_count=8)
    steps = config["steps"]
    elapsed = steps * config["fixed_dt_ns"]
    names = ["config.json", "metrics.json", "particles.json"]
    values = {"config.json": config,
              "metrics.json": {"steps": steps, "simulated_time_ns": elapsed, "metrics": metrics},
              "particles.json": [{"position": [0, 1, 0], "velocity": [0, 0, 0], "inverse_mass": 1}
                                  for _ in range(particle_count)]}
    if entry["output"]["image"]:
        names.append("image.ppm")
        w, h = config["view"]["width"], config["view"]["height"]
        (target / "image.ppm").write_bytes(f"P6\n{w} {h}\n255\n".encode() + bytes(w*h*3))
    identity = run_id or str(uuid4())
    values["provenance.json"] = {"run_id": identity, "steps": steps, "simulated_time_ns": elapsed,
        "document_id": str(uuid4()), "scene_id": str(uuid4()), "revision": 0, "performance": None,
        "width": config["view"]["width"], "height": config["view"]["height"], "files": names}
    for file, value in values.items():
        (target / file).write_bytes(canonical(value))
    return identity


class PlanTests(unittest.TestCase):
    def test_stable_order_defaults_and_identity(self):
        value = plan()
        value["sweep"] = {"steps": [2, 3], "cloth.iterations": [4, 12]}
        value["solvers"] = ["xpbd_gpu", "xpbd_cpu"]
        value["seeds"] = [43, 42]
        a = expand_plan(value)
        reordered = json.loads(json.dumps(value, sort_keys=True))
        b = expand_plan(reordered)
        self.assertEqual(a, b)
        self.assertEqual(a["totals"]["experiments"], 16)
        self.assertEqual([(e["config"]["cloth"]["iterations"], e["config"]["steps"],
                           e["config"]["solver"], e["config"]["cloth"]["seed"]) for e in a["experiments"]],
                         [(i, n, s, seed) for i in [4, 12] for n in [2, 3]
                          for s in ["xpbd_gpu", "xpbd_cpu"] for seed in [43, 42]])
        # A fresh interpreter must produce identical IDs, unaffected by randomized hash order.
        code = "import json,sys; from decker.experiments import expand_plan; print(json.dumps(expand_plan(json.load(sys.stdin))))"
        result = subprocess.run([sys.executable, "-c", code], input=json.dumps(value),
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(a, json.loads(result.stdout))
        value["budget"]["max_experiments"] = 16
        changed = expand_plan(value)
        self.assertNotEqual(changed["plan_id"], a["plan_id"])
        self.assertEqual(changed["experiments"], a["experiments"])

    def test_seed_steps_output_and_solver_are_identity_inputs(self):
        original = expand_plan(plan())["experiments"][0]["experiment_id"]
        for field in ("seed", "steps", "solver", "output"):
            with self.subTest(field=field):
                value = plan()
                if field == "seed": value["seeds"] = [43]
                if field == "steps": value["base"]["steps"] = 4
                if field == "solver": value["solvers"] = ["xpbd_gpu"]
                if field == "output": value["output"]["image"] = True
                self.assertNotEqual(original, expand_plan(value)["experiments"][0]["experiment_id"])

    def test_float32_defaults_and_boundaries(self):
        value = plan()
        value["base"]["cloth"] = {"spacing": f32(.01), "particle_mass": f32(.001), "compliance": f32(.01)}
        expanded = expand_plan(value)
        self.assertEqual(expand_plan(expanded["plan"]), expanded)
        a = plan()
        a["base"]["cloth"]["damping"] = -0.0
        b = copy.deepcopy(a)
        b["base"]["cloth"]["damping"] = 0
        self.assertEqual(expand_plan(a), expand_plan(b))
        a["sweep"] = {"cloth.spacing": [.15, f32(.15)]}
        with self.assertRaises(ExperimentError): expand_plan(a)

    def test_illegal_combinations_and_unknown_fields(self):
        mutations = [lambda p: p.update(version=True), lambda p: p.update(version=2),
            lambda p: p.update(typo=1), lambda p: p["base"].update(steps=True),
            lambda p: p["base"]["cloth"].update(seed=1), lambda p: p.update(seeds=[]),
            lambda p: p.update(seeds=[1, 1]), lambda p: p.update(solvers=["none"]),
            lambda p: p.update(sweep={"cloth.seed": [1]}), lambda p: p.update(sweep={"bad": [1]}),
            lambda p: p.update(sweep={"cloth.rows": [33]}), lambda p: p.update(sweep={"steps": [-1]}),
            lambda p: p.update(sweep={"fixed_dt_ns": [33333334]}),
            lambda p: p.update(sweep={"cloth.height": [1, 2], "cloth.floor_y": [0, 1]}),
            lambda p: p["base"]["cloth"].update(height=float("nan")),
            lambda p: p.update(output={"image": 1, "width": 16, "height": 12})]
        for mutation in mutations:
            value = plan()
            mutation(value)
            with self.subTest(value=value), self.assertRaises(ExperimentError): expand_plan(value)

    def test_budgets_reject_before_output_or_cartesian_materialization(self):
        value = plan()
        value["sweep"] = {"steps": list(range(4096))}
        value["seeds"] = list(range(4096))
        with patch("decker.experiments.itertools.product", side_effect=AssertionError("expanded")):
            with self.assertRaises(ExperimentError): expand_plan(value)
        expanded = expand_plan(plan())
        for key, amount in (("max_total_steps", 2), ("max_work_units", expanded["totals"]["work_units"]-1)):
            with tempfile.TemporaryDirectory() as temp:
                value = plan()
                value["budget"][key] = amount
                target = Path(temp) / "rejected"
                with self.assertRaises(ExperimentError): create_index(target, value)
                self.assertFalse(target.exists())
        value = plan()
        value["budget"].update(max_total_steps=3, max_work_units=expanded["totals"]["work_units"])
        self.assertEqual(expand_plan(value)["totals"], expanded["totals"])

    def test_json_limits_and_duplicate_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / "input.json"
            for text in ('{"version":1,"version":2}', '{"value":NaN}', '{"value":1e999}', '['*30+'0'+']'*30):
                file.write_text(text)
                with self.assertRaises(ExperimentError): read_json(file)
            file.write_text(" " * 20)
            with self.assertRaises(ExperimentError): read_json(file, 10)


class IndexTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "index"
        self.value = plan()
        self.index = create_index(self.root, self.value)
        self.entry = self.index["experiments"][0]
        self.identity = self.entry["experiment_id"]
        self.env = environment(runtime={"simulation": {"solver": "xpbd_cpu"}})

    def register(self, run_id, name="result"):
        return register_artifacts(self.root, self.identity, name, expected_run_id=run_id,
                                  execution_environment=self.env)

    def test_roundtrip_multiple_attempts_preserve_old_evidence(self):
        before = verify_index(self.root)
        self.assertEqual(before["experiments"][0]["status"], "planned")
        run_id = bundle(self.root, self.entry)
        a = self.register(run_id)
        b = self.register(bundle(self.root, self.entry, "second"), "second")
        checked = verify_index(self.root)
        self.assertEqual(checked["experiments"][0]["attempts"], [a, b])
        self.assertNotEqual(a["attempt_id"], b["attempt_id"])
        self.assertEqual(a["environment"], self.env)
        self.assertEqual(a["source"]["run_id"], run_id)
        content = (self.root / "index.json").read_bytes()
        with self.assertRaises(ExperimentError): self.register(run_id)
        with self.assertRaises(ExperimentError): create_index(self.root, self.value)
        self.assertEqual((self.root / "index.json").read_bytes(), content)

    def test_open_snapshot_reader_serializes_with_atomic_registration(self):
        identity = bundle(self.root, self.entry)
        old = (self.root / "index.json").read_bytes()
        entered, errors = threading.Event(), []
        def register():
            entered.set()
            try:
                self.register(identity)
            except Exception as error:
                errors.append(error)
        with _open_read(self.root / "index.json") as reader:
            writer = threading.Thread(target=register)
            writer.start()
            self.assertTrue(entered.wait(2))
            self.assertEqual(reader.read(), old)
        writer.join(timeout=10)
        self.assertFalse(writer.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(verify_index(self.root)["experiments"][0]["status"], "verified")

    def test_bad_source_config_steps_and_missing_outputs_preserve_index(self):
        identity = bundle(self.root, self.entry)
        original = (self.root / "index.json").read_bytes()
        for filename, mutate in [
            ("config.json", lambda v: v["cloth"].update(seed=7)),
            ("provenance.json", lambda v: v.update(run_id=str(uuid4()))),
            ("provenance.json", lambda v: v.update(steps=4)),
            ("metrics.json", lambda v: v.update(simulated_time_ns=0)),
            ("particles.json", lambda v: v.pop()),
            ("metrics.json", lambda v: v["metrics"].update(particle_count=True))]:
            path = self.root / "result" / filename
            saved = path.read_bytes()
            value = json.loads(saved)
            mutate(value)
            path.write_text(json.dumps(value))
            with self.subTest(filename=filename), self.assertRaises(ExperimentError): self.register(identity)
            self.assertEqual(original, (self.root / "index.json").read_bytes())
            path.write_bytes(saved)
        (self.root / "result/metrics.json").unlink()
        with self.assertRaises(ExperimentError): self.register(identity)
        self.assertEqual(original, (self.root / "index.json").read_bytes())

    def test_reverification_detects_tampering_cached_ids_and_digests(self):
        identity = bundle(self.root, self.entry)
        self.register(identity)
        saved = (self.root / "index.json").read_bytes()
        for field, value in (("plan_id", "0"*64), ("version", 2), ("version", True)):
            edited = json.loads(saved)
            edited[field] = value
            (self.root / "index.json").write_text(json.dumps(edited))
            with self.assertRaises(ExperimentError): verify_index(self.root)
        (self.root / "index.json").write_bytes(saved)
        particles = self.root / "result/particles.json"
        particles.write_bytes(particles.read_bytes() + b" ")
        with self.assertRaises(ExperimentError): verify_index(self.root)
        new_id = bundle(self.root, self.entry, "second")
        with self.assertRaises(ExperimentError): self.register(new_id, "second")
        self.assertEqual(saved, (self.root / "index.json").read_bytes())

    def test_publication_failure_and_lock_preserve_index(self):
        identity = bundle(self.root, self.entry)
        before = (self.root / "index.json").read_bytes()
        with patch("decker.experiment_index.os.replace", side_effect=OSError("injected failure")):
            with self.assertRaises(ExperimentError): self.register(identity)
        self.assertEqual(before, (self.root / "index.json").read_bytes())
        self.assertFalse(list(self.root.glob(".index*")))
        lock = self.root / ".index.lock"
        lock.write_text("other writer")
        with self.assertRaises(ExperimentError): self.register(identity)
        self.assertEqual(lock.read_text(), "other writer")
        lock.unlink()
        self.register(identity)

    def test_new_index_publication_failure_cleans_only_own_directory(self):
        new = self.root.parent / "new"
        with patch("decker.experiment_index.os.replace", side_effect=OSError("injected failure")):
            with self.assertRaises(ExperimentError): create_index(new, plan())
        self.assertFalse(new.exists())
        self.assertTrue((self.root / "index.json").is_file())

    def test_paths_cannot_escape_and_reparse_points_are_rejected(self):
        identity = bundle(self.root, self.entry)
        for path in ("../result", "result/../result", "result\\x", "/result", "C:/result", ".decker", "result.", "result:stream"):
            with self.subTest(path=path), self.assertRaises(ExperimentError): self.register(identity, path)
        # Windows junction/symlink detection is tested without requiring symlink privileges.
        with patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaises(ExperimentError): verify_index(self.root)

    def test_image_truncation_and_dimensions(self):
        p = plan()
        p["output"]["image"] = True
        root = self.root.parent / "images"
        index = create_index(root, p)
        entry = index["experiments"][0]
        identity = bundle(root, entry)
        image = root / "result/image.ppm"
        saved = image.read_bytes()
        for invalid in (saved[:-1], saved.replace(b"16 12", b"12 16")):
            image.write_bytes(invalid)
            with self.assertRaises(ExperimentError):
                register_artifacts(root, entry["experiment_id"], "result", expected_run_id=identity,
                                   execution_environment=self.env)
        image.write_bytes(saved)
        register_artifacts(root, entry["experiment_id"], "result", expected_run_id=identity, execution_environment=self.env)
        verify_index(root)


if __name__ == "__main__":
    unittest.main()
