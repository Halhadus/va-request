import csv
import tempfile
import unittest
from pathlib import Path
from check_dmabuf_trace import FORMATS, check


class DirectEvidence(unittest.TestCase):
    def fixture(self, root, *, memory=4, count=42, complete=True, missing_case=False, cold=True, context_count=9):
        results, trace = root / "results.tsv", root / "trace.tsv"
        names = list(FORMATS)
        selected = names[1:] if missing_case else names
        results.write_text("".join("PASS\tdmabuf-direct-stress-" + name + "\n" for name in selected))
        with results.open("a") as file:
            if cold:
                for name in names: file.write("PASS\tdmabuf-cold-stress-" + name + "\n")
            for name in ("NV12", "NV21"): file.write("PASS\tdmabuf-multiplanar-stress-" + name + "\n")
            if complete: file.write("COMPLETE\t1\n")
        with trace.open("w") as file:
            writer = csv.writer(file, delimiter="\t")
            writer.writerow(["STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY", "OUTPUT_PLANES", "CAPTURE_PLANES", "CONTEXT"])
            for fourcc in FORMATS.values():
                value = f"{int.from_bytes(fourcc.encode(), 'little'):08x}"
                for index in range(count): writer.writerow([0, value, value, 1, memory, memory, 1, 1, min(max(1, index - 32), context_count)])
                if fourcc in ("NV12", "NV21"):
                    for _ in range(34): writer.writerow([0, value, value, 1, memory, memory, 2, 2, 1])
        return results, trace

    def test_fresh_dma_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(sum(check(*self.fixture(Path(directory))).values()), 404)

    def test_missing_cold_case_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), cold=False))

    def test_repeated_context_cannot_substitute_for_fresh_contexts(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), context_count=1))

    def test_staging_cannot_substitute(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), memory=1))

    def test_short_run_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), count=41))

    def test_failed_case_cannot_be_replaced_by_old_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), missing_case=True))

    def test_incomplete_client_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): check(*self.fixture(Path(directory), complete=False))


if __name__ == "__main__":
    unittest.main()
