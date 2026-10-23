"""The optional inventory keeps only scoped messages and records denial."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import collect_dmabuf_kernel as collect


class KernelMessages(unittest.TestCase):
    def collect(self, available=True, result=None, error=None):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "kernel"
            with patch.object(sys, "argv", ["collect", str(prefix)]), \
                 patch.object(collect.shutil, "which", return_value="dmesg" if available else None), \
                 patch.object(collect.subprocess, "run", return_value=result, side_effect=error) as run, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(collect.main(), 0)
            for call in run.call_args_list:
                self.assertNotIn("stderr", call.kwargs)
                self.assertEqual(call.args[0], ["dmesg", "--color=never"])
            return json.loads(prefix.with_suffix(".json").read_text()), prefix.with_suffix(".txt").read_text()

    def test_scoped_and_bounded(self):
        result = subprocess.CompletedProcess([], 0, "unrelated secret\n" + "[1] rga2: job\n" * 405 + "[2] swiotlb: full\n")
        record, text = self.collect(result=result)
        self.assertEqual(record["matched"], 406)
        self.assertEqual(len(text.splitlines()), 400)
        self.assertNotIn("unrelated", text)
        self.assertIn("swiotlb", text)

    def test_denied(self):
        record, text = self.collect(result=subprocess.CompletedProcess([], 1, ""))
        self.assertEqual(record["exit"], 1)
        self.assertEqual(text, "")

    def test_missing(self):
        record, text = self.collect(available=False)
        self.assertFalse(record["available"])
        self.assertIsNone(record["exit"])
        self.assertEqual(text, "")


if __name__ == "__main__":
    unittest.main()
