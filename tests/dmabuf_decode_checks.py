#!/usr/bin/env python3
"""Imported target pixel proof for H264, HEVC8/10, VP8 and MPEG2."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import hevc_slice_checks as packets


def validate_imports(paths, minimum):
    if len(paths) < minimum:
        raise ValueError(f'expected at least {minimum} fresh importer process traces, got {len(paths)}')
    for path in paths:
        rows = path.read_text().splitlines()
        if not rows:
            raise ValueError(f'{path}: empty import trace')
        for row in rows:
            values = row.split('\t')
            if len(values) != 5 or any(not x.isdecimal() for x in values) or any(int(x) <= 0 for x in values[:4]) or int(values[4]) != 0:
                raise ValueError(f'{path}: failed or invalid PRIME allocation: {row}')


def run(root, build, output, ffmpeg, render):
    output.mkdir(parents=True, exist_ok=True)
    current = Path(tempfile.mkdtemp(prefix='run.', dir=output))
    passed, failed, complete, stage = 0, 1, 0, 'setup'
    shim = build / 'prime_surface_interpose.so'
    env = dict(os.environ, LIBVA_DRIVER_NAME='v4l2request', LIBVA_DRIVERS_PATH=str(build), VAREQ_ADVERTISE_AV1='0',
               VAREQ_RENDER_NODE=render, VAREQ_PRIME_TRACE_DIR=str(current))
    env['LD_PRELOAD'] = str(shim) + (':' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
    with (current / 'commands.jsonl').open('w') as commands, (current / 'results.tsv').open('w') as results:
        def command(args, environment=None):
            process = subprocess.run(args, cwd=root, env=environment)
            commands.write(json.dumps({'stage': stage, 'argv': args, 'exit': process.returncode}) + '\n'); commands.flush()
            if process.returncode:
                raise ValueError(f'{stage}: command exited {process.returncode}')

        try:
            identity = {}
            identity['tree'] = subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], cwd=root, text=True).strip()
            identity['dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, text=True).strip())
            for name in ['v4l2request_drv_video.so', 'prime_surface_interpose.so', 'hevc_slice_interpose.so']:
                identity[name] = hashlib.sha256((build / name).read_bytes()).hexdigest()
            identity['export'] = env.get('VAREQ_EXPBUF_EXPORT', 'default')
            identity['capture_dmabuf'] = env.get('VAREQ_CAPTURE_DMABUF', '0')
            identity['prime_canonical'] = env.get('VAREQ_PRIME_CANONICAL', '0')
            (current / 'source.json').write_text(json.dumps(identity, indent=2) + '\n')
            stage = 'packet-imports'
            command([sys.executable, str(root / 'tests/hevc_slice_checks.py'), str(build), str(current / 'packets'), ffmpeg, render], env)
            summary = (current / 'packets/summary.txt').read_text()
            if not summary.startswith('PASS=58 FAIL=0 COMPLETE=1 EXIT=0 STAGE=done\n'):
                raise ValueError('packet worker incomplete or unexpected case count')
            passed = 58; results.write('PASS\tpacket-imports-58\n'); results.flush()
            stage = 'h264-generate'
            clip, software, hardware = current / 'h264.mp4', current / 'h264-sw.md5', current / 'h264-hw.md5'
            command([ffmpeg, '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=8:duration=1',
                     '-c:v', 'libx264', '-preset', 'fast', '-profile:v', 'main', '-x264-params', 'keyint=8:min-keyint=8:scenecut=0:bframes=2', '-frames:v', '8', '-y', str(clip)])
            command([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', str(clip), '-pix_fmt', 'nv12', '-frames:v', '8', '-f', 'framemd5', '-y', str(software)])
            stage = 'h264-import'
            command([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-hwaccel', 'vaapi', '-hwaccel_output_format', 'vaapi', '-vaapi_device', render,
                     '-i', str(clip), '-vf', 'hwdownload,format=nv12', '-pix_fmt', 'nv12', '-frames:v', '8', '-f', 'framemd5', '-y', str(hardware)], env)
            if packets.frames(software, 8) != packets.frames(hardware, 8):
                raise ValueError('H264 imported target frame hashes differ')
            passed += 1; results.write('PASS\th264-import\n'); results.flush()
            stage = 'import-evidence'
            validate_imports(list(current.glob('prime-*.tsv')), 59)
            passed += 1; results.write('PASS\timport-evidence\n'); complete, failed, stage = 1, 0, 'done'
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            (current / 'failure.txt').write_text(str(error) + '\n'); results.write(f'FAIL\t{stage}\n'); print(f'FAIL {error}', flush=True)
        summary = f'PASS={passed} FAIL={failed} COMPLETE={complete} EXIT={failed} STAGE={stage}\nRUN={current}\n'
        (current / 'summary.txt').write_text(summary); (output / 'summary.txt').write_text(summary); print(summary, end='', flush=True)
    return failed


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit('usage: dmabuf_decode_checks.py BUILD OUTPUT FFMPEG RENDER_NODE')
    raise SystemExit(run(Path(__file__).resolve().parent.parent, Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3], sys.argv[4]))
