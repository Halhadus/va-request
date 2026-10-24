#!/usr/bin/env python3
"""Compare reconstructed packets against independent software-decoded pixels; no V4L2 hardware is emulated."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from hevc_pps_vector import rewrite_pps_id


def run(command, env=None):
    subprocess.run(command, env=env, check=True)


def main():
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        print('SKIP: optional packet pixel oracle requires FFmpeg with libx264/libx265')
        return 77
    encoders = subprocess.run([ffmpeg, '-v', 'error', '-encoders'], text=True, stdout=subprocess.PIPE, check=True).stdout
    if not all(encoder in encoders for encoder in ('libx264', 'libx265')):
        print('SKIP: optional packet pixel oracle requires libx264/libx265')
        return 77
    build, shim = map(Path, sys.argv[1:3])
    cases = [('h264-baseline', 'h264', ['-profile:v', 'baseline', '-x264-params', 'threads=1']),
             ('h264-main', 'h264', ['-profile:v', 'main', '-x264-params', 'threads=1:bframes=3']),
             ('h264-scaling-slices', 'h264', ['-profile:v', 'high', '-x264-params', 'threads=1:cqm=jvt:slices=4']),
             ('h264-mbaff', 'h264', ['-profile:v', 'high', '-x264-params', 'threads=1:interlaced=1:tff=1']),
             ('hevc-main', 'hevc', ['-x265-params', 'pools=none:frame-threads=1:log-level=error']),
             ('hevc-scaling-pps15', 'hevc', ['-x265-params', 'pools=none:frame-threads=1:log-level=error:scaling-list=default'])]
    with tempfile.TemporaryDirectory(prefix='bytestream-oracle-') as directory:
        root = Path(directory)
        for name, codec, options in cases:
            source, reconstructed = root / (name + '.' + codec), root / ('reconstructed-' + name + '.' + codec)
            run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x192:rate=8', '-frames:v', '8', '-c:v',
                 'libx264' if codec == 'h264' else 'libx265', *options, '-y', str(source)])
            if name.endswith('pps15'): rewrite_pps_id(source, 15)
            preload = str(shim)
            # A sanitizer-instrumented shim must load its runtime before libva/FFmpeg.
            if os.environ.get('VAV4L2_ORACLE_ASAN'): preload = os.environ['VAV4L2_ORACLE_ASAN'] + ':' + preload
            env = dict(os.environ, LD_PRELOAD=preload, LIBVA_DRIVER_NAME='v4l2', LIBVA_DRIVERS_PATH=str(build), VAV4L2_PACKET_ORACLE_OUT=str(reconstructed))
            run([ffmpeg, '-v', 'error', '-hwaccel', 'vaapi', '-hwaccel_device', '/dev/null', '-hwaccel_output_format', 'vaapi', '-i', str(source), '-f', 'null', '-'], env)
            pixels = []
            for label, path in [('source', source), ('reconstructed', reconstructed)]:
                output = root / (name + '-' + label + '.yuv')
                run([ffmpeg, '-v', 'error', '-i', str(path), '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-y', str(output)])
                pixels.append(output.read_bytes())
            expected = 320 * 192 * 3 // 2 * 8
            if len(pixels[0]) != expected or len(pixels[1]) != expected or pixels[0] != pixels[1]:
                raise RuntimeError(f'{name}: incomplete or mismatched software-decoded pictures')
            print(f'PASS {name}: eight exact pictures; reconstructed parameter sets and unchanged slices')
    return 0


if __name__ == '__main__':
    sys.exit(main())
