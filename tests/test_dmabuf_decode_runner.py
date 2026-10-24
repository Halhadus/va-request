import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('dmabuf_runner', Path(__file__).with_name('dmabuf_decode_checks.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ImportRunner(unittest.TestCase):
    def run_case(self, fault=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = root / 'build'; build.mkdir(); output = root / 'out'
            for name in ['v4l2_drv_video.so', 'hevc_slice_interpose.so', 'prime_surface_interpose.so']:
                (build / name).write_bytes(b'new binary')
            (output / 'run.old').mkdir(parents=True)
            (output / 'run.old/summary.txt').write_text('PASS=30 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done\n')

            def execute(args, **kw):
                env = kw.get('env')
                if args[0] == runner.sys.executable:
                    current = Path(env['VAV4L2_PRIME_TRACE_DIR'])
                    self.assertTrue(env['LD_PRELOAD'].startswith(str(build / 'prime_surface_interpose.so')))
                    packets = current / 'packets'; packets.mkdir()
                    (packets / 'summary.txt').write_text('PASS=58 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done\n')
                    for i in range(58 if fault != 'missing' else 57):
                        (current / f'prime-{i}.tsv').write_text('842094158\t320\t240\t8\t0\n')
                    if fault == 'packet-exit': return subprocess.CompletedProcess(args, 1)
                    if fault == 'incomplete': (packets / 'summary.txt').write_text('PASS=27 FAIL=0 COMPLETE=0 EXIT=0 STAGE=done\n')
                elif '-hwaccel' in args:
                    current = Path(env['VAV4L2_PRIME_TRACE_DIR'])
                    if fault != 'unhooked': (current / 'prime-h264.tsv').write_text('842094158\t320\t240\t8\t' + ('1' if fault == 'import-error' else '0') + '\n')
                    if fault == 'hardware-exit': return subprocess.CompletedProcess(args, 2)
                    Path(args[-1]).write_text('#dimensions 0: 320x240\n' + ''.join(f'0, {i}, {i}, 1, 115200, {('f' * 32) if fault == 'pixels' and i == 7 else format(i, '032x')}\n' for i in range(8)))
                elif 'framemd5' in args:
                    Path(args[-1]).write_text('#dimensions 0: 320x240\n' + ''.join(f'0, {i}, {i}, 1, 115200, {i:032x}\n' for i in range(8)))
                return subprocess.CompletedProcess(args, 0)

            with patch.object(runner.subprocess, 'run', side_effect=execute), patch.object(runner.subprocess, 'check_output', side_effect=['a' * 40 + '\n', '']):
                code = runner.run(root, build, output, 'fixture-ffmpeg', 'fixture-render')
            summary = (output / 'summary.txt').read_text()
            self.assertIn('RUN=', summary)
            self.assertEqual((output / 'run.old/summary.txt').read_text(), 'PASS=30 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done\n')
            return code, summary

    def test_all_evidence_and_exact_pixels(self):
        code, summary = self.run_case()
        self.assertEqual(code, 0)
        self.assertIn('PASS=60 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done', summary)

    def test_failures_cannot_be_hidden_by_old_pass(self):
        for fault in ['missing', 'unhooked', 'import-error', 'pixels', 'packet-exit', 'incomplete', 'hardware-exit']:
            with self.subTest(fault=fault):
                code, summary = self.run_case(fault)
                self.assertEqual(code, 1)
                self.assertIn('FAIL=1 COMPLETE=0 EXIT=1', summary)
                if fault in ['missing', 'unhooked', 'import-error']: self.assertIn('STAGE=import-evidence', summary)

    def test_empty_and_malformed_traces_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace.tsv'
            for text in ['', 'garbage', '1\t2\t3\t4\t1\n', '1\t0\t3\t4\t0\n']:
                path.write_text(text)
                with self.assertRaises(ValueError): runner.validate_imports([path], 1)


if __name__ == '__main__': unittest.main()
