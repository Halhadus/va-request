import tempfile
import unittest
from pathlib import Path
from compare_framemd5 import read_frames


class FrameMD5Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'frames.md5'

    def test_valid(self):
        self.path.write_text('#format: frame checksums\n0, 0, 0, 1, 6, ' + 'ab' * 16 + '\n')
        self.assertEqual(read_frames(self.path, 1), [(6, 'ab' * 16)])
        with self.assertRaises(ValueError):
            read_frames(self.path, 2)

    def test_empty_and_malformed(self):
        for value in ('#header only\n', '', '0,0,0,1,0,' + 'ab' * 16, '0,0,0,1,6,not-md5'):
            self.path.write_text(value)
            with self.assertRaises(ValueError):
                read_frames(self.path, 1)


if __name__ == '__main__':
    unittest.main()
