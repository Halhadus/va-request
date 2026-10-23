import os
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path


class FeatureRunner(unittest.TestCase):
    def run_case(self, *, abort=False, encode_failure=False, decode_failure=False, repository=False, dirty=False, missing_driver=False, layout_difference=False, layout_missing=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tests").mkdir(); (root / "builddir").mkdir(); (root / "bin").mkdir()
            shutil.copy(Path(__file__).with_name("run_feature_checks.sh"), root / "tests/run_feature_checks.sh")
            shutil.copy(Path(__file__).with_name("feature_source.py"), root / "tests/feature_source.py")
            shutil.copy(Path(__file__).with_name("compare_framemd5.py"), root / "tests/compare_framemd5.py")
            if not missing_driver: (root / "builddir/v4l2request_drv_video.so").write_bytes(b"current driver")
            shutil.copy(Path(__file__).with_name("check_dmabuf_trace.py"), root / "tests/check_dmabuf_trace.py")
            client = root / "builddir/va_feature_client"
            client.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
if os.environ.get("TEST_CLIENT_ABORT") == "1": sys.exit(2)
names = ["NV12", "NV21", "I420", "YUY2", "UYVY", "VYUY", "YVYU", "BGRX"]
with open(sys.argv[2], "w") as f:
    f.write("PASS\\tfake-public-client\\n")
    for name in names:
        for prefix in ["dmabuf-direct-stress-", "dmabuf-cold-stress-"]: f.write("PASS\\t" + prefix + name + "\\n")
    for name in ["NV12", "NV21"]: f.write("PASS\\tdmabuf-multiplanar-stress-" + name + "\\n")
    f.write("COMPLETE\\t1\\n")
with open(os.environ["VAREQ_VPP_TRACE"], "w") as f:
    f.write("STATUS\\tSRC_FOURCC\\tDST_FOURCC\\tQUEUE_VALID\\tOUTPUT_MEMORY\\tCAPTURE_MEMORY\\tOUTPUT_PLANES\\tCAPTURE_PLANES\\tCONTEXT\\n")
    for name in names:
        value = format(int.from_bytes(name.encode(), "little"), "08x")
        for index in range(42): f.write("0\\t" + value + "\\t" + value + "\\t1\\t4\\t4\\t1\\t1\\t" + str(max(1, index - 32)) + "\\n")
        if name in ["NV12", "NV21"]:
            for _ in range(34): f.write("0\\t" + value + "\\t" + value + "\\t1\\t4\\t4\\t2\\t2\\t1\\n")
for q in [5, 50, 95]: Path(sys.argv[2] + f".native-{q}.jpg").write_bytes(b"current")
for i in range(8):
    for kind in ["ref", "test"]:
        if os.environ.get("TEST_LAYOUT_MISSING") == "1" and i == 7 and kind == "test": continue
        Path(sys.argv[2] + f".layout-{i}-{kind}.jpg").write_bytes(b"current")
print("PASS fake-public-client")
''')
            client.chmod(0o755)
            if repository:
                subprocess.run(["git", "init", "-q", "-b", "main"], cwd=root, check=True)
                (root / "source.c").write_text("initial\n")
                subprocess.run(["git", "add", "source.c"], cwd=root, check=True)
                subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-q", "-m", "source"], cwd=root, check=True)
                self.expected_tree = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=root, text=True).strip()
                if dirty: (root / "source.c").write_text("changed\n")
            ffmpeg = root / "bin/ffmpeg"
            ffmpeg.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
if sys.argv[-1] == "-": sys.exit(1 if os.environ.get("TEST_DECODE_FAILURE") == "1" else 0)
if "framemd5" in sys.argv:
    if os.environ.get("TEST_DECODE_FAILURE") == "1": sys.exit(1)
    digest = "f" * 32 if os.environ.get("TEST_LAYOUT_DIFF") == "1" and "-test.jpg" in sys.argv[sys.argv.index("-i") + 1] else "0" * 32
    Path(sys.argv[-1]).write_text("0, 0, 0, 1, 115200, " + digest + "\\n")
    sys.exit(0)
if os.environ.get("TEST_ENCODE_FAILURE") == "1": sys.exit(1)
for i in range(1, 9): Path(sys.argv[-1].replace("%02d", f"{i:02d}")).write_bytes(b"current")
''')
            ffmpeg.chmod(0o755)
            old = root / "verify/features/run.old"; old.mkdir(parents=True)
            for i in range(1, 9): (old / f"jpeg-{i:02d}.jpg").write_bytes(b"stale")
            for i in range(8): (old / f"results.tsv.layout-{i}-test.jpg").write_bytes(b"stale")
            env = {key: value for key, value in os.environ.items() if not key.startswith(("VAREQ_", "LIBVA_"))}
            env.update(PATH=str(root / "bin") + os.pathsep + os.environ["PATH"], VAREQ_BUILD_DIR=str(root / "builddir"),
                       TEST_CLIENT_ABORT=str(int(abort)), TEST_ENCODE_FAILURE=str(int(encode_failure)),
                       TEST_DECODE_FAILURE=str(int(decode_failure)), TEST_LAYOUT_DIFF=str(int(layout_difference)),
                       TEST_LAYOUT_MISSING=str(int(layout_missing)))
            p = subprocess.run(["bash", "tests/run_feature_checks.sh"], cwd=root, env=env,
                               stdout=subprocess.PIPE, text=True, timeout=15)
            summary = (root / "verify/features/summary.txt").read_text()
            run = Path(summary.split("RUN=")[-1].strip())
            self.source = json.loads((run / "source.json").read_text())
            self.expected_client = hashlib.sha256(client.read_bytes()).hexdigest()
            return p.returncode, summary

    def test_fresh_encode_and_decode(self):
        exit_code, summary = self.run_case()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS=23 FAIL=0 COMPLETE=1 EXIT=0", summary)
        self.assertEqual(self.source["driver_sha256"], hashlib.sha256(b"current driver").hexdigest())
        self.assertEqual(self.source["client_sha256"], self.expected_client)
        self.assertIsNone(self.source["tree"])

    def test_parent_hardware_settings_cannot_select_a_real_client(self):
        with patch.dict(os.environ, {"VAREQ_BUILD_DIR": "/unavailable/hardware-build", "VAREQ_RENDER_NODE": "/unavailable/render-node",
            "VAREQ_VPP_TRACE": "/unavailable/vpp.tsv", "LIBVA_DRIVERS_PATH": "/unavailable/drivers"}):
            code, summary = self.run_case()
        self.assertEqual(code, 0)
        self.assertIn("PASS=23 FAIL=0 COMPLETE=1 EXIT=0", summary)
        self.assertEqual(self.source["client_sha256"], self.expected_client)
        self.assertEqual(self.source["driver_sha256"], hashlib.sha256(b"current driver").hexdigest())

    def test_stale_images_cannot_hide_encode_failure(self):
        exit_code, summary = self.run_case(encode_failure=True)
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL=1", summary)

    def test_incomplete_client_is_not_success(self):
        exit_code, summary = self.run_case(abort=True)
        self.assertEqual(exit_code, 1)
        self.assertIn("COMPLETE=0 EXIT=1", summary)
        self.assertEqual(self.source["client_sha256"], self.expected_client)

    def test_clean_source_identity(self):
        code, _ = self.run_case(repository=True)
        self.assertEqual(code, 0)
        self.assertEqual(self.source["tree"], self.expected_tree)
        self.assertFalse(self.source["dirty"])

    def test_dirty_source_is_marked(self):
        code, _ = self.run_case(repository=True, dirty=True)
        self.assertEqual(code, 0)
        self.assertEqual(self.source["tree"], self.expected_tree)
        self.assertTrue(self.source["dirty"])

    def test_missing_driver_cannot_fall_back_to_a_stale_install(self):
        code, summary = self.run_case(missing_driver=True)
        self.assertEqual(code, 1)
        self.assertIn("COMPLETE=0 EXIT=1 STAGE=source", summary)
        self.assertIsNone(self.source["driver_sha256"])

    def test_decode_failure_is_not_success(self):
        exit_code, summary = self.run_case(decode_failure=True)
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL=3", summary)

    def test_jpeg_layout_pixel_difference_is_not_success(self):
        code, summary = self.run_case(layout_difference=True)
        self.assertEqual(code, 1)
        self.assertIn("FAIL=1 COMPLETE=1 EXIT=1", summary)

    def test_old_layout_images_cannot_replace_a_missing_current_candidate(self):
        code, summary = self.run_case(layout_missing=True)
        self.assertEqual(code, 1)
        self.assertIn("FAIL=1 COMPLETE=1 EXIT=1", summary)


if __name__ == "__main__":
    unittest.main()
