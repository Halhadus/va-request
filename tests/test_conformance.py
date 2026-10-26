import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('conformance', Path(__file__).parents[1] / 'tools/hardware.py')
conformance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conformance)
spec = importlib.util.spec_from_file_location('report_runner', Path(__file__).parents[1] / 'tools/test.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Conformance(unittest.TestCase):
    def test_corpus_uses_standard_report_and_retains_lost_pass_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'sample.h264').write_bytes(b'source')
            vector = self.vector(); vector.pop('status'); vector.update(path='sample.h264', sha256=conformance.digest(root / 'sample.h264'))
            manifest = root / 'corpus.json'; manifest.write_text(json.dumps(dict(schema=conformance.CORPUS_SCHEMA, origin='fixture', vectors=[vector])))
            previous = root / 'previous.json'
            previous.write_text(json.dumps(dict(schema=runner.SCHEMA, profile='conformance', suites=[dict(name='conformance', cases=[dict(vector, status='PASS')])])))
            def execute(name, command, output, env=None):
                if name == 'conformance-capabilities': (output / (name + '.log')).write_text('{"profiles": []}')
                return dict(name=name, status='PASS', exit_code=0, duration_s=0)
            for compare in (False, True):
                with patch.object(runner, 'source_identity', return_value={}), patch.object(runner.shutil, 'which', return_value='/tool'), patch.object(runner, 'execute', side_effect=execute):
                    code = runner.main(['--suite', 'conformance', '--manifest', str(manifest), '--build-dir', str(root / 'build'),
                        '--output-dir', str(root / str(compare)), *(['--previous', str(previous)] if compare else [])])
                report = json.loads(next((root / str(compare)).glob('run-*/report.json')).read_text())
                self.assertEqual(code, int(compare))
                self.assertEqual(report['schema'], runner.SCHEMA)
                self.assertEqual(report['summary']['skip'], 1)
                self.assertEqual(report['summary']['fail'], int(compare))
                self.assertEqual(len(list((root / str(compare)).glob('libva-v4l2-run-*.tar.gz'))), 1)

    def test_pixels_require_full_output_and_driver_completions(self):
        vector = self.vector(); vector.pop('status'); vector['source'] = '/fixture.h264'
        pixels = b''.join(bytes([i]) * 384 for i in range(8))
        for mode in ('exact', 'partial', 'error', 'no-trace', 'mismatch'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                def execute(name, command, env):
                    hardware = name.endswith('-hardware')
                    if hardware:
                        self.assertIn('-hwaccel_output_format', command); self.assertIn('hwdownload,format=nv12', command)
                        if mode != 'no-trace': Path(env['VAV4L2_DECODE_TRACE']).write_text('STAGE=submit-done\tSTATUS=0\n' * 8)
                    data = pixels[:-1] if hardware and mode == 'partial' else b'x' + pixels[1:] if hardware and mode == 'mismatch' else pixels
                    Path(command[-1]).write_bytes(data)
                    return dict(exit_code=2 if hardware and mode == 'error' else 0, duration_s=0)
                result = conformance.run_vector(execute, vector, Path(directory), {}, '/render', '/ffmpeg')
                self.assertEqual(result['status'], 'PASS' if mode == 'exact' else 'FAIL')
                self.assertTrue((Path(directory) / 'sample-va.yuv').is_file())

    def test_repeat_keeps_one_session_and_checks_every_picture(self):
        vector = self.vector(); vector.pop('status'); vector['source'] = '/fixture.h264'
        with tempfile.TemporaryDirectory() as directory:
            def execute(name, command, env):
                self.assertEqual(command[command.index('-stream_loop') + 1], '2')
                Path(command[-1]).write_bytes(bytes(16*16*3//2*8*3))
                if name.endswith('-hardware'): Path(env['VAV4L2_DECODE_TRACE']).write_text('STAGE=submit-done\tSTATUS=0\n'*24)
                return dict(exit_code=0, duration_s=0)
            result = conformance.run_vector(execute, vector, Path(directory), {}, '/render', '/ffmpeg', 3)
            self.assertEqual(result['status'], 'PASS'); self.assertEqual(result['hardware_frames'], 24)
            self.assertEqual(result['iterations'], 3)
        old = self.vector(); old['iterations'] = 3
        self.assertEqual(conformance.lost_passes(dict(cases=[old]), dict(cases=[self.vector()])), ['sample'])

    def test_large_trace_is_consumed_incrementally(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'decode.tsv'
            with trace.open('w') as file:
                for i in range(10000): file.write('STAGE=submit-done\tSTATUS=0\n')
            with patch.object(Path, 'read_text', side_effect=AssertionError('whole-file trace read')):
                self.assertEqual(conformance.completions(trace), {'stateful': 0, 'stateless': 10000})

    def vector(self, name='sample', status='PASS'):
        return dict(name=name, status=status, sha256='a' * 64, width=16, height=16, frames=8, format='nv12', profile='VAProfileH264High')

    def test_equal_totals_cannot_hide_lost_pass(self):
        previous = dict(cases=[self.vector('old'), self.vector('new', 'FAIL')])
        current = dict(cases=[self.vector('old', 'FAIL'), self.vector('new')])
        self.assertEqual(conformance.lost_passes(previous, current), ['old'])
        current['cases'][0]['status'] = 'SKIP'
        self.assertEqual(conformance.lost_passes(previous, current), ['old'])

    def test_changed_source_or_deleted_case_is_a_lost_pass(self):
        previous = dict(cases=[self.vector()])
        self.assertEqual(conformance.lost_passes(previous, dict(cases=[])), ['sample'])
        changed = self.vector(); changed['sha256'] = 'b' * 64
        self.assertEqual(conformance.lost_passes(previous, dict(cases=[changed])), ['sample'])

    def test_manifest_hash_and_trace_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'sample.h264').write_bytes(b'source')
            vector = self.vector(); vector.pop('status'); vector.update(path='sample.h264', sha256=conformance.digest(root / 'sample.h264'))
            manifest = root / 'corpus.json'; manifest.write_text(json.dumps(dict(schema=conformance.CORPUS_SCHEMA, origin='generated fixture', vectors=[vector])))
            self.assertEqual(len(conformance.load_vectors(manifest)['vectors']), 1)
            (root / 'sample.h264').write_bytes(b'changed')
            with self.assertRaises(ValueError): conformance.load_vectors(manifest)
            trace = root / 'decode.tsv'
            trace.write_text('STAGE=stateful-capture-done\tSTATUS=0\nSTAGE=submit-done\tSTATUS=0\nSTAGE=submit-done\tSTATUS=-1\n')
            self.assertEqual(conformance.completions(trace), {'stateful': 1, 'stateless': 1})


if __name__ == '__main__': unittest.main()
