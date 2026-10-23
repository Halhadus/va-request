import importlib.util
from pathlib import Path
import tempfile
import unittest
spec = importlib.util.spec_from_file_location('pps', Path(__file__).with_name('hevc_pps_vector.py'))
pps = importlib.util.module_from_spec(spec); spec.loader.exec_module(pps)


class PPSVector(unittest.TestCase):
    def test_exact_aligned_rewrite(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'clip.hevc'
            original = b'\x00\x00\x01\x44\x01\xc0\x00\x00\x01\x26\x01\xa8'
            path.write_bytes(original)
            self.assertEqual(pps.rewrite_pps_id(path), (1, 1))
            # PPS0 code1 becomes PPS15 code000010000. All following syntax
            # bits,payload and CABAC byte phase remain unchanged.
            self.assertEqual(path.read_bytes(), b'\x00\x00\x00\x01\x44\x01\x08\x40\x00\x00\x00\x01\x26\x01\x82\x08')

    def test_rejection_is_atomic(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'clip.hevc'
            for data, number in [(b'not Annex-B', 15), (b'\x00\x00\x01\x44\x01\x50', 15),
                (b'\x00\x00\x01\x44\x01\x80', 15), (b'\x00\x00\x01\x44\x01\x80', 1)]:
                path.write_bytes(data)
                with self.assertRaises(ValueError): pps.rewrite_pps_id(path, number)
                self.assertEqual(path.read_bytes(), data)

    def test_emulation_prevention(self):
        raw = b'\x00\x00\x00\x01\x00\x00\x02\x00\x00\x03\x04'
        self.assertEqual(pps.rbsp(pps.ebsp(raw)), raw)
        with self.assertRaises(ValueError): pps.rbsp(b'\x00\x00\x03\xff')


if __name__ == '__main__': unittest.main()
