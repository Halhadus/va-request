"""Exercise the real matrix JPEG stage without a VPU or FFmpeg dependency."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('run_full_matrix.sh').read_text()
UNIT_STAGE = SCRIPT[SCRIPT.index('stage=unit\n'):SCRIPT.index('\nH264_NODE=')]
JPEG_STAGE = SCRIPT[SCRIPT.index('stage=jpeg\n'):SCRIPT.index('\nstage=vpp\n')]
HEVC_STAGE = SCRIPT[SCRIPT.index('stage=hevc-packets\n'):SCRIPT.index('\nif [ "$av1" -eq 1 ]; then\n  pair_sw')]
EXPORT_STAGE = SCRIPT[SCRIPT.index('must meson compile -C "$BUILD" va_export_client\n'):SCRIPT.index('\n# Select the GStreamer factory')]
RECORD = next(line for line in SCRIPT.splitlines() if line.startswith('record_result()'))


class MatrixFactory(unittest.TestCase):
    def test_standard_factory_is_selected_by_matching_video_node(self):
        start = SCRIPT.index('pick_gst_h264() {'); end = SCRIPT.index('\n}\n', start) + 3
        function = SCRIPT[start:end]
        for node, expected in (('/dev/video127', 0), ('/dev/video3', 1)):
            with self.subTest(node=node):
                inspector = '''gst-inspect-1.0() {
  if [ "$1" = v4l2codecs ]; then printf '  v4l2slh264dec: H264 decoder\\n';
  elif [ "$1" = v4l2slh264dec ]; then printf '  video-device : Video node\\n    Default: "/dev/video127"\\n';
  else return 1; fi
}
'''
                result = subprocess.run(['bash', '-c', inspector + function + '\nH264_NODE="$1"; unset GST_H264_DECODER; pick_gst_h264', 'test', node], text=True, stdout=subprocess.PIPE)
                self.assertEqual(result.returncode, expected)
                self.assertEqual(result.stdout.strip(), 'v4l2slh264dec' if expected == 0 else '')


class MatrixJPEG(unittest.TestCase):
    def run_case(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'matrix'
            output.mkdir()
            previous = output / 'va.jpg'
            previous.write_bytes(b'previous-run')
            ff = root / 'ffmpeg'
            ff.write_text(f'#!{sys.executable}\n' + '''import os, sys
from pathlib import Path
if '-c:v' in sys.argv:
    if os.environ['JPEG_TEST_MODE'] == 'encode-fail': sys.exit(7)
    Path(sys.argv[-1]).write_bytes(b'fresh-run')
elif os.environ['JPEG_TEST_MODE'] == 'decode-fail': sys.exit(8)
''')
            ff.chmod(0o755)
            probe = root / 'ffprobe'
            probe.write_text(f'#!{sys.executable}\n' + '''import os, sys
from pathlib import Path
Path(os.environ['JPEG_TEST_PROBED']).write_text(sys.argv[-1])
if Path(sys.argv[-1]).read_bytes() != b'fresh-run': sys.exit(1)
print('mjpeg,320,240')
''')
            probe.chmod(0o755)
            environment = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                               JPEG_TEST_MODE=mode, JPEG_TEST_PROBED=str(root / 'probed'),
                               OUT=str(output), FF=str(ff), DRM='mock-drm')
            shell = 'pass=0; fail=0\n' + RECORD + '\nmust() { "$@" || exit 1; }\n' + JPEG_STAGE
            result = subprocess.run(['bash', '-c', shell], env=environment, text=True, stdout=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stdout)
            success = mode == 'success'
            self.assertEqual(previous.read_bytes(), b'fresh-run' if success else b'previous-run')
            self.assertEqual((output / 'jpeg-exit.txt').read_text().strip(), '7' if mode == 'encode-fail' else '0')
            self.assertEqual((output / 'results.tsv').read_text(), ('PASS' if success else 'FAIL') + '\tjpeg\n')
            if mode == 'encode-fail':
                self.assertFalse((root / 'probed').exists())
            else:
                self.assertIn('jpeg-current.', (root / 'probed').read_text())
            if not success:
                self.assertIn('JPEG_FAIL', result.stdout)

    def test_failed_encode_does_not_validate_old_file(self):
        self.run_case('encode-fail')

    def test_failed_decode_is_not_published(self):
        self.run_case('decode-fail')

    def test_success_publishes_only_current_image(self):
        self.run_case('success')


class MatrixUnit(unittest.TestCase):
    def run_case(self, unit_exit, probe_exit):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = root / 'build'; output = root / 'matrix'
            build.mkdir(); output.mkdir(); (build / 'meson-logs').mkdir()
            meson = root / 'meson'
            meson.write_text(f'#!{sys.executable}\n' + '''import os, sys
from pathlib import Path
logs = Path(os.environ['BUILD']) / 'meson-logs'
(logs / 'testlog.txt').write_text('current unit diagnostic')
(logs / 'testlog.json').write_text('current structured diagnostic')
print('unit stdout')
print('unit stderr', file=sys.stderr)
sys.exit(int(os.environ['TEST_UNIT_EXIT']))
''')
            meson.chmod(0o755)
            probe = build / 'test_video_probe'
            probe.write_text(f'#!{sys.executable}\n' + '''import os, sys
from pathlib import Path
Path(os.environ['OUT'], 'probe-called').touch()
print('live probe stdout')
sys.exit(int(os.environ['TEST_PROBE_EXIT']))
''')
            probe.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                       BUILD=str(build), OUT=str(output), TEST_UNIT_EXIT=str(unit_exit), TEST_PROBE_EXIT=str(probe_exit))
            shell = 'set -uo pipefail\npass=0; fail=0\n' + RECORD + '\n' + UNIT_STAGE
            result = subprocess.run(['bash', '-c', shell], env=env, text=True, stdout=subprocess.PIPE)
            success = unit_exit == 0 and probe_exit == 0
            self.assertEqual(result.returncode, 0 if success else 1, result.stdout)
            self.assertEqual((output / 'results.tsv').read_text(), ('PASS' if success else 'FAIL') + '\tunit\n')
            self.assertEqual((output / 'unit-exit.txt').read_text(), str(unit_exit) + '\n')
            self.assertEqual((output / 'unit-testlog.txt').read_text(), 'current unit diagnostic')
            self.assertEqual((output / 'unit-testlog.json').read_text(), 'current structured diagnostic')
            self.assertEqual((output / 'probe-called').exists(), unit_exit == 0)

    def test_unit_failure_keeps_diagnostics_and_skips_live_probe(self):
        self.run_case(7, 0)

    def test_live_probe_failure_is_not_hidden_by_tee(self):
        self.run_case(0, 8)

    def test_unit_success_keeps_diagnostics_and_counts_once(self):
        self.run_case(0, 0)


class MatrixPacketization(unittest.TestCase):
    def test_hevc_worker_inherits_matrix_paths_and_export_and_propagates_failure(self):
        for export, build_exit, worker_exit in (('default', 0, 0), ('0', 0, 0), ('0', 0, 9), ('0', 7, 0)):
            with self.subTest(export=export, build_exit=build_exit, worker_exit=worker_exit), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); build = root / 'alternate build'; output = root / 'matrix gbm'
                build.mkdir(); output.mkdir(); (root / 'tests').mkdir()
                meson = root / 'meson'
                meson.write_text(f'#!/bin/sh\nexit {build_exit}\n'); meson.chmod(0o755)
                (root / 'tests/hevc_slice_checks.py').write_text('''import os, sys
from pathlib import Path
assert sys.argv[1:] == [os.environ['BUILD'], os.environ['OUT'] + '/hevc-slices', 'matrix-ffmpeg', 'matrix-render']
assert os.environ.get('VAREQ_EXPBUF_EXPORT', 'default') == os.environ['TEST_EXPORT']
Path(os.environ['OUT'], 'worker-called').touch()
sys.exit(int(os.environ['TEST_WORKER_EXIT']))
''')
                env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'], ROOT=str(root), BUILD=str(build),
                    OUT=str(output), FF='matrix-ffmpeg', DRM='matrix-render', TEST_EXPORT=export, TEST_WORKER_EXIT=str(worker_exit))
                env.pop('VAREQ_EXPBUF_EXPORT', None)
                if export != 'default': env['VAREQ_EXPBUF_EXPORT'] = export
                shell = 'set -uo pipefail\npass=0; fail=0\n' + RECORD + '\nmust() { "$@" || { record_result FAIL "$stage:command"; exit 1; }; }\n' + HEVC_STAGE + '\ntest "$fail" -eq 0\n'
                result = subprocess.run(['bash', '-c', shell], env=env, text=True, stdout=subprocess.PIPE)
                success = build_exit == 0 and worker_exit == 0
                self.assertEqual(result.returncode, 0 if success else 1, result.stdout)
                self.assertEqual((output / 'worker-called').exists(), build_exit == 0)
                case = 'hevc-packets:command' if build_exit else 'hevc-packets'
                self.assertEqual((output / 'results.tsv').read_text(), ('PASS' if success else 'FAIL') + '\t' + case + '\n')

    def test_h264_requires_every_packet_marker_and_successful_client(self):
        complete = [f'PACKET_EXACT {mode} {frame}' for mode in range(1, 5) for frame in range(8)]
        retained_complete = [f'RETAINED_EXACT {frame}' for frame in range(8)]
        cases = [(markers, status, retained_complete, True) for markers, status in
            ((complete, 0), (complete[:-1], 0), (complete + complete[-1:], 0), (complete[::-1], 0), (complete, 7))]
        cases += [(complete, 0, retained, lifetime) for retained, lifetime in
            ((retained_complete[:-1], True), (retained_complete[::-1], True),
             (retained_complete + retained_complete[-1:], True), (retained_complete, False))]
        for markers, client_exit, retained, lifetime in cases:
            with self.subTest(markers=len(markers), client_exit=client_exit, retained=retained, lifetime=lifetime), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); output = root / 'matrix'; output.mkdir()
                meson = root / 'meson'; meson.write_text('#!/bin/sh\nexit 0\n'); meson.chmod(0o755)
                client = root / 'va_export_client'
                lines = ['EXPORT_EXACT 7', 'LAZY_EXACT 7'] + markers + retained + (['RETAINED_LIFETIME_EXACT'] if lifetime else [])
                client.write_text(f'#!{sys.executable}\nimport sys\nprint(' + repr('\n'.join(lines)) + f')\nsys.exit({client_exit})\n')
                client.chmod(0o755)
                env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'], BUILD=str(root), OUT=str(output), DRM='mock', CLIP=str(root))
                shell = 'set -uo pipefail\npass=0; fail=0\n' + RECORD + '\nmust() { "$@" || exit 1; }\n' + EXPORT_STAGE + '\ntest "$fail" -eq 0\n'
                result = subprocess.run(['bash', '-c', shell], env=env, text=True, stdout=subprocess.PIPE)
                success = markers == complete and client_exit == 0 and retained == retained_complete and lifetime
                self.assertEqual(result.returncode, 0 if success else 1, result.stdout)
                self.assertTrue((output / 'results.tsv').read_text().startswith(('PASS' if success else 'FAIL') + '\tva-export'))


if __name__ == '__main__':
    unittest.main()
