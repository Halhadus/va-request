import importlib.util
import json
import io
import tarfile
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import patch
import xml.etree.ElementTree as ET
spec = importlib.util.spec_from_file_location("report_runner", Path(__file__).parents[1] / "tools/test.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Reports(unittest.TestCase):
    def test_main_preserves_exit_codes_and_finalizes_reports(self):
        for failure in (None, "configure", "build", "unit", "interrupt", "runner", "sanitation"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                def execute(name, command, output, env=None):
                    if failure == "interrupt": raise KeyboardInterrupt
                    if failure == "runner": raise RuntimeError("runner test failure")
                    return {"name": name, "status": "FAIL" if name == failure else "PASS", "exit_code": int(name == failure), "duration_s": 0}
                with patch.object(runner, "source_identity", return_value={}), patch.object(runner.shutil, "which", return_value="/meson"), patch.object(runner, "execute", side_effect=execute), patch.object(runner, "sanitize_report_logs", side_effect=ValueError("sanitation test failure") if failure == "sanitation" else None):
                    code = runner.main(["--suite", "unit", "--build-dir", str(path / "build"), "--output-dir", str(path / "reports")])
                expected = 130 if failure == "interrupt" else int(failure is not None)
                self.assertEqual(code, expected)
                report = json.loads(next((path / "reports").glob("run-*/report.json")).read_text())
                self.assertEqual(report["exit_code"], expected)
                self.assertEqual(report["complete"], failure not in ("configure", "build", "interrupt", "runner"))
                self.assertEqual(len(list((path / "reports").glob("libva-v4l2-run-*.tar.gz"))), 1)

    def test_conformance_collects_process_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            command = [sys.executable, '-c', "import time; data = bytearray(4*1024*1024); time.sleep(0.3); print(len(data))"]
            result = runner.execute('conformance-fixture-hardware', command, path)
            self.assertEqual(result['exit_code'], 0)
            if Path('/proc/self/status').is_file():
                self.assertGreater(result['resources']['samples'], 0)
                self.assertGreater(result['resources']['peak_rss_kib'], 4096)
                self.assertGreaterEqual(result['resources']['peak_fds'], 3)

    def test_failed_primary_is_not_hidden_by_passing_retry(self):
        record = {"suites": [{"name": "primary", "status": "FAIL", "exit_code": 1}, {"name": "retry", "status": "PASS", "exit_code": 0}], "complete": True, "exit_code": 1}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); runner.write_report(record, path)
            summary = json.loads((path / "report.json").read_text())["summary"]
            self.assertEqual(summary, {"pass": 1, "fail": 1, "skip": 0, "complete": True, "exit_code": 1})
            self.assertEqual(ET.parse(path / "junit.xml").getroot().attrib["failures"], "1")

    def test_failed_suite_cannot_borrow_passing_stale_cases(self):
        record = {"suites": [{"name": "unit", "status": "FAIL", "exit_code": 1, "cases": [{"name": "old", "status": "PASS", "exit_code": 0}]}], "complete": True, "exit_code": 1}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); runner.write_report(record, path)
            self.assertEqual(record["summary"]["fail"], 1)

    def test_incomplete_run_stays_incomplete(self):
        record = {"suites": [], "complete": False, "exit_code": 130}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); runner.write_report(record, path)
            self.assertIn("COMPLETE=0 EXIT=130", (path / "summary.txt").read_text())

    def test_shared_logs_keep_test_diagnostics_without_inherited_secrets(self):
        data = {"name": "decode", "result": "FAIL", "stdout": "pixel mismatch", "stderr": "queue failure",
                "env": {"API_TOKEN": "private", "VAV4L2_EXPORT": "gbm", "ASAN_OPTIONS": "halt_on_error=1"}}
        result = json.loads(runner.sanitize_meson_log("unit-testlog.json", json.dumps(data).encode()))
        self.assertEqual(result["env"], {"VAV4L2_EXPORT": "gbm", "ASAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result["stdout"], data["stdout"])
        self.assertEqual(result["stderr"], data["stderr"])
        text = b"Inherited environment: API_TOKEN=private\n1/1 decode FAIL\nqueue failure\n"
        self.assertEqual(runner.sanitize_meson_log("testlog.txt", text), b"1/1 decode FAIL\nqueue failure\n")

    def test_nested_log_filter_preserves_other_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            with tarfile.open(path / "hardware.tar.gz", "w:gz") as archive:
                for name, data in [("run/unit-testlog.txt", b"Inherited environment: TOKEN=private\nFAIL decode\n"),
                                   ("run/pixels.bin", bytes(range(256)))]:
                    member = tarfile.TarInfo(name); member.size = len(data); archive.addfile(member, io.BytesIO(data))
            runner.sanitize_report_logs(path)
            with tarfile.open(path / "hardware.tar.gz") as archive:
                self.assertEqual(archive.extractfile("run/unit-testlog.txt").read(), b"FAIL decode\n")
                self.assertEqual(archive.extractfile("run/pixels.bin").read(), bytes(range(256)))

    def test_malformed_log_is_removed_instead_of_shared(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); log = path / "testlog.json"
            log.write_text('{"env": {"TOKEN": "private"}')
            with self.assertRaises(ValueError): runner.sanitize_report_logs(path)
            self.assertFalse(log.exists())

    def test_timeout_and_unexpected_pass_are_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "testlog.json"
            path.write_text('\n'.join(json.dumps({"name": result, "result": result, "returncode": 0, "duration": 0.1}) for result in ("OK", "SKIP", "TIMEOUT", "UNEXPECTEDPASS")))
            self.assertEqual([case["status"] for case in runner.meson_cases(path)], ["PASS", "SKIP", "FAIL", "FAIL"])


if __name__ == "__main__": unittest.main()
