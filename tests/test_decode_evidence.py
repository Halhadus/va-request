import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from decode_evidence import summarize
from check_capture_pool_trace import check as check_pool

SCRIPT = Path(__file__).with_name('run_full_matrix.sh').read_text()
PAIR = SCRIPT[SCRIPT.index('pair_sw() {'):SCRIPT.index('\npair_gst_mpeg2() {')]
RECORD = next(line for line in SCRIPT.splitlines() if line.startswith('record_result()'))


class DecodeEvidence(unittest.TestCase):
    def test_matrix_preserves_complete_truncated_and_process_failures(self):
        for count, code in [(8, 0), (7, 0), (4, 251), (0, 0)]:
            with self.subTest(count=count, code=code), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); output = root / 'matrix output'; output.mkdir()
                ff = root / 'ffmpeg'
                ff.write_text(f'#!{sys.executable}\n' + '''import os, sys
from pathlib import Path
hardware = '-hwaccel' in sys.argv
count = int(os.environ['TEST_FRAMES']) if hardware else 8
if hardware:
 Path(os.environ['VAREQ_DECODE_TRACE']).write_text('STAGE=capture-empty\\tSTATUS=-1\\tALLOCATED=4\\tFREE=0\\n')
elif 'VAREQ_DECODE_TRACE' in os.environ: sys.exit(77)
if count: Path(sys.argv[-1]).write_text(''.join(f'0,{i},{i},1,12441600,{i:032x}\\n' for i in range(count)))
print('fixture hardware stderr' if hardware else 'fixture software stderr', file=sys.stderr)
sys.exit(int(os.environ['TEST_EXIT']) if hardware else 0)
'''); ff.chmod(0o755)
                # A previous trace must never substitute for this invocation.
                (output / 'smoke-decode.tsv').write_text('stale-previous-run')
                for kind in ['hw', 'sw']: (output / f'smoke-{kind}.md5').write_text(''.join(f'0,{i},{i},1,12441600,{i:032x}\n' for i in range(8)))
                env = dict(os.environ, ROOT=str(Path(__file__).resolve().parents[1]), OUT=str(output), FF=str(ff), DRM='mock-render', TEST_FRAMES=str(count), TEST_EXIT=str(code))
                env.pop('VAREQ_DECODE_TRACE', None)
                shell = 'set -uo pipefail\npass=0; fail=0\n' + RECORD + '\ncompare_md5() { python3 "$ROOT/tests/compare_framemd5.py" "$@"; }\n' + PAIR + '\npair_sw "clip with spaces" 8 smoke\ntest "$fail" -eq 0\n'
                result = subprocess.run(['bash', '-c', shell], env=env, text=True, stdout=subprocess.PIPE)
                self.assertEqual(result.returncode, 0 if count == 8 and not code else 1, result.stdout)
                report = json.loads((output / 'smoke-decode.json').read_text())
                self.assertEqual((report['hardware_frames'], report['software_frames'], report['hardware_exit']), (count, 8, code))
                self.assertEqual(report['complete_match'], count == 8 and not code)
                self.assertEqual(report['emitted_prefix_matches'], bool(count))
                self.assertEqual(report['last_events'][-1]['STAGE'], 'capture-empty')
                self.assertEqual((output / 'smoke-exit.txt').read_text(), f'HARDWARE_EXIT={code}\nSOFTWARE_EXIT=0\n')
                self.assertTrue((output / 'results.tsv').read_text().startswith(('PASS' if count == 8 and not code else 'FAIL') + '\t'))

    def test_malformed_and_missing_evidence_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); h = root / 'hardware'; s = root / 'reference'; trace = root / 'trace'
            h.write_text('0,0,0,1,12,' + 'a' * 32 + '\n'); s.write_bytes(h.read_bytes())
            report = summarize('one', 1, 0, 0, trace, h, s)
            self.assertFalse(report['trace_present']); self.assertTrue(report['complete_match'])
            trace.write_text('broken-event\n'); report = summarize('one', 1, 0, 0, trace, h, s)
            self.assertTrue(report['errors']); self.assertFalse(report['complete_match'])
            h.write_text('invalid-frame\n'); report = summarize('one', 1, 0, 0, trace, h, s)
            self.assertEqual(report['hardware_frames'], 0); self.assertFalse(report['complete_match'])
            h.unlink(); report = summarize('one', 1, 0, 0, trace, h, s)
            self.assertTrue(report['errors']); self.assertEqual(report['hardware_frames'], 0)

    def test_required_pool_evidence_rejects_substitution_and_incomplete_or_duplicate_jobs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'pool.tsv'
            config = dict(STAGE='capture-config', POOL='1', MEMORY='4', ALLOCATED='24', WIDTH='3840', HEIGHT='2160', SIZEIMAGE='16588800')
            jobs = [dict(STAGE=stage, PID='123', CONTEXT='1', FRAME=str(i), STATUS='0', POOL='1', MEMORY='4') for i in range(1, 9) for stage in ['capture-pool-complete', 'end-picture']]
            rows = [config] + jobs
            def write(data): path.write_text('\n'.join('\t'.join(f'{k}={v}' for k, v in row.items()) for row in data) + '\n')
            write(rows); self.assertEqual(check_pool(path, 8), 8)
            for field, value in [('POOL', '0'), ('MEMORY', '1'), ('ALLOCATED', '5'), ('WIDTH', '1920')]:
                bad = [dict(config, **{field: value})] + jobs; write(bad)
                with self.assertRaises(ValueError): check_pool(path, 8)
            for bad in [rows[:-1], rows + [jobs[-1]], [config] + [dict(e, STATUS='1') for e in jobs]]:
                write(bad)
                with self.assertRaises(ValueError): check_pool(path, 8)

    def test_forced_matrix_calls_keep_parent_policy_and_enable_both_codecs(self):
        stage = SCRIPT[SCRIPT.index('# Forced internal CAPTURE pool hardware checks;'):SCRIPT.index('# End forced internal CAPTURE pool checks.')]
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, OUT=directory, CLIP='clip dir', VAREQ_TEST_CAPTURE_POOL='1', VAREQ_CAPTURE_POOL_DMABUF='auto')
            shell = 'set -u\npair_sw() { printf "%s|%s|%s|%s\\n" "$VAREQ_CAPTURE_POOL_DMABUF" "$3" "$2" "$6" >> "$OUT/calls"; }\n' + stage + '\nprintf "%s" "$VAREQ_CAPTURE_POOL_DMABUF" > "$OUT/parent"\n'
            result = subprocess.run(['bash', '-c', shell], env=env)
            self.assertEqual(result.returncode, 0)
            self.assertEqual((Path(directory) / 'calls').read_text().splitlines(), ['required|h264-4k-pool|8|1', 'required|hevc-4k-pool|8|1'])
            self.assertEqual((Path(directory) / 'parent').read_text(), 'auto')


if __name__ == '__main__': unittest.main()
