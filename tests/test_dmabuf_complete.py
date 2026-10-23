import csv
import tempfile
import unittest
from pathlib import Path
import check_dmabuf_complete as evidence

class CompleteEvidence(unittest.TestCase):
    def fixture(self, root):
        cases = [['PASS', name, '-'] for name in sorted(evidence.CASES)] + [['COMPLETE', '1']]
        cases += [['PRIME_EVIDENCE', name, layer, '32', '3', '1'] for name in evidence.FORMATS for layer in ('composed', 'separate')]
        vpp = []
        for name in evidence.RGB.values():
            for job in range(40): vpp.append(['0', evidence.fourcc(name), evidence.fourcc(name), '1', '4', '4', '1', '1', str(1 if job < 32 else job)])
        jpeg = [['complete', evidence.fourcc('NM12'), '320', '240', '2', '4', '1'] for _ in range(4)]
        jpeg += [['complete', evidence.fourcc('YM12'), '320', '240', '3', '4', '1'] for _ in range(8)]
        jpeg += [['complete', evidence.fourcc('YM12'), '320', '240', '3', '1', '1'] for _ in range(12)]
        fields = ['STATUS', 'SRC_FOURCC', 'DST_FOURCC', 'QUEUE_VALID', 'OUTPUT_MEMORY', 'CAPTURE_MEMORY', 'OUTPUT_PLANES', 'CAPTURE_PLANES', 'CONTEXT']
        jpeg_fields = ['STAGE', 'FORMAT', 'WIDTH', 'HEIGHT', 'PLANES', 'OUTPUT_MEMORY', 'CAPTURE_MEMORY']
        paths = [root/'results.tsv', root/'vpp.tsv', root/'jpeg.tsv']
        return paths, [cases, [fields] + vpp, [jpeg_fields] + jpeg]

    def run_fixture(self, mutation=None):
        with tempfile.TemporaryDirectory() as d:
            paths, rows = self.fixture(Path(d))
            if mutation: mutation(rows)
            for path, data in zip(paths, rows):
                with path.open('w') as f: csv.writer(f, delimiter='\t').writerows(data)
            return evidence.check(*paths)

    def test_complete(self): self.assertEqual(self.run_fixture(), (120, 12))
    def test_failures(self):
        for change in [lambda r: r[0].pop(), lambda r: r[0].pop(0), lambda r: r[0].append(r[0][0]),
                       lambda r: r[0][-1].__setitem__(3, '31'), lambda r: r[0][-1].__setitem__(4, '2'),
                       lambda r: r[0][-1].__setitem__(5, '0'), lambda r: r[0].append(r[0][-1]),
                       lambda r: r[0][0].__setitem__(0, 'SKIP'), lambda r: r[1].pop(),
                       lambda r: r[1][1].__setitem__(4, '1'), lambda r: r[1][1].__setitem__(6, '2'),
                       lambda r: [row.__setitem__(8, '1') for row in r[1][1:]],
                       lambda r: r[2].pop(), lambda r: r[2][1].__setitem__(4, '1'),
                       lambda r: r[2][1].__setitem__(5, '1'), lambda r: r[2][1].__setitem__(0, 'failed')]:
            with self.subTest(change=change), self.assertRaises(ValueError): self.run_fixture(change)

if __name__ == '__main__': unittest.main()
