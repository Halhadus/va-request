import csv
from pathlib import Path
import tempfile
import unittest
from check_capture_trace import check


class CaptureEvidence(unittest.TestCase):
    def fixture(self, root, memory=4, frames=8, missing=None, stage="complete"):
        path = root / "trace.tsv"
        with path.open("w") as file:
            writer = csv.writer(file, delimiter="\t")
            writer.writerow(["STAGE", "CODEC", "FORMAT", "MEMORY", "ERRNO", "SURFACE", "SIZEIMAGE", "STRIDE"])
            for codec in (0, 1, 3, 4):
                if codec != missing:
                    for frame in range(frames): writer.writerow([stage, codec, "3231564e", memory, 0, frame, 4096, 64])
        return path

    def test_four_existing_codecs(self):
        with tempfile.TemporaryDirectory() as directory: self.assertEqual(sum(check(self.fixture(Path(directory))).values()), 32)

    def test_staging_is_not_direct_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(self.fixture(Path(directory), memory=1))

    def test_queueing_is_not_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(self.fixture(Path(directory), stage="queued"))

    def test_short_or_missing_codec_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            for kwargs in ({"frames": 7}, {"missing": 1}):
                with self.assertRaises(ValueError): check(self.fixture(Path(directory), **kwargs))


if __name__ == "__main__": unittest.main()
