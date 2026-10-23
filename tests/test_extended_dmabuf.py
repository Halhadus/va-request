import csv
from pathlib import Path
import tempfile
import unittest

import check_extended_dmabuf as evidence


class ExtendedEvidence(unittest.TestCase):
    def fixture(self, path):
        results, trace = path / "results.tsv", path / "trace.tsv"
        rows = [["PASS", name, "-"] for name in sorted(evidence.CASES)] + [["COMPLETE", "1"]]
        fields = ["STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY", "OUTPUT_PLANES", "CAPTURE_PLANES", "CONTEXT"]
        jobs = []
        for name in evidence.FORMATS:
            for i in range(44 if name in ("ARGB", "BGRA") else 43):
                jobs.append(["0", evidence.fourcc(name), evidence.fourcc(name), "1", "4", "4", "1", "1", str(1 + i % 9)])
        for source, target in evidence.PAIRS:
            jobs.extend([["0", evidence.fourcc(source), evidence.fourcc(target), "1", "4", "4", "1", "1", "21"] for _ in range(8)])
        return results, trace, rows, fields, jobs

    def exercise(self, change=None, accepted=False):
        with tempfile.TemporaryDirectory() as directory:
            results, trace, rows, fields, jobs = self.fixture(Path(directory))
            if change: change(rows, fields, jobs)
            with results.open("w") as f: csv.writer(f, delimiter="\t").writerows(rows)
            with trace.open("w") as f: csv.writer(f, delimiter="\t").writerows([fields] + jobs)
            if accepted: self.assertEqual(sum(evidence.check(results, trace).values()), 265)
            else:
                with self.assertRaises((ValueError, KeyError)): evidence.check(results, trace)

    def test_valid(self): self.exercise(accepted=True)
    def test_missing_case(self): self.exercise(lambda rows, fields, jobs: rows.pop(0))
    def test_duplicate_case(self): self.exercise(lambda rows, fields, jobs: rows.append(rows[0]))
    def test_failed_case(self): self.exercise(lambda rows, fields, jobs: rows[0].__setitem__(0, "FAIL"))
    def test_incomplete(self): self.exercise(lambda rows, fields, jobs: rows.pop())
    def test_staging(self): self.exercise(lambda rows, fields, jobs: jobs[0].__setitem__(4, "1"))
    def test_short_cold(self): self.exercise(lambda rows, fields, jobs: jobs.pop(0))
    def test_one_context(self): self.exercise(lambda rows, fields, jobs: [row.__setitem__(8, "1") for row in jobs])
    def test_short_cross(self): self.exercise(lambda rows, fields, jobs: jobs.pop())


if __name__ == "__main__": unittest.main()
