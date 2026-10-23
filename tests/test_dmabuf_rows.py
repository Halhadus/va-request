import csv
from pathlib import Path
import tempfile
import unittest
import check_dmabuf_rows as evidence


class RowEvidence(unittest.TestCase):
    def exercise(self, change=None, accepted=False):
        rows = [["PASS", name, "-"] for name in sorted(evidence.CASES)] + [["COMPLETE", "1"]]
        fields = ["STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY"]
        jobs = []
        for name in evidence.FORMATS:
            if name == "P010": continue
            raw = f"{int.from_bytes(name.encode(), 'little'):08x}"
            jobs.extend([["0", raw, raw, "1", "1", "1"] for _ in range(4)])
        if change: change(rows, fields, jobs)
        with tempfile.TemporaryDirectory() as directory:
            results, trace = Path(directory)/"results.tsv", Path(directory)/"trace.tsv"
            with results.open("w") as file: csv.writer(file, delimiter="\t").writerows(rows)
            with trace.open("w") as file: csv.writer(file, delimiter="\t").writerows([fields]+jobs)
            if accepted: self.assertEqual(evidence.check(results, trace), 24)
            else:
                with self.assertRaises((ValueError, KeyError)): evidence.check(results, trace)

    def test_valid(self): self.exercise(accepted=True)
    def test_missing(self): self.exercise(lambda rows, fields, jobs: rows.pop(0))
    def test_duplicate(self): self.exercise(lambda rows, fields, jobs: rows.append(rows[0]))
    def test_failed(self): self.exercise(lambda rows, fields, jobs: rows[0].__setitem__(0, "FAIL"))
    def test_incomplete(self): self.exercise(lambda rows, fields, jobs: rows.pop())
    def test_short_trace(self): self.exercise(lambda rows, fields, jobs: jobs.pop())
    def test_wrong_transport(self): self.exercise(lambda rows, fields, jobs: jobs[0].__setitem__(4, "4"))
    def test_failed_job(self): self.exercise(lambda rows, fields, jobs: jobs[0].__setitem__(0, "1"))


if __name__ == "__main__": unittest.main()
