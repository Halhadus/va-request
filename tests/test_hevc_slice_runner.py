import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parent))
spec = importlib.util.spec_from_file_location('hevc_slice_runner', Path(__file__).with_name('hevc_slice_checks.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class HEVCSliceRunner(unittest.TestCase):
    def run_case(self, fault=None, export=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = root / 'build'; output = root / 'report'
            build.mkdir(); output.mkdir(); old = output / 'run.old'; old.mkdir()
            (old / 'summary.txt').write_text('PASS=10 FAIL=0 COMPLETE=1 EXIT=0\n')
            for name in ('hevc_slice_interpose.so', 'v4l2_drv_video.so'):
                (build / name).write_bytes(b'fixture-binary')
            calls = []

            def execute(args, **kwargs):
                if args[0] == 'git':
                    return subprocess.CompletedProcess(args, 0, stdout='a' * 40 + '\n')
                calls.append((args, kwargs['env']))
                path = Path(args[-1])
                if '-c:v' in args:
                    if fault == 'generate':
                        return subprocess.CompletedProcess(args, 7)
                    path.write_bytes((b'\x00\x00\x01\x00' + b'\x00\x00\x01\x01\xaa' * 15) * 8 if path.suffix == '.m2v' else b'fresh-hevc')
                    return subprocess.CompletedProcess(args, 0)
                depth = 10 if path.name.startswith(('main10', 'h264-high10')) else 8
                paff = path.name.startswith('h264-paff-')
                width, height = (128, 96) if paff else (320, 240)
                size = width * height * 3 // (1 if depth == 10 else 2)
                env = kwargs['env']; mode = env.get('VAV4L2_HEVC_PACKET_MODE')
                if args[args.index('-f') + 1] == 'rawvideo':
                    if fault == 'paff-diagnostic-exit' and '-hwaccel' in args: return subprocess.CompletedProcess(args, 9)
                    data = bytearray(size * 8)
                    if '-hwaccel' in args: data[width] = 1
                    path.write_bytes(data)
                    return subprocess.CompletedProcess(args, 0)
                records = [f'#dimensions 0: {width}x{height}'] + [f'0, {i}, {i}, 1, {size}, {i:032x}' for i in range(8)]
                if (fault == 'pixels' or (fault == 'mpeg2-pixels' and path.name.startswith('mpeg2-'))) and mode == 'offset':
                    records[-1] = records[-1].replace(f'{7:032x}', 'f' * 32)
                if paff and fault in ('paff-pixels', 'paff-diagnostic-exit') and mode:
                    records[-1] = records[-1].replace(f'{7:032x}', 'f' * 32)
                path.write_text('\n'.join(records) + '\n')
                if '-hwaccel' in args:
                    if paff:
                        rows = []
                        for i in range(8):
                            for stage in ['h264-field-hold', 'end-picture', 'h264-field-pair', 'submit', 'submit', 'end-picture']:
                                if fault == 'missing-hold' and stage == 'h264-field-hold': continue
                                rows.append(f'STAGE={stage}\tFRAME={i}\tSURFACE={i}\tSTATUS=0')
                        Path(env['VAV4L2_DECODE_TRACE']).write_text('\n'.join(rows) + '\n')
                    if '-pps15-' in path.name:
                        Path(env['VAV4L2_DECODE_TRACE']).write_text(('STAGE=hevc-header\tSTATUS=0\tARG=' + ('0' if fault == 'pps-id' else '15') + '\n') * 8)
                    self.assertEqual(env['LIBVA_DRIVER_NAME'], 'v4l2')
                    self.assertEqual(env['LIBVA_DRIVERS_PATH'], str(build))
                    self.assertEqual(Path(env['VAV4L2_DECODE_TRACE']).name, path.name.replace('-hw.md5', '-decode.tsv'))
                    self.assertEqual(Path(env['VAV4L2_STORAGE_TRACE']).name, path.name.replace('-hw.md5', '-storage.tsv'))
                    self.assertTrue(env['LD_PRELOAD'].startswith(str(build / 'hevc_slice_interpose.so') + ':'))
                    if fault == 'missing-trace':
                        return subprocess.CompletedProcess(args, 0)
                    with Path(env['VAV4L2_HEVC_PACKET_LOG']).open('w') as stream:
                        writer = csv.writer(stream, delimiter='\t'); writer.writerow(runner.FIELDS)
                        for i in range(16 if paff else 8):
                            actual_mode = 'whole' if fault == 'mode' and mode == 'array' else mode
                            vendor = 'another-driver' if fault == 'vendor' else runner.VENDOR
                            picture = 0 if fault == 'duplicate' and i == 7 else i
                            slices = 15 if path.name.startswith('mpeg2-') else 2 if '-slices-' in path.name else 1
                            writer.writerow([actual_mode, picture, slices, slices if mode in ('whole', 'offset') else slices * 3,
                                0, vendor, env.get('VAV4L2_EXPBUF_EXPORT', 'default')])
                    if fault == 'exit' and mode == 'fragments':
                        return subprocess.CompletedProcess(args, 8)
                return subprocess.CompletedProcess(args, 0)

            environment = dict(os.environ, LD_PRELOAD='existing-test-hook', LIBVA_DRIVER_NAME='stale-driver', LIBVA_DRIVERS_PATH='stale-build')
            environment.pop('VAV4L2_HEVC_PACKET_MODE', None); environment.pop('VAV4L2_HEVC_PACKET_LOG', None)
            if export is None:
                environment.pop('VAV4L2_EXPBUF_EXPORT', None)
            else:
                environment['VAV4L2_EXPBUF_EXPORT'] = export
            with patch.dict(os.environ, environment, clear=True), patch.object(runner.subprocess, 'run', side_effect=execute), patch.object(runner, 'rewrite_pps_id'), patch.object(runner, 'load_paff_fixture', return_value=b'fixture-paff'):
                status = runner.run_checks(root, build, output, 'fixture-ffmpeg', 'fixture-render')
            summary = (output / 'summary.txt').read_text()
            run = Path(summary.split('RUN=', 1)[1].strip())
            self.assertNotEqual(run, old)
            self.assertEqual((old / 'summary.txt').read_text(), 'PASS=10 FAIL=0 COMPLETE=1 EXIT=0\n')
            self.assertEqual((run / 'summary.txt').read_text(), summary)
            failure = (run / 'failure.txt').read_text() if (run / 'failure.txt').exists() else ''
            self.diagnostics = [json.loads(p.read_text()) for p in run.glob('*-diagnostic.json')]
            return status, summary, failure, (run / 'results.tsv').read_text(), calls

    def test_nonzero_pps_cannot_pass_with_pps_zero_controls(self):
        status, summary, _, results, calls = self.run_case('pps-id')
        self.assertNotEqual(status, 0)
        self.assertIn('STAGE=main-pps15-whole', summary)

    def test_paff_cannot_pass_without_actual_field_pairing(self):
        status, summary, failure, _, _ = self.run_case('missing-hold')
        self.assertNotEqual(status, 0)
        self.assertIn('STAGE=h264-paff-i-whole', summary)
        self.assertIn('PAFF h264-field-hold', failure)

    def test_paff_pixels_remain_failed_with_plane_and_field_diagnostics(self):
        status, summary, failure, _, calls = self.run_case('paff-pixels')
        self.assertNotEqual(status, 0)
        self.assertIn('STAGE=h264-paff-i-whole', summary)
        self.assertIn('exact frame sizes/hashes differ', failure)
        pixels = self.diagnostics[0]['pixels']
        self.assertEqual(pixels['hardware_bytes'], 128 * 96 * 3 // 2 * 8)
        self.assertEqual(pixels['frames'][0]['planes'][0]['different_bytes'], 1)
        self.assertTrue(pixels['frames'][0]['planes'][0]['even_rows_match'])
        self.assertFalse(pixels['frames'][0]['planes'][0]['odd_rows_match'])
        self.assertEqual(sum(args[args.index('-f') + 1] == 'rawvideo' for args, _ in calls), 2)

    def test_diagnostic_failure_keeps_original_pixel_failure(self):
        status, _, failure, _, _ = self.run_case('paff-diagnostic-exit')
        self.assertNotEqual(status, 0)
        self.assertIn('exact frame sizes/hashes differ', failure)
        self.assertTrue(self.diagnostics[0]['errors'])

    def test_truncated_raw_pixels_are_not_compared_as_complete_frames(self):
        result = runner.paff_pixel_diagnostics(bytes(128), bytes(128))
        self.assertIn('error', result)
        self.assertEqual(result['frames'], [])

    def test_default_exact_reference_and_all_modes(self):
        status, summary, _, results, calls = self.run_case()
        self.assertEqual(status, 0)
        self.assertIn('PASS=58 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done', summary)
        self.assertEqual(results.count('PASS\t'), 58)
        self.assertEqual(len(calls), 80)

    def test_expbuf_disabled_environment_reaches_hardware_children(self):
        status, _, _, _, calls = self.run_case(export='0')
        self.assertEqual(status, 0)
        self.assertTrue(all(env['VAV4L2_EXPBUF_EXPORT'] == '0' for _, env in calls))

    def test_missing_adapter_evidence_cannot_pass_with_exact_pixels(self):
        status, summary, failure, _, _ = self.run_case('missing-trace')
        self.assertEqual(status, 1)
        self.assertIn('PASS=0 FAIL=1 COMPLETE=0 EXIT=1', summary)
        self.assertIn('adapter.tsv', failure)

    def test_wrong_mode_cannot_pass_with_exact_pixels(self):
        status, _, failure, _, _ = self.run_case('mode')
        self.assertEqual(status, 1)
        self.assertIn('invalid hardware/substitution evidence', failure)

    def test_wrong_driver_cannot_pass_with_exact_pixels(self):
        status, _, failure, _, _ = self.run_case('vendor')
        self.assertEqual(status, 1)
        self.assertIn('invalid hardware/substitution evidence', failure)

    def test_failed_process_cannot_pass_with_valid_trace_and_output(self):
        status, summary, failure, results, _ = self.run_case('exit')
        self.assertEqual(status, 1)
        self.assertIn('COMPLETE=0 EXIT=1 STAGE=main-fragments', summary)
        self.assertIn('command exited 8', failure)
        self.assertIn('FAIL\tmain-fragments', results)

    def test_pixel_mismatch_is_fatal_without_tolerance(self):
        status, _, failure, _, _ = self.run_case('pixels')
        self.assertEqual(status, 1)
        self.assertIn('exact frame sizes/hashes differ', failure)

    def test_duplicate_picture_cannot_replace_missing_picture(self):
        status, _, failure, _, _ = self.run_case('duplicate')
        self.assertEqual(status, 1)
        self.assertIn('invalid hardware/substitution evidence', failure)

    def test_mpeg2_changed_representation_must_match_whole_hardware(self):
        status, summary, failure, _, _ = self.run_case('mpeg2-pixels')
        self.assertEqual(status, 1)
        self.assertIn('STAGE=mpeg2-offset', summary)
        self.assertIn('exact frame sizes/hashes differ', failure)

    def test_failed_generation_stops_before_reference_and_hardware(self):
        status, summary, _, _, calls = self.run_case('generate')
        self.assertEqual(status, 1)
        self.assertIn('COMPLETE=0 EXIT=1 STAGE=main:generate', summary)
        self.assertEqual(len(calls), 1)


if __name__ == '__main__':
    unittest.main()
