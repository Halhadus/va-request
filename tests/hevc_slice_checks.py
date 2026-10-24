#!/usr/bin/env python3
"""Internal full-matrix worker for codec submissions and exact pixel checks."""
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from compare_framemd5 import read_frames
from hevc_pps_vector import rewrite_pps_id

FRAMES = 8
MODES = ('whole', 'offset', 'fragments', 'array', 'split-prefix')
VECTORS = (('main', 8, 1, 'hevc'), ('main10', 10, 1, 'hevc'),
    ('main-slices', 8, 2, 'hevc'), ('main10-slices', 10, 2, 'hevc'),
    ('main-pps15', 8, 1, 'hevc'), ('main10-pps15', 10, 1, 'hevc'),
    ('h264-main-slices', 8, 2, 'h264'), ('h264-high10-slices', 10, 2, 'h264'),
    ('h264-paff-i', 8, 1, 'paff'), ('h264-paff-p', 8, 1, 'paff'),
    ('vp8', 8, 1, 'vp8'), ('mpeg2', 8, 0, 'mpeg2'))
FIELDS = ['MODE', 'PICTURE', 'SLICES', 'SEGMENTS', 'STATUS', 'DRIVER', 'EXPBUF']
VENDOR = 'libva-v4l2 0.1.0'
PAFF_HASHES = {'h264-paff-i': '05692de0347095b9c771665437a2fe0e08b057bdd93c638635098872714d56ca',
    'h264-paff-p': '7dd081a6e37ccc9270aa45ef65e23ca4a38fcd211c7e7ea62dad94bda5e90c8b'}


def validate_trace(path, mode, export_mode, expected_slices=1, pictures=FRAMES):
    with path.open() as stream:
        reader = csv.DictReader(stream, delimiter='\t')
        if reader.fieldnames != FIELDS:
            raise ValueError(f'{path}: missing/invalid adapter trace header')
        rows = list(reader)
    if len(rows) != pictures:
        raise ValueError(f'{path}: expected {pictures} hardware pictures, found {len(rows)}')
    for index, row in enumerate(rows):
        if None in row or any(value is None for value in row.values()):
            raise ValueError(f'{path}: malformed trace row {index}')
        slices = int(row['SLICES'])
        ratio = 1 if mode in ('whole', 'offset') else 3
        if row['MODE'] != mode or int(row['PICTURE']) != index or slices != expected_slices or int(row['SEGMENTS']) != slices * ratio or \
           row['STATUS'] != '0' or row['DRIVER'] != VENDOR or row['EXPBUF'] != export_mode:
            raise ValueError(f'{path}: invalid hardware/substitution evidence at picture {index}')


def frames(path, depth, geometry=(320, 240)):
    records = read_frames(path, FRAMES)
    width, height = geometry
    size = width * height * 3 // (1 if depth == 10 else 2)
    if any(record[0] != size for record in records) or len(set(records)) < 2 or \
       f'#dimensions 0: {width}x{height}' not in path.read_text().splitlines():
        raise ValueError(f'{path}: wrong geometry/format or unexpectedly static output')
    return records


def load_paff_fixture(root, tag):
    hashes = PAFF_HASHES
    data = bytes.fromhex((root / 'tests/fixtures' / f'{tag.removeprefix("h264-")}.h264.hex').read_text())
    if hashlib.sha256(data).hexdigest() != hashes[tag]:
        raise ValueError(f'{tag}: PAFF fixture checksum mismatch')
    return data


def validate_field_trace(path):
    rows = [dict(field.split('=', 1) for field in line.split('\t')) for line in path.read_text().splitlines()]
    stages = {}
    for stage, count in [('h264-field-hold', FRAMES), ('h264-field-pair', FRAMES), ('submit', FRAMES * 2), ('end-picture', FRAMES * 2)]:
        records = [r for r in rows if r.get('STAGE') == stage]
        if len(records) != count or any(r.get('STATUS') != '0' for r in records):
            raise ValueError(f'{path}: missing/failed PAFF {stage} evidence')
        stages[stage] = records
    keys = lambda stage: [(r.get('FRAME'), r.get('SURFACE')) for r in stages[stage]]
    if len(set(keys('h264-field-hold'))) != FRAMES or keys('h264-field-hold') != keys('h264-field-pair') or keys('submit') != [key for key in keys('h264-field-pair') for _ in range(2)]:
        raise ValueError(f'{path}: PAFF field ownership/submission mismatch')


def paff_pixel_diagnostics(software, hardware, width=128, height=96):
    size = width * height * 3 // 2
    result = {'format': 'nv12', 'width': width, 'height': height, 'expected_frames': FRAMES,
        'software_bytes': len(software), 'hardware_bytes': len(hardware), 'frames': []}
    if len(software) != FRAMES * size or len(hardware) != FRAMES * size:
        result['error'] = 'incomplete raw diagnostic output'
        return result
    for index in range(FRAMES):
        sw, hw = software[index * size:(index + 1) * size], hardware[index * size:(index + 1) * size]
        planes = []
        for name, start, rows in [('Y', 0, height), ('UV', width * height, height // 2)]:
            a, b = sw[start:start + width * rows], hw[start:start + width * rows]
            field = lambda data, parity: b''.join(data[y * width:(y + 1) * width] for y in range(parity, rows, 2))
            planes.append({'plane': name, 'different_bytes': sum(x != y for x, y in zip(a, b)),
                'software_sha256': hashlib.sha256(a).hexdigest(), 'hardware_sha256': hashlib.sha256(b).hexdigest(),
                'even_rows_match': field(a, 0) == field(b, 0), 'odd_rows_match': field(a, 1) == field(b, 1)})
        result['frames'].append({'frame': index, 'planes': planes})
    return result


def save_paff_failure(run, stage, clip, ffmpeg, render, environment, command):
    # Rerun only the failing synthetic vector. Preserve the original failure;
    # diagnostics must never turn a pixel mismatch into PASS or SKIP.
    prefix = run / f'{stage}-diagnostic'
    report = {'kind': 'separate diagnostic rerun', 'errors': []}
    software, hardware = Path(f'{prefix}-sw.pixels.bin'), Path(f'{prefix}-hw.pixels.bin')
    env = dict(environment, VAV4L2_HEVC_PACKET_LOG=f'{prefix}-adapter.tsv',
        VAV4L2_DECODE_TRACE=f'{prefix}-decode.tsv', VAV4L2_STORAGE_TRACE=f'{prefix}-storage.tsv')
    for target, accelerated in [(software, False), (hardware, True)]:
        args = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror']
        if accelerated: args += ['-hwaccel', 'vaapi', '-hwaccel_output_format', 'vaapi', '-vaapi_device', render]
        args += ['-i', str(clip)]
        if accelerated: args += ['-vf', 'hwdownload,format=nv12']
        args += ['-pix_fmt', 'nv12', '-frames:v', str(FRAMES), '-f', 'rawvideo', '-y', str(target)]
        try:
            command(args, env if accelerated else environment)
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            report['errors'].append(str(error))
    try:
        report['pixels'] = paff_pixel_diagnostics(software.read_bytes(), hardware.read_bytes())
    except OSError as error:
        report['errors'].append(str(error))
    Path(f'{prefix}.json').write_text(json.dumps(report, indent=2) + '\n')



def mpeg2_slice_count(path):
    counts = []
    for unit in path.read_bytes().split(b'\x00\x00\x01')[1:]:
        if not unit:
            continue
        if unit[0] == 0:
            counts.append(0)
        elif 1 <= unit[0] <= 0xaf and counts:
            counts[-1] += 1
    if len(counts) != FRAMES or not counts[0] or any(n != counts[0] for n in counts):
        raise ValueError(f'{path}: inconsistent MPEG2 picture/slice counts')
    return counts[0]


def run_checks(root, build, output, ffmpeg, render):
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='run.', dir=output))
    pass_count, fail_count, complete, exit_code, stage = 0, 0, 0, 1, 'setup'
    environment = dict(os.environ, LIBVA_DRIVER_NAME='v4l2', LIBVA_DRIVERS_PATH=str(build), VAV4L2_ADVERTISE_AV1='0')
    shim = build / 'hevc_slice_interpose.so'
    driver = build / 'v4l2_drv_video.so'
    with (run / 'results.tsv').open('w') as results, (run / 'commands.jsonl').open('w') as commands:
        results.write('RESULT\tCASE\n'); results.flush()

        def command(args, env=environment):
            process = subprocess.run(args, cwd=root, env=env)
            commands.write(json.dumps({'stage': stage, 'argv': args, 'exit': process.returncode}) + '\n'); commands.flush()
            if process.returncode:
                raise ValueError(f'{stage}: command exited {process.returncode}')

        try:
            identity = subprocess.run(['git', 'rev-parse', 'HEAD^{tree}'], cwd=root, text=True, stdout=subprocess.PIPE, check=True)
            (run / 'source.json').write_text(json.dumps({'tree': identity.stdout.strip(), 'driver_sha256': hashlib.sha256(driver.read_bytes()).hexdigest(),
                'adapter_sha256': hashlib.sha256(shim.read_bytes()).hexdigest(), 'export': environment.get('VAV4L2_EXPBUF_EXPORT', 'default')}, indent=2) + '\n')
            for tag, depth, slices, codec in VECTORS:
                clip, reference = run / f'{tag}.{"ivf" if codec == "vp8" else "m2v" if codec == "mpeg2" else "h264" if codec in ("h264", "paff") else "hevc"}', run / f'{tag}-sw.md5'
                fmt, software_fmt = ('p010le', 'yuv420p10le') if depth == 10 else ('nv12', 'yuv420p')
                geometry = (128, 96) if codec == 'paff' else (320, 240)
                stage = f'{tag}:generate'
                source = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=8:duration=1']
                if codec == 'paff':
                    clip.write_bytes(load_paff_fixture(root, tag))
                elif codec == 'mpeg2':
                    command(source + ['-pix_fmt', software_fmt, '-c:v', 'mpeg2video', '-g', '8', '-bf', '2',
                        '-threads', '1', '-frames:v', str(FRAMES), '-f', 'mpeg2video', '-y', str(clip)])
                    slices = mpeg2_slice_count(clip)
                elif codec == 'vp8':
                    command(source + ['-pix_fmt', software_fmt, '-c:v', 'libvpx', '-deadline', 'good', '-cpu-used', '4', '-g', '8',
                        '-lag-in-frames', '0', '-auto-alt-ref', '0', '-frames:v', str(FRAMES), '-f', 'ivf', '-y', str(clip)])
                elif codec == 'h264':
                    command(source + ['-pix_fmt', software_fmt, '-c:v', 'libx264', '-preset', 'medium', '-profile:v', 'high10' if depth == 10 else 'main', '-x264-params',
                        f'keyint=8:min-keyint=8:scenecut=0:open-gop=0:repeat-headers=1:aud=1:slices={slices}:ref=4:bframes=2:b-adapt=0:b-pyramid=normal:weightp=2:weightb=1:threads=1',
                        '-frames:v', str(FRAMES), '-f', 'h264', '-y', str(clip)])
                else:
                    command(source + ['-pix_fmt', software_fmt, '-c:v', 'libx265', '-preset', 'ultrafast', '-profile:v', 'main10' if depth == 10 else 'main', '-x265-params',
                        f'keyint=8:min-keyint=8:scenecut=0:open-gop=0:repeat-headers=1:aud=1:slices={slices}:pools=1:frame-threads=1:log-level=error',
                        '-frames:v', str(FRAMES), '-f', 'hevc', '-y', str(clip)])
                if tag.endswith('-pps15'): rewrite_pps_id(clip)
                stage = f'{tag}:software'
                command([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror', '-i', str(clip), '-pix_fmt', fmt,
                    '-frames:v', str(FRAMES), '-f', 'framemd5', '-y', str(reference)])
                expected = frames(reference, depth, geometry)
                baseline = None
                for mode in MODES if codec in ('hevc', 'h264', 'paff') else MODES[:4]:
                    stage = f'{tag}-{mode}'
                    actual, trace = run / f'{stage}-hw.md5', run / f'{stage}-adapter.tsv'
                    preload = str(shim) + (':' + environment['LD_PRELOAD'] if environment.get('LD_PRELOAD') else '')
                    instrumented = dict(environment, LD_PRELOAD=preload, VAV4L2_HEVC_PACKET_MODE=mode, VAV4L2_HEVC_PACKET_LOG=str(trace),
                        VAV4L2_DECODE_TRACE=str(run / f'{stage}-decode.tsv'), VAV4L2_STORAGE_TRACE=str(run / f'{stage}-storage.tsv'))
                    command([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror', '-hwaccel', 'vaapi', '-hwaccel_output_format', 'vaapi',
                        '-vaapi_device', render, '-i', str(clip), '-vf', f'hwdownload,format={fmt}', '-pix_fmt', fmt,
                        '-frames:v', str(FRAMES), '-f', 'framemd5', '-y', str(actual)], instrumented)
                    validate_trace(trace, mode, environment.get('VAV4L2_EXPBUF_EXPORT', 'default'), slices, FRAMES * (2 if codec == 'paff' else 1))
                    if codec == 'paff': validate_field_trace(Path(instrumented['VAV4L2_DECODE_TRACE']))
                    if tag.endswith('-pps15'):
                        rows = [dict(field.split('=', 1) for field in line.split('\t')) for line in Path(instrumented['VAV4L2_DECODE_TRACE']).read_text().splitlines()]
                        headers = [row for row in rows if row.get('STAGE') == 'hevc-header']
                        if len(headers) != FRAMES or any(row.get('STATUS') != '0' or row.get('ARG') != '15' for row in headers):
                            raise ValueError(f'{stage}: missing actual nonzero PPS hardware controls')
                    observed = frames(actual, depth, geometry)
                    # MPEG2's kernel IDCT differs from FFmpeg's. Its unchanged full
                    # matrix cases keep the GStreamer oracle; representations here
                    # must match whole-slice hardware exactly.
                    if (codec != 'mpeg2' and observed != expected) or (baseline is not None and observed != baseline):
                        if codec == 'paff': save_paff_failure(run, stage, clip, ffmpeg, render, instrumented, command)
                        raise ValueError(f'{stage}: exact frame sizes/hashes differ from software or whole-slice hardware')
                    if baseline is None:
                        baseline = observed
                    results.write(f'PASS\t{stage}\n'); results.flush(); pass_count += 1
                    print(f'PASS {stage}', flush=True)
            complete, exit_code, stage = 1, 0, 'done'
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            results.write(f'FAIL\t{stage}\n'); results.flush(); fail_count = 1
            (run / 'failure.txt').write_text(str(error) + '\n')
            print(f'FAIL {error}', flush=True)
        finally:
            summary = f'PASS={pass_count} FAIL={fail_count} COMPLETE={complete} EXIT={exit_code} STAGE={stage}\nRUN={run}\n'
            (run / 'summary.txt').write_text(summary); (output / 'summary.txt').write_text(summary)
            print(summary, end='', flush=True)
    return exit_code


def main():
    if len(sys.argv) != 5:
        print('usage: hevc_slice_checks.py BUILD OUTPUT FFMPEG RENDER', file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parent.parent
    return run_checks(root, Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3], sys.argv[4])


if __name__ == '__main__':
    raise SystemExit(main())
