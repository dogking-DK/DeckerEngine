"""Deterministic observation/failure boundaries of the runnable Editor example."""
import copy
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("editor_settings_example", Path(__file__).resolve().parents[2] / "examples/automation/editor_settings.py")
example = importlib.util.module_from_spec(spec)
spec.loader.exec_module(example)


def guard(revision):
    return dict(view_id="view", document_id="document", revision=revision)


def observed(*, revision=3, observed_revision=3, error="", current=True):
    return dict(guard=guard(revision), render_revision=3, presentation=dict(observed_guard=guard(observed_revision),
        error=error, phase="Failed" if error else "Complete", current=current, has_image=True, renderer="pathtrace",
        document_id="document", camera_revision=3, target_spp=2, spp=2))


class ScriptedClient:
    def __init__(self, observations, fail_orbit=False, expire=False):
        self.observations = list(observations)
        self.fail_orbit, self.expire = fail_orbit, expire
        self.calls, self.writes, self.now = [], [], 0

    def call(self, method, params=None, **_):
        self.calls.append(method)
        if method == "commands.list":
            value = [dict(name=name) for name in ("editor.viewport.query", "editor.viewport.set", "editor.camera.query", "editor.camera.orbit")]
        elif method == "commands.describe":
            value = dict(effect="control", undoable=False)
        elif method == "editor.viewport.query":
            value = dict(guard=guard(1), blocked_reason="", settings=dict(paused=False), capabilities=dict(pathtracing=True))
        elif method == "editor.camera.query":
            if not self.writes:
                value = dict(guard=guard(1), blocked_reason="", settings=dict(navigation_mode="orbit"))
            else:
                if not self.observations:
                    raise AssertionError("Unexpected poll/retry after supplied observations")
                value = self.observations.pop(0)
                if self.expire:
                    self.now = 2
        else:
            self.writes.append((method, copy.deepcopy(params)))
            if method == "editor.viewport.set":
                assert params["guard"] == guard(1)
                value = dict(guard=guard(2))
            elif method == "editor.camera.orbit":
                assert params["guard"] == guard(2)
                if self.fail_orbit:
                    raise RuntimeError("Orbit rejected after viewport acceptance")
                value = dict(guard=guard(3))
            else:
                raise AssertionError("Unexpected method: " + method)
        return SimpleNamespace(value=copy.deepcopy(value))


class ObservationTests(unittest.TestCase):
    def run_example(self, client, accepted):
        with patch.object(example.time, "monotonic", side_effect=lambda: client.now), patch.object(example.time, "sleep"):
            return example.configure(client, 2, 5, 2, 1, accepted)

    def test_old_failure_waits_for_matching_observation(self):
        client = ScriptedClient([observed(observed_revision=2, error="old frame failure", current=False), observed()])
        accepted = []
        self.assertEqual(self.run_example(client, accepted)["status"], "passed")
        self.assertEqual(len(client.writes), 2)
        self.assertEqual(len(accepted), 2)

    def test_current_failure_retains_receipts_without_retry(self):
        client = ScriptedClient([observed(error="GPU rejected work", current=False)])
        accepted = []
        with self.assertRaisesRegex(RuntimeError, "Rendering failed"):
            self.run_example(client, accepted)
        self.assertEqual(len(accepted), 2)
        self.assertEqual(len(client.writes), 2)

    def test_concurrent_guard_does_not_refresh_or_write_again(self):
        client = ScriptedClient([observed(revision=4, observed_revision=4)])
        with self.assertRaisesRegex(RuntimeError, "changed while observing"):
            self.run_example(client, [])
        self.assertEqual(len(client.writes), 2)

    def test_timeout_is_bounded_after_acceptance(self):
        client = ScriptedClient([observed(current=False)], expire=True)
        accepted = []
        with self.assertRaisesRegex(TimeoutError, "deadline"):
            self.run_example(client, accepted)
        self.assertEqual(len(client.writes), 2)
        self.assertEqual(len(accepted), 2)

    def test_second_write_failure_does_not_rollback_first(self):
        client = ScriptedClient([], fail_orbit=True)
        accepted = []
        with self.assertRaisesRegex(RuntimeError, "Orbit rejected"):
            self.run_example(client, accepted)
        self.assertEqual(accepted, [dict(method="editor.viewport.set", guard=guard(2))])
        self.assertEqual([name for name, _ in client.writes], ["editor.viewport.set", "editor.camera.orbit"])


if __name__ == "__main__":
    unittest.main()
