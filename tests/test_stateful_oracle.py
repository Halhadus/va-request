#!/usr/bin/env python3
"""Compare reconstructed packets against independent software-decoded pixels; no V4L2 hardware is emulated."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from hevc_pps_vector import rewrite_pps_id
from h264_poc_vector import rewrite_poc_type1
from vp9_hidden_vector import hide_key_frames
from encoder_pool import x265_pool


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
    cases += [('h264-poc1-cavlc', 'h264', ['-profile:v', 'baseline', '-x264-params', 'threads=1:bframes=0:slices=4']),
              ('h264-poc1-cabac', 'h264', ['-profile:v', 'main', '-x264-params', 'threads=1:bframes=0:slices=4']),
              ('hevc-main10', 'hevc', ['-pix_fmt', 'yuv420p10le', '-x265-params', 'pools=none:frame-threads=1:log-level=error']),
              ('hevc-temporal', 'hevc', ['-x265-params', 'pools=none:frame-threads=1:log-level=error:temporal-layers=1'])]
    cases += [('hevc-multiple-slices', 'hevc', ['-x265-params', f'pools={x265_pool()}:frame-threads=1:log-level=error:slices=2'])]
    if 'libvpx-vp9' in encoders:
        cases += [('vp9-profile0', 'vp9', ['-deadline', 'realtime', '-cpu-used', '8', '-lag-in-frames', '0']),
                  ('vp9-profile2', 'vp9', ['-pix_fmt', 'yuv420p10le', '-deadline', 'realtime', '-cpu-used', '8', '-lag-in-frames', '0'])]
        cases += [('vp9-hidden-profile0', 'vp9', ['-g', '1', '-deadline', 'realtime', '-cpu-used', '8', '-lag-in-frames', '0']),
                  ('vp9-hidden-profile2', 'vp9', ['-g', '1', '-pix_fmt', 'yuv420p10le', '-deadline', 'realtime', '-cpu-used', '8', '-lag-in-frames', '0'])]
    if 'libaom-av1' in encoders:
        cases += [('av1-grain', 'av1', ['-cpu-used', '8', '-threads', '1', '-lag-in-frames', '0', '-aom-params', 'film-grain-test=1']),
                  ('av1-main', 'av1', ['-cpu-used', '8', '-threads', '1', '-lag-in-frames', '0']),
                  ('av1-main10', 'av1', ['-pix_fmt', 'yuv420p10le', '-cpu-used', '8', '-threads', '1', '-lag-in-frames', '0']),
                  ('av1-tiles', 'av1', ['-cpu-used', '8', '-threads', '1', '-lag-in-frames', '0', '-tiles', '2x2'])]
    with tempfile.TemporaryDirectory(prefix='bytestream-oracle-') as directory:
        root = Path(directory)
        for name, codec, options in cases:
            source, reconstructed = root / (name + '.' + codec), root / ('reconstructed-' + name + '.' + codec)
            run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x192:rate=8', '-frames:v', '8', '-c:v',
                 'libx264' if codec == 'h264' else 'libvpx-vp9' if codec == 'vp9' else 'libaom-av1' if codec == 'av1' else 'libx265', *options,
                 *(['-f', 'ivf'] if codec == 'vp9' else ['-f', 'obu'] if codec == 'av1' else []), '-y', str(source)])
            if name.startswith('vp9-hidden'): hide_key_frames(source)
            if name.startswith('h264-poc1'): rewrite_poc_type1(source)
            if name.endswith('pps15'): rewrite_pps_id(source, 15)
            preload = str(shim)
            # A sanitizer-instrumented shim must load its runtime before libva/FFmpeg.
            if os.environ.get('VAV4L2_ORACLE_ASAN'): preload = os.environ['VAV4L2_ORACLE_ASAN'] + ':' + preload
            env = dict(os.environ, LD_PRELOAD=preload, LIBVA_DRIVER_NAME='v4l2', LIBVA_DRIVERS_PATH=str(build), VAV4L2_PACKET_ORACLE_OUT=str(reconstructed), VAV4L2_ADVERTISE_AV1='1')
            run([ffmpeg, '-v', 'error', '-hwaccel', 'vaapi', '-hwaccel_device', '/dev/null', '-hwaccel_output_format', 'vaapi', '-i', str(source), '-f', 'null', '-'], env)
            if codec == 'vp9':
                data = reconstructed.read_bytes(); cursor = 32; packets = 0
                while cursor < len(data):
                    if cursor + 12 > len(data): raise RuntimeError('truncated reconstructed IVF header')
                    length = int.from_bytes(data[cursor:cursor + 4], 'little'); cursor += 12
                    if not length or length > len(data) - cursor: raise RuntimeError('invalid reconstructed IVF frame')
                    cursor += length; packets += 1
                if packets != (16 if name.startswith('vp9-hidden') else 8):
                    raise RuntimeError(f'{name}: incorrect reconstructed coded-frame count {packets}')
            pixels = []
            pixel_format = 'yuv420p10le' if name in ('hevc-main10', 'vp9-profile2', 'vp9-hidden-profile2', 'av1-main10') else 'yuv420p'
            for label, path in [('source', source), ('reconstructed', reconstructed)]:
                output = root / (name + '-' + label + '.yuv')
                run([ffmpeg, '-v', 'error', '-i', str(path), '-pix_fmt', pixel_format, '-fps_mode', 'passthrough', '-f', 'rawvideo', '-y', str(output)])
                pixels.append(output.read_bytes())
            expected = 320 * 192 * 3 // 2 * 8 * (2 if pixel_format == 'yuv420p10le' else 1)
            if len(pixels[0]) != expected or len(pixels[1]) != expected or pixels[0] != pixels[1]:
                raise RuntimeError(f'{name}: incomplete or mismatched software-decoded pictures ({len(pixels[0])}/{len(pixels[1])}, expected {expected})')
            print(f'PASS {name}: eight exact pictures; reconstructed packets')
    return 0


if __name__ == '__main__':
    sys.exit(main())
