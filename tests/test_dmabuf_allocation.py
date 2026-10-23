import csv
from pathlib import Path
import tempfile
import unittest

import check_dmabuf_allocation as evidence


class AllocationEvidence(unittest.TestCase):
    def exercise(self, change=None, accepted=False):
        fields = ["NODE", "STAGE", "WIDTH_BYTES", "ROWS", "SIZE", "ALLOCATED", "ERRNO", "FORMAT", "ALLOC_WIDTH", "ALLOC_HEIGHT", "STRIDE", "IMAGE"]
        rows = [["/dev/video0", "exported-rga2-capture", "128", "256", "32768", "32768", "0", "34325258", "64", "128", "256", "32768"] for _ in range(8)]
        if change: change(rows, fields)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "allocation.tsv"
            with path.open("w") as file: csv.writer(file, delimiter="\t").writerows([fields] + rows)
            if accepted: self.assertEqual(evidence.check(path), 8)
            else:
                with self.assertRaises((ValueError, KeyError)): evidence.check(path)

    def test_valid(self): self.exercise(accepted=True)
    def test_missing(self): self.exercise(lambda rows, fields: rows.pop())
    def test_fallback(self): self.exercise(lambda rows, fields: rows[0].__setitem__(1, "exported"))
    def test_oversized(self): self.exercise(lambda rows, fields: rows[0].__setitem__(5, str(32768 + evidence.os.sysconf("SC_PAGESIZE"))))
    def test_short(self): self.exercise(lambda rows, fields: rows[0].__setitem__(11, "32767"))
    def test_wrong_shape(self): self.exercise(lambda rows, fields: rows[0].__setitem__(10, "144"))
    def test_wrong_format(self): self.exercise(lambda rows, fields: rows[0].__setitem__(7, "32315559"))
    def test_missing_geometry(self): self.exercise(lambda rows, fields: fields.__setitem__(11, "OLD_IMAGE"))


if __name__ == "__main__": unittest.main()
