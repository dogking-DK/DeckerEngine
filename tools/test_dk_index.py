"""Regression tests for the rotating index history. Uses temporary files only."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import dk_index


class IndexHistoryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.history = self.root / "history.md"
        self.history.write_text("# Test\n\n| tool | time | mode | version | head | state |\n"
                                "| --- | --- | --- | --- | --- | --- |\n", encoding="utf-8")
        self.enterContext(patch.object(dk_index, "ROOT", self.root))
        self.enterContext(patch.object(dk_index, "HISTORY", self.history))

    def test_each_tool_keeps_three_latest_updates(self):
        for number in range(5):
            for tool in dk_index.NAMES:
                dk_index.record(dict(tool=tool, version="test", operation="sync"), f"hash{number}", "干净")
        rows = dk_index.history_rows(self.history.read_text(encoding="utf-8"))
        for tool in dk_index.NAMES:
            self.assertEqual([r.split("|")[-3].strip() for r in rows if r.startswith(f"| {tool} |")],
                             ["hash4", "hash3", "hash2"])

    def test_failed_lookup_does_not_discard_other_verified_update(self):
        good = dict(tool="codegraph", version="test", operation="增量同步")
        with patch.object(dk_index, "run", return_value="abc"), patch.object(
                dk_index, "operate", side_effect=[good, ValueError("wrong project")]):
            results = dk_index.index("update", "all")
        self.assertIn("error", results[1])
        rows = dk_index.history_rows(self.history.read_text(encoding="utf-8"))
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0].startswith("| codegraph |"))

    def test_noop_status_and_failure_preserve_history(self):
        original = self.history.read_bytes()
        with patch.object(dk_index, "run", return_value="abc"), patch.object(
                dk_index, "operate", return_value=dict(tool="codegraph", operation="已是最新")):
            dk_index.index("update", "codegraph")
            dk_index.index("status", "codegraph")
        with patch.object(dk_index, "run", side_effect=ValueError("git unavailable")):
            self.assertIn("error", dk_index.index("update", "codegraph")[0])
        self.assertEqual(original, self.history.read_bytes())

    def test_memory_watcher_contention_is_retried_before_verification(self):
        busy = ValueError("another index operation for this project is active with different options")
        args = {"repo_path": str(self.root), "mode": "moderate"}
        with patch.object(dk_index, "cbm", side_effect=[busy, {}, {"state": "succeeded"}]) as cbm, \
                patch.object(dk_index.time, "sleep") as sleep:
            dk_index.update_cbm([], {}, args)
        sleep.assert_called_once()
        self.assertEqual(cbm.call_count, 3)
        self.assertEqual(cbm.call_args.kwargs, {"repo_path": str(self.root), "status": True})


if __name__ == "__main__":
    unittest.main()
