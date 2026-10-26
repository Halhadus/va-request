#!/usr/bin/env python3
"""Capability-selected decode, processing and buffer acceptance checks."""
import hashlib
import json
import re
import os
from pathlib import Path
import shutil
import tarfile

# profile, encoder, optional encoder settings. The stream has eight changing frames.
CODECS = {
    "h264": ("VAProfileH264High", "libx264", ["-profile:v", "high"]),
    "hevc": ("VAProfileHEVCMain", "libx265", ["-x265-params", "pools=1:frame-threads=1:log-level=error"]),
    "hevc10": ("VAProfileHEVCMain10", "libx265", ["-x265-params", "pools=1:frame-threads=1:log-level=error"]),
    "vp9": ("VAProfileVP9Profile0", "libvpx-vp9", ["-deadline", "realtime", "-cpu-used", "8", "-lag-in-frames", "0"]),
    "vp9-10": ("VAProfileVP9Profile2", "libvpx-vp9", ["-deadline", "realtime", "-cpu-used", "8", "-lag-in-frames", "0"]),
    "vp8": ("VAProfileVP8Version0_3", "libvpx", []),
    "mpeg2": ("VAProfileMPEG2Main", "mpeg2video", ["-g", "4"]),
    "av1": ("VAProfileAV1Profile0", "libaom-av1", ["-cpu-used", "8", "-lag-in-frames", "0"]),
    "av1-10": ("VAProfileAV1Profile0", "libaom-av1", ["-cpu-used", "8", "-lag-in-frames", "0"]),
}
FRAMES = 8
WIDTH, HEIGHT = 320, 192
ACCEPTANCE_GROUPS = ("features", "extended", "rows", "complete", "matrix", "imported", "direct-capture", "renderer", "transport")
ACCEPTANCE_REQUIREMENTS = {
    "processing": "JPEG encode and VPP; NV12/NV21/YU12/NV16/BA24/AR24/XR24/RGBP raw menus and rotate/flip controls",
    "packets": "H.264 Main, HEVC Main/Main10, VP8 and MPEG-2 Main decode",
    "matrix": "Processing and packet prerequisites; H.264 Baseline/High/High10/High422 and MPEG-2 Simple decode",
    "renderer": "Built EGL/GLES acceptance client; runtime format and modifier queries determine individual skips",
    "transport": "Discovered raw M2M node and built transport client",
}


def acceptance_plan(capabilities, inventory):
    entries = {(item["profile"], item["entrypoint"]) for item in capabilities["profiles"]}
    decode = {profile for profile, entry in entries if entry == "VAEntrypointVLD"}
    jpeg = ("VAProfileJPEGBaseline", "VAEntrypointEncPicture") in entries
    vpp = ("VAProfileNone", "VAEntrypointVideoProc") in entries
    raw = [node for node in inventory.get("nodes", []) if "raw M2M candidate" in node.get("roles", [])]
    menus = "\n".join(section.get("stdout", "") for node in raw for section in node.get("sections", []))
    formats = ("NV12", "NV21", "YU12", "NV16", "BA24", "AR24", "XR24", "RGBP")
    processing = jpeg and vpp and all("'" + name + "'" in menus for name in formats) and all(name in menus for name in ("rotate", "horizontal_flip", "vertical_flip"))
    packets = {"VAProfileH264Main", "VAProfileHEVCMain", "VAProfileHEVCMain10", "VAProfileVP8Version0_3", "VAProfileMPEG2Main"} <= decode
    matrix = processing and packets and {"VAProfileH264ConstrainedBaseline", "VAProfileH264High", "VAProfileH264High10", "VAProfileH264High422", "VAProfileMPEG2Simple"} <= decode
    return {"features": processing, "extended": processing, "rows": processing, "complete": processing, "matrix": matrix,
            "imported": packets, "direct-capture": packets, "renderer": True, "transport": bool(raw)}


def acceptance_cases(path):
    cases = []
    with tarfile.open(path) as archive:
        for member in archive.getmembers():
            if not member.isfile() or not member.name.endswith(".tsv"): continue
            for number, line in enumerate(archive.extractfile(member).read().decode().splitlines(), 1):
                row = line.split("\t")
                if len(row) < 2: continue
                if row[0] in ("PASS", "FAIL", "SKIP"): status, name = row[:2]
                elif row[1] in ("PASS", "FAIL", "SKIP"): name, status = row[:2]
                else: continue
                label = member.name.rsplit("/", 1)[0] + "/" + name if member.name.endswith("/results.tsv") else member.name + ":" + str(number) + ":" + name
                cases.append({"name": label, "status": status,
                    "exit_code": 0 if status == "PASS" else 77 if status == "SKIP" else 1, "duration_s": 0})
    if not cases: raise ValueError("acceptance report contains no case results")
    return cases


def skip(name, reason):
    return {"name": name, "status": "SKIP", "exit_code": 77, "duration_s": 0, "reason": reason}


def select_codecs(capabilities):
    supported = {item["profile"] for item in capabilities["profiles"] if item["entrypoint"] == "VAEntrypointVLD"}
    return [name for name, (profile, _, _) in CODECS.items() if profile in supported]


def compare_pixels(actual, reference, codec):
    size = WIDTH * HEIGHT * 3 // 2 * FRAMES * (2 if codec in ("hevc10", "vp9-10", "av1-10") else 1)
    if len(actual) != size or len(reference) != size:
        raise ValueError(f"expected {FRAMES} complete raw frames ({size} bytes), got {len(actual)}/{len(reference)}")
    frame_size = size // FRAMES
    if len({actual[start:start + frame_size] for start in range(0, size, frame_size)}) < 2:
        raise ValueError("decoded frames are static")
    if codec == "mpeg2":
        # Hardware IDCT implementations can round differently from the CPU IDCT.
        errors = [abs(a - b) for a, b in zip(actual, reference)]
        if max(errors) > 4 or sum(errors) / size > 0.5:
            raise ValueError("MPEG-2 pixels exceed IDCT tolerance (max 4, mean 0.5)")
    elif actual != reference:
        raise ValueError("decoded pixels differ from software reference")


def run_hardware(run, output, build, render_node, extended=True):
    env = dict(os.environ, LIBVA_DRIVER_NAME="v4l2", LIBVA_DRIVERS_PATH=str(build), VAV4L2_RENDER_NODE=render_node)
    cases = []
    client = build / "va_capabilities"
    if not client.is_file():
        return [skip("va-capabilities", "VA test client was not built")]
    probe = run("va-capabilities", [str(client), render_node], env)
    if probe["status"] != "PASS": return [probe]
    capabilities = json.loads((output / "va-capabilities.log").read_text())
    (output / "va-capabilities.json").write_text(json.dumps(capabilities, indent=2) + "\n")
    selected = select_codecs(capabilities)
    for codec, (_, encoder, options) in CODECS.items():
        name = "decode-" + codec
        if codec not in selected:
            cases.append(skip(name, "VA decode profile unavailable")); continue
        if not shutil.which("ffmpeg"):
            cases.append(skip(name, "ffmpeg is not installed")); continue
        ten_bit = codec in ("hevc10", "vp9-10", "av1-10")
        raw_format = "p010le" if ten_bit else "nv12"
        clip = output / (codec + ".mkv")
        encode = run(name + "-generate", ["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
            f"testsrc2=size={WIDTH}x{HEIGHT}:rate=25", "-frames:v", str(FRAMES), "-c:v", encoder,
            "-threads", "1", *options, "-pix_fmt", "yuv420p10le" if ten_bit else "yuv420p", "-y", str(clip)], env)
        if encode["status"] != "PASS": cases.append(encode); continue
        reference, actual = output / (codec + "-cpu.yuv"), output / (codec + "-va.yuv")
        cpu = run(name + "-reference", ["ffmpeg", "-nostdin", "-v", "error", "-hwaccel", "none", "-i", str(clip),
            "-pix_fmt", raw_format, "-f", "rawvideo", "-y", str(reference)], env)
        if cpu["status"] != "PASS": cases.append(cpu); continue
        decode_env = dict(env, VAV4L2_DECODE_TRACE=str(output / (codec + "-decode.tsv")))
        hardware = run(name, ["ffmpeg", "-nostdin", "-v", "error", "-hwaccel", "vaapi", "-hwaccel_device", render_node,
            "-hwaccel_output_format", "vaapi", "-i", str(clip), "-vf", "hwdownload,format=" + raw_format, "-f", "rawvideo", "-y", str(actual)], decode_env)
        if hardware["status"] == "PASS":
            try: compare_pixels(actual.read_bytes(), reference.read_bytes(), codec)
            except (OSError, ValueError) as error:
                hardware.update(status="FAIL", exit_code=1, reason=str(error))
        cases.append(hardware)
    if extended:
        cases.extend(run_processing(run, output, env, capabilities))
        inventory = json.loads((output / "capabilities.json").read_text()) if (output / "capabilities.json").is_file() else {}
        plan = acceptance_plan(capabilities, inventory)
        clients = {"features": "va_feature_client", "extended": "va_dmabuf_extended", "rows": "va_dmabuf_rows", "complete": "va_dmabuf_complete",
            "matrix": "va_export_client", "imported": "prime_surface_interpose.so", "direct-capture": "prime_surface_interpose.so", "renderer": "va_dmabuf_renderer", "transport": "vpp_dmabuf_probe"}
        selected = []
        for group in ACCEPTANCE_GROUPS:
            if not plan[group]: cases.append(skip("acceptance-" + group, "fixture prerequisites unavailable; see acceptance-plan.json"))
            elif not (build / clients[group]).is_file(): cases.append(skip("acceptance-" + group, "acceptance client was not built"))
            else: selected.append(group)
        (output / "acceptance-plan.json").write_text(json.dumps({"groups": plan, "selected": selected, "requirements": ACCEPTANCE_REQUIREMENTS}, indent=2) + "\n")
        if selected:
            env.update(VAV4L2_BUILD_DIR=str(build), VAV4L2_REPORT_FILE=str(output / "hardware.tar.gz"), VAV4L2_RENDER_NODE=render_node)
            acceptance = run("acceptance", ["bash", "tests/run_dmabuf_completion.sh", *selected], env)
            if (output / "hardware.tar.gz").is_file():
                acceptance["cases"] = acceptance_cases(output / "hardware.tar.gz")
                if any(case["status"] == "FAIL" for case in acceptance["cases"]): acceptance.update(status="FAIL", exit_code=1)
            cases.append(acceptance)
    return cases


def run_processing(run, output, env, capabilities):
    entries = {(item["profile"], item["entrypoint"]) for item in capabilities["profiles"]}
    cases = []
    for name, profile, entry, filters, encoder in (
        ("jpeg", "VAProfileJPEGBaseline", "VAEntrypointEncPicture", "format=nv12,hwupload", "mjpeg_vaapi"),
        ("vpp", "VAProfileNone", "VAEntrypointVideoProc", f"format=nv12,hwupload,scale_vaapi=w={WIDTH}:h={HEIGHT}:format=nv12,hwdownload,format=nv12", "rawvideo")):
        if (profile, entry) not in entries: cases.append(skip(name, "VA entrypoint unavailable")); continue
        if not shutil.which("ffmpeg"): cases.append(skip(name, "ffmpeg is not installed")); continue
        actual = output / (name + "-pixels.yuv")
        target = output / "jpeg.mkv" if name == "jpeg" else actual
        command = ["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-vaapi_device", env["VAV4L2_RENDER_NODE"], "-f", "lavfi", "-i",
            f"testsrc2=size={WIDTH}x{HEIGHT}:rate=25", "-frames:v", str(FRAMES), "-vf", filters, "-c:v", encoder]
        if name == "vpp": command.extend(["-f", "rawvideo"])
        result = run(name, [*command, "-y", str(target)], env)
        if result["status"] == "PASS" and name == "jpeg":
            result = run("jpeg-readback", ["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-i", str(target), "-pix_fmt", "nv12", "-f", "rawvideo", "-y", str(actual)], env)
        if result["status"] == "PASS" and name == "jpeg":
            pixels = actual.read_bytes(); frame_size = WIDTH * HEIGHT * 3 // 2
            if len(pixels) != frame_size * FRAMES or len({pixels[i:i + frame_size] for i in range(0, len(pixels), frame_size)}) < 2:
                result.update(status="FAIL", exit_code=1, reason="JPEG readback is truncated or static")
        elif result["status"] == "PASS":
            reference = output / (name + "-reference.yuv")
            cpu = run(name + "-reference", ["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=size={WIDTH}x{HEIGHT}:rate=25",
                "-frames:v", str(FRAMES), "-pix_fmt", "nv12", "-f", "rawvideo", "-y", str(reference)], env)
            if cpu["status"] == "PASS":
                try: compare_pixels(actual.read_bytes(), reference.read_bytes(), name)
                except (OSError, ValueError) as error: result.update(status="FAIL", exit_code=1, reason=str(error))
        cases.append(result)
    return cases


CORPUS_SCHEMA = "libva-v4l2.conformance/v1"


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''): h.update(data)
    return h.hexdigest()


def load_vectors(manifest):
    record = json.loads(manifest.read_text())
    if not isinstance(record, dict) or record.get('schema') != CORPUS_SCHEMA or not isinstance(record.get('origin'), str) or not record['origin'] or not isinstance(record.get('vectors'), list) or not record['vectors']:
        raise ValueError('manifest requires schema, corpus origin and vectors')
    names = set()
    for v in record['vectors']:
        if not isinstance(v, dict) or not {'name', 'path', 'sha256', 'profile', 'width', 'height', 'frames', 'format'} <= v.keys() or not all(isinstance(v[k], str) and v[k] for k in ('name', 'path', 'sha256', 'profile', 'format')):
            raise ValueError('incomplete vector metadata')
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,95}', v['name']) or v['name'] in names:
            raise ValueError('invalid or duplicate vector name')
        names.add(v['name'])
        if not re.fullmatch('[0-9a-f]{64}', v['sha256']) or v['format'] not in ('nv12', 'p010le'):
            raise ValueError('vector requires SHA256 and an implemented raw format')
        if any(type(v[key]) is not int or not 0 < v[key] <= 65536 for key in ('width', 'height', 'frames')) or v['width'] % 2 or v['height'] % 2:
            raise ValueError('invalid vector geometry or frame count')
        path = (manifest.parent / v['path']).resolve()
        if digest(path) != v['sha256']: raise ValueError(f"{v['name']}: compressed source hash mismatch")
        v['source'] = str(path)
    return record


def lost_passes(previous, current):
    """Changing an input or deleting a vector cannot conceal an old pass."""
    before = {v['name']: v for v in previous['cases']}
    after = {v['name']: v for v in current['cases']}
    if len(before) != len(previous['cases']) or len(after) != len(current['cases']): raise ValueError('duplicate result name')
    identity = ('sha256', 'width', 'height', 'frames', 'format', 'profile')
    return sorted(name for name, old in before.items() if old['status'] == 'PASS' and
                  (name not in after or after[name]['status'] != 'PASS' or any(old[k] != after[name][k] for k in identity) or after[name].get('iterations', 1) < old.get('iterations', 1)))


def completions(trace):
    counts = {'stateful': 0, 'stateless': 0}
    with trace.open() as stream:
        for line in stream:
            event = dict(field.split('=', 1) for field in line.rstrip('\n').split('\t'))
            if event.get('STATUS') != '0': continue
            if event.get('STAGE') == 'stateful-capture-done': counts['stateful'] += 1
            if event.get('STAGE') == 'submit-done': counts['stateless'] += 1
    return counts


def run_vector(run, vector, output, env, render, ffmpeg, repeat=1):
    if type(repeat) is not int or not 1 <= repeat <= 100000: raise ValueError("repeat must be between 1 and 100000")
    case = {key: value for key, value in vector.items() if key != 'source'}
    case.update(status='FAIL', exit_code=1, duration_s=0, hardware_frames=0, software_frames=0, iterations=repeat)
    expected = vector['width'] * vector['height'] * 3 // 2 * vector['frames'] * repeat * (2 if vector['format'] == 'p010le' else 1)
    common = ['-nostdin', '-v', 'error', '-xerror']
    paths = []
    for hardware in (False, True):
        path = output / (vector['name'] + ('-va.yuv' if hardware else '-cpu.yuv'))
        paths.append(path)
        trace = output / (vector['name'] + '-decode.tsv')
        local = dict(env, VAV4L2_DECODE_TRACE=str(trace))
        command = [ffmpeg, *common]
        if hardware: command += ['-hwaccel', 'vaapi', '-hwaccel_device', render, '-hwaccel_output_format', 'vaapi']
        else: command += ['-hwaccel', 'none']
        if repeat > 1: command += ['-stream_loop', str(repeat - 1)]
        command += ['-i', vector['source']]
        if hardware: command += ['-vf', 'hwdownload,format=' + vector['format']]
        command += ['-pix_fmt', vector['format'], '-f', 'rawvideo', '-y', str(path)]
        result = run('conformance-' + vector['name'] + ('-hardware' if hardware else '-software'), command, local)
        case['duration_s'] += result['duration_s']
        if 'resources' in result: case['hardware_resources' if hardware else 'software_resources'] = result['resources']
        length = path.stat().st_size if path.exists() else 0
        case['hardware_frames' if hardware else 'software_frames'] = length // (expected // (vector['frames'] * repeat))
        if result['exit_code'] or length != expected:
            case['reason'] = f"{'hardware' if hardware else 'software'} exit={result['exit_code']}, bytes={length}/{expected}"
            return case
    case['completions'] = completions(trace) if trace.exists() else {'stateful': 0, 'stateless': 0}
    if sum(case['completions'].values()) < vector['frames'] * repeat:
        case['reason'] = 'missing driver completion evidence'
    elif digest(paths[0]) != digest(paths[1]): case['reason'] = 'hardware pixels differ from software reference'
    else: case.update(status='PASS', exit_code=0)
    return case


def run_conformance(run, output, build, render, manifest, previous=None, repeat=1):
    corpus = load_vectors(manifest.resolve())
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg: raise RuntimeError("conformance requires FFmpeg")
    env = dict(os.environ, LIBVA_DRIVER_NAME="v4l2", LIBVA_DRIVERS_PATH=str(build), VAV4L2_RENDER_NODE=render)
    probe = run("conformance-capabilities", [str(build / "va_capabilities"), render], env)
    if probe["exit_code"]: return probe
    capabilities = json.loads((output / "conformance-capabilities.log").read_text())
    supported = {p["profile"] for p in capabilities["profiles"] if p["entrypoint"] == "VAEntrypointVLD"}
    cases = []
    for vector in corpus["vectors"]:
        if vector["profile"] not in supported:
            case = {k: v for k, v in vector.items() if k != "source"}
            case.update(status="SKIP", exit_code=77, duration_s=0, reason="VA decode profile unavailable")
        else: case = run_vector(run, vector, output, env, render, ffmpeg, repeat)
        cases.append(case)
    lost = []
    if previous:
        before = json.loads(previous.read_text())
        if before.get("schema") != "libva-v4l2.test-report/v1" or before.get("profile") != "conformance":
            raise ValueError("previous must be a conformance report from tools/test.py")
        suites = [suite for suite in before["suites"] if suite["name"] == "conformance"]
        if len(suites) != 1: raise ValueError("previous report has no unique conformance suite")
        lost = lost_passes(suites[0], {"cases": cases})
    failed = bool(lost) or any(case["status"] == "FAIL" for case in cases)
    return dict(name="conformance", status="FAIL" if failed else "PASS", exit_code=int(failed),
                duration_s=probe["duration_s"] + sum(case["duration_s"] for case in cases), cases=cases,
                origin=corpus["origin"], manifest_sha256=digest(manifest), lost_passes=lost)
