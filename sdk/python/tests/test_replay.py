import copy
import json
from pathlib import Path
import random
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from decker import CallTimeout
from decker.replay import Recorder, _snapshot, replay
from decker.replay_format import (
    ReplayError, atomic_record, canonical, digest, inventory, load_recording, validate,
)

DOC = "10000000-0000-4000-8000-000000000001"
SCENE = "20000000-0000-4000-8000-000000000001"


def seal(record):
    record["sha256"] = digest({k: v for k, v in record.items() if k != "sha256"})
    return record


def empty_record():
    snapshot = {"state": {"scene_id": SCENE, "revision": 0, "dirty": False, "entity_count": 0}, "entities": []}
    stamp = {"size": 0, "sha256": "0" * 64}
    return seal({"format": "DeckerRecording", "version": 1,
                 "header": {"sdk_version": 1, "protocol": 1, "capabilities": {}, "commands_sha256": "0"*64,
                            "engine_build": "test", "seed": 2**64-1, "context": {}, "environment": {},
                            "root": "source", "manifest": "project.json", "scene": "scene.json",
                            "inputs": {"project.json": dict(stamp), "scene.json": dict(stamp)}, "document_id": DOC},
                 "initial": snapshot, "steps": [], "final": copy.deepcopy(snapshot)})


class ReplayTests(unittest.TestCase):
    def test_codec_version_checksum_and_unsupported_steps_before_calls(self):
        valid = empty_record()
        self.assertEqual(validate(valid), valid)
        mutations = []
        bad = copy.deepcopy(valid); bad["version"] = 2; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["header"]["seed"] = True; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["header"]["new"] = 0; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["header"]["context"]["big"] = 2**64; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["initial"]["state"]["future_field"] = 1; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["initial"]["state"]["entity_count"] = 1; mutations.append(seal(bad))
        bad = copy.deepcopy(valid); bad["sha256"] = "a"*64; mutations.append(bad)
        bad = copy.deepcopy(valid)
        bad["steps"] = [{"kind": "call", "method": "runtime.shutdown", "params": {},
                          "outcome": {"kind": "success", "value": {}}, "after": digest(bad["final"]), "writes": {}}]
        mutations.append(seal(bad))
        with tempfile.TemporaryDirectory() as directory:
            for record in mutations:
                client = Mock()
                with self.assertRaises(ReplayError):
                    replay(client, record, directory, engine_build="test")
                self.assertEqual(client.mock_calls, [])
            path = Path(directory) / "record.json"
            path.write_bytes(canonical(valid))
            self.assertEqual(load_recording(path), valid)
            path.write_bytes(b'{"format":1,"format":2}')
            with self.assertRaises(ReplayError):
                load_recording(path)
            path.write_bytes(canonical(valid)[:-1])
            with self.assertRaises(ReplayError):
                load_recording(path)

    def test_atomic_publish_failure_preserves_prior_record_and_cleans_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            path.write_bytes(b"old")
            with patch("decker.replay_format.os.replace", side_effect=OSError("injected replacement failure")):
                with self.assertRaises(OSError):
                    atomic_record(path, empty_record())
            self.assertEqual(path.read_bytes(), b"old")
            self.assertEqual(list(Path(directory).iterdir()), [path])
            atomic_record(path, empty_record())
            self.assertEqual(load_recording(path)["version"], 1)

    def test_all_input_bytes_including_external_asset_dependencies_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "assets").mkdir(); (root / ".decker").mkdir()
            (root / "assets/texture.bin").write_bytes(b"original")
            (root / ".decker/cache").write_bytes(b"ignored")
            original = inventory(root)
            self.assertEqual(set(original), {"assets/texture.bin"})
            (root / "assets/texture.bin").write_bytes(b"modified")
            self.assertNotEqual(inventory(root), original)
            (root / ".decker/asset-operations").mkdir()
            (root / ".decker/asset-operations/pending.json").write_text("{}")
            with self.assertRaises(ReplayError):
                inventory(root)
        for name in ("../outside", "/outside", "C:/outside", "a\\b", "a//b", ".decker/cache"):
            record = empty_record()
            record["header"]["inputs"][name] = {"size": 0, "sha256": "0"*64}
            with self.assertRaises(ReplayError):
                validate(seal(record))

    def test_pagination_is_complete_and_detects_concurrent_changes(self):
        state = dict(empty_record()["initial"]["state"], document_id=DOC, entity_count=257)

        def entity(index):
            return {"id": str(index), "name": "test", "transform": {}, "parent": None, "assets": [], "world_matrix": []}

        pages = [SimpleNamespace(value={"state": state, "offset": 0, "entities": [entity(i) for i in range(256)], "has_more": True}),
                 SimpleNamespace(value={"state": state, "offset": 256, "entities": [entity(256)], "has_more": False})]
        client = Mock(timeout=5)
        client.call.side_effect = pages
        _, logical = _snapshot(client, time.monotonic()+5)
        self.assertEqual(len(logical["entities"]), 257)
        self.assertNotIn("document_id", logical["state"])
        self.assertNotIn("world_matrix", logical["entities"][0])
        pages[1].value["state"] = dict(state, revision=1)
        client.call.side_effect = pages
        with self.assertRaises(ReplayError):
            _snapshot(client, time.monotonic()+5)

    def test_seed_materializes_ids_without_changing_explicit_ids_or_global_rng(self):
        first, second = Recorder(), Recorder()
        first.rng = random.Random(42); second.rng = random.Random(42)
        global_state = random.getstate()
        params = {"commands": [{"method": "entity.create", "params": {}},
                                {"method": "entity.create", "params": {"id": DOC}}]}
        one = first._materialize("scene.transaction", params)
        self.assertEqual(one, second._materialize("scene.transaction", params))
        self.assertEqual(one["commands"][1]["params"]["id"], DOC)
        self.assertNotIn("id", params["commands"][0]["params"])
        self.assertEqual(random.getstate(), global_state)

    def test_unknown_execution_poison_prevents_more_edits_or_complete_record(self):
        recorder = Recorder()
        recorder.client, recorder.root, recorder.rng = Mock(timeout=5), Path("."), random.Random(1)
        record = empty_record()
        recorder.header, recorder.initial, recorder.final = record["header"], record["initial"], record["final"]
        recorder.steps, recorder._files, recorder._broken, recorder._closed = [], {}, False, False
        with patch("decker.replay._check_files"), patch("decker.replay._check_state"), \
                patch("decker.replay._execute", side_effect=CallTimeout("unknown", status="timeout", execution="unknown")) as execute:
            with self.assertRaises(CallTimeout):
                recorder.call("entity.create", {"guard": {"document_id": DOC, "revision": 0}})
            with self.assertRaises(ReplayError):
                recorder.call("scene.save")
            with self.assertRaises(ReplayError):
                recorder.save("record.json")
            self.assertEqual(execute.call_count, 1)

    def test_replay_deadline_and_fingerprint_failure_do_not_load_or_edit(self):
        record = empty_record()
        with tempfile.TemporaryDirectory() as directory:
            client = Mock(timeout=5)
            with self.assertRaises(ReplayError) as caught:
                replay(client, record, directory, engine_build="test")
            self.assertEqual(caught.exception.completed, 0)
            self.assertEqual(client.mock_calls, [])
            with patch("decker.replay._check_files"), patch("decker.replay.time.monotonic", side_effect=[0, 10]):
                with self.assertRaises(ReplayError):
                    replay(client, record, directory, engine_build="test", timeout=1)
            self.assertEqual(client.mock_calls, [])


if __name__ == "__main__":
    unittest.main()
