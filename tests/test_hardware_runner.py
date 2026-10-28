import importlib.util
from pathlib import Path
import unittest
import io
import tarfile
import tempfile
from unittest.mock import patch
from encoder_pool import x265_pool

spec = importlib.util.spec_from_file_location("hardware", Path(__file__).parents[1] / "tools/hardware.py")
hardware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardware)


class Hardware(unittest.TestCase):
    def test_encoder_pool_uses_available_cpus_without_assuming_node_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for node, cpus in ((0, '0-1'), (1, ''), (2, '2-3,8'), (7, '9-10')):
                path = root / f'node{node}'; path.mkdir(); (path / 'cpulist').write_text(cpus)
            for allowed, expected, automatic in (({0}, '1,-', '1,-'), ({2, 3, 8}, '-,-,1,-', '-,-,3,-'),
                ({9}, '-,-,-,-,-,-,-,1,-', '-,-,-,-,-,-,-,1,-')):
                with self.subTest(allowed=allowed), patch('encoder_pool.os.sched_getaffinity', return_value=allowed):
                    self.assertEqual(x265_pool(node_root=root), expected)
                    self.assertEqual(x265_pool(None, root), automatic)
            with patch('encoder_pool.os.sched_getaffinity', return_value={99}):
                self.assertEqual(x265_pool(node_root=root), '1')
                self.assertEqual(x265_pool(node_root=root / 'missing'), '1')
                self.assertEqual(x265_pool(None, root / 'missing'), '*')
            with patch('encoder_pool.os.sched_getaffinity', side_effect=OSError):
                self.assertEqual(x265_pool(node_root=root), '1')
            with patch('encoder_pool.Path.glob', side_effect=OSError):
                self.assertEqual(x265_pool(node_root=root), '1')

    def test_acceptance_groups_follow_interfaces_without_board_identity(self):
        profiles = ["VAProfileH264ConstrainedBaseline", "VAProfileH264Main", "VAProfileH264High", "VAProfileH264High10", "VAProfileH264High422",
            "VAProfileHEVCMain", "VAProfileHEVCMain10", "VAProfileVP8Version0_3", "VAProfileMPEG2Simple", "VAProfileMPEG2Main"]
        caps = {"profiles": [{"profile": p, "entrypoint": "VAEntrypointVLD"} for p in profiles] + [
            {"profile": "VAProfileJPEGBaseline", "entrypoint": "VAEntrypointEncPicture"}, {"profile": "VAProfileNone", "entrypoint": "VAEntrypointVideoProc"}]}
        inventory = {"nodes": [{"roles": ["raw M2M candidate"], "sections": [{"stdout": "'NV12' 'NV21' 'YU12' 'NV16' 'BA24' 'AR24' 'XR24' 'RGBP' rotate horizontal_flip vertical_flip"}]}]}
        self.assertTrue(all(hardware.acceptance_plan(caps, inventory).values()))
        caps["profiles"] = [entry for entry in caps["profiles"] if entry["profile"] != "VAProfileHEVCMain10"]
        plan = hardware.acceptance_plan(caps, inventory)
        self.assertFalse(plan["matrix"] or plan["imported"] or plan["direct-capture"])
        self.assertTrue(plan["features"] and plan["renderer"] and plan["transport"])
        plan = hardware.acceptance_plan(caps, {})
        self.assertFalse(plan["features"] or plan["transport"])
        self.assertTrue(plan["renderer"])

    def test_acceptance_report_keeps_nested_failures_and_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hardware.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                for name, text in [("run/results.tsv", "NAME\tSTATUS\tEXIT_CODE\nmatrix\tFAIL\t1\n"),
                    ("run/matrix/results.tsv", "RESULT\tCASE\nPASS\thevc-main\nFAIL\thevc10\nSKIP\tav1\n"),
                    ("run/renderer.tsv", "PASS\tNV12\nSKIP\tY210\nCOMPLETE\t1\n"),
                    ("run/transport.tsv", "STATUS\tNODE\tFORMAT\nPASS\t/dev/video0\tNV12\nSKIP\t/dev/video0\tY210\n"),
                    ("run/trace.tsv", "TIME\tNODE\n123\t/dev/video0\n")]:
                    data = text.encode(); member = tarfile.TarInfo(name); member.size = len(data); archive.addfile(member, io.BytesIO(data))
            cases = hardware.acceptance_cases(path)
            self.assertEqual([case["status"] for case in cases], ["FAIL", "PASS", "FAIL", "SKIP", "PASS", "SKIP", "PASS", "SKIP"])
            self.assertEqual(len({case["name"] for case in cases}), 8)

    def test_profiles_are_independent_of_board_and_each_other(self):
        profiles = {"profiles": [{"profile": "VAProfileHEVCMain", "entrypoint": "VAEntrypointVLD"},
            {"profile": "VAProfileH264High", "entrypoint": "VAEntrypointEncSlice"}]}
        self.assertEqual(hardware.select_codecs(profiles), ["hevc"])
        self.assertEqual(hardware.select_codecs({"profiles": []}), [])

    def frames(self):
        n = hardware.WIDTH * hardware.HEIGHT * 3 // 2
        return b"".join(bytes([16 + frame]) * n for frame in range(hardware.FRAMES))

    def test_unchanged_or_truncated_output_is_not_success(self):
        frames = self.frames()
        hardware.compare_pixels(frames, frames, "hevc")
        for actual in (frames[:-1], frames[:len(frames) // hardware.FRAMES] * hardware.FRAMES):
            with self.assertRaises(ValueError): hardware.compare_pixels(actual, frames, "hevc")

    def test_exact_and_idct_comparisons_are_distinct(self):
        frames = self.frames()
        rounded = bytes([frames[0] + 1]) + frames[1:]
        with self.assertRaises(ValueError): hardware.compare_pixels(rounded, frames, "h264")
        hardware.compare_pixels(rounded, frames, "mpeg2")
        corrupt = bytes([frames[0] + 5]) + frames[1:]
        with self.assertRaises(ValueError): hardware.compare_pixels(corrupt, frames, "mpeg2")


if __name__ == "__main__": unittest.main()
