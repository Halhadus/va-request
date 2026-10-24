#!/usr/bin/env python3
"""Build and test libva-v4l2, preserving a machine-readable report on failure."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import xml.etree.ElementTree as ET
import importlib.util

SCHEMA = "libva-v4l2.test-report/v1"
ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def source_identity(root, build):
    result = {"commit": None, "tree": None, "dirty": None, "driver_sha256": sha256(build / "v4l2_drv_video.so")}
    if (root / ".git").exists():
        for field, args in (("commit", ["rev-parse", "HEAD"]), ("tree", ["rev-parse", "HEAD^{tree}"]), ("dirty", ["status", "--porcelain", "--untracked-files=no"])):
            value = subprocess.run(["git", *args], cwd=root, text=True, stdout=subprocess.PIPE, check=False)
            if value.returncode == 0: result[field] = bool(value.stdout.strip()) if field == "dirty" else value.stdout.strip()
    return result


def execute(name, command, output, cwd=ROOT, env=None):
    started = time.monotonic()
    print(f"[RUN] {name}: {' '.join(map(str, command))}", flush=True)
    with (output / f"{name}.log").open("w") as log:
        # stderr stays attached to the terminal. Meson also retains its own per-test diagnostics.
        with subprocess.Popen(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, bufsize=1, start_new_session=True) as child:
            try:
                for line in child.stdout:
                    print(line, end="", flush=True); log.write(line)
                code = child.wait()
            except BaseException:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try: child.wait(timeout=5)
                    except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.wait()
                raise
    return {"name": name, "status": "SKIP" if code == 77 else "PASS" if code == 0 else "FAIL", "exit_code": code, "duration_s": round(time.monotonic() - started, 6)}


def meson_cases(path):
    cases = []
    for line in path.read_text().splitlines():
        if not line.strip(): continue
        item = json.loads(line)
        result = item.get("result")
        status = "PASS" if result in ("OK", "EXPECTEDFAIL") else "SKIP" if result == "SKIP" else "FAIL"
        cases.append({"name": item["name"], "status": status, "exit_code": item.get("returncode"), "duration_s": item.get("duration", 0)})
    return cases


def sanitize_meson_log(name, data):
    if not name.endswith(("testlog.json", "testlog.txt")): return data
    if name.endswith(".txt"):
        return "".join(line for line in data.decode().splitlines(keepends=True) if not line.startswith("Inherited environment:")).encode()
    rows = []
    for line in data.decode().splitlines():
        if not line.strip(): continue
        row = json.loads(line)
        if "env" in row:
            row["env"] = {key: value for key, value in row["env"].items() if key.startswith(("VAV4L2_", "LIBVA_")) or key in
                ("ASAN_OPTIONS", "UBSAN_OPTIONS", "MSAN_OPTIONS", "TSAN_OPTIONS", "MALLOC_PERTURB_", "MESON_TEST_ITERATION")}
        rows.append(json.dumps(row) + "\n")
    return "".join(rows).encode()


def sanitize_report_logs(output):
    for path in output.iterdir():
        if path.is_file() and path.name.endswith(("testlog.json", "testlog.txt")):
            try: path.write_bytes(sanitize_meson_log(path.name, path.read_bytes()))
            except (OSError, ValueError):
                path.unlink(missing_ok=True)
                raise
    nested = output / "hardware.tar.gz"
    if not nested.is_file(): return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output, suffix=".tar.gz", delete=False) as file: temporary = Path(file.name)
        with tarfile.open(nested) as source, tarfile.open(temporary, "w:gz") as target:
            for member in source:
                if not member.isfile(): target.addfile(member); continue
                data = sanitize_meson_log(member.name, source.extractfile(member).read())
                member.size = len(data)
                target.addfile(member, io.BytesIO(data))
        os.replace(temporary, nested)
    except (OSError, ValueError, tarfile.TarError):
        nested.unlink(missing_ok=True)
        raise
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)


def write_report(record, output):
    cases = []
    for suite in record["suites"]:
        children = suite.get("cases") or []
        cases.extend(children or [suite])
        if children and suite["status"] == "FAIL" and not any(case["status"] == "FAIL" for case in children): cases.append(suite)
    record["summary"] = {key: sum(case["status"] == value for case in cases) for key, value in (("pass", "PASS"), ("fail", "FAIL"), ("skip", "SKIP"))}
    record["summary"].update(complete=record["complete"], exit_code=record["exit_code"])
    (output / "report.json").write_text(json.dumps(record, indent=2) + "\n")
    with (output / "results.tsv").open("w") as file:
        file.write("NAME\tSTATUS\tEXIT_CODE\tDURATION_S\n")
        for case in cases: file.write(f"{case['name']}\t{case['status']}\t{case.get('exit_code')}\t{case.get('duration_s', 0)}\n")
    suite = ET.Element("testsuite", name="libva-v4l2", tests=str(len(cases)), failures=str(record["summary"]["fail"]), skipped=str(record["summary"]["skip"]))
    for case in cases:
        node = ET.SubElement(suite, "testcase", name=case["name"], time=str(case.get("duration_s", 0)))
        if case["status"] == "FAIL": ET.SubElement(node, "failure", message=f"exit {case.get('exit_code')}; see accompanying logs")
        elif case["status"] == "SKIP": ET.SubElement(node, "skipped", message="Not available on this host")
    ET.ElementTree(suite).write(output / "junit.xml", encoding="utf-8", xml_declaration=True)
    summary = record["summary"]
    (output / "summary.txt").write_text(f"PASS={summary['pass']} FAIL={summary['fail']} SKIP={summary['skip']} COMPLETE={int(summary['complete'])} EXIT={summary['exit_code']}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("unit", "hardware", "smoke", "acceptance", "rk3588"), default="unit")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "verify")
    parser.add_argument("--render-node", default="/dev/dri/renderD128")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args(argv)
    build = args.build_dir.resolve(); parent = args.output_dir.resolve(); parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    record = {"schema": SCHEMA, "run_id": output.name, "started_at": datetime.now(timezone.utc).isoformat(), "profile": args.suite,
              "platform": {"machine": platform.machine(), "kernel": platform.release()}, "source": source_identity(ROOT, build), "suites": [], "complete": False, "exit_code": 1}
    def run(name, command, env=None):
        result = execute(name, command, output, env=env); record["suites"].append(result); return result
    code = 1; interrupted_code = 130
    def terminated(signum, frame):
        nonlocal interrupted_code
        interrupted_code = 128 + signum
        raise KeyboardInterrupt
    previous_term = signal.signal(signal.SIGTERM, terminated)
    try:
        if not shutil.which("meson"): raise RuntimeError("meson is required")
        options = ["-Dtests=true", "-Dhardware_tests=" + ("enabled" if args.suite in ("acceptance", "rk3588") else "auto"), "-Db_ndebug=false", "-Db_sanitize=" + ("address,undefined" if args.sanitize else "none")]
        command = ["meson", "setup", "--reconfigure", str(build), *options] if (build / "meson-private/coredata.dat").is_file() else ["meson", "setup", str(build), str(ROOT), *options]
        if run("configure", command)["exit_code"]: return 1
        if run("build", ["meson", "compile", "-C", str(build)])["exit_code"]: return 1
        record["source"] = source_identity(ROOT, build)
        unit = run("unit", ["meson", "test", "-C", str(build), "--suite", "unit", "--print-errorlogs"])
        for name in ("testlog.json", "testlog.txt", "testlog.junit.xml"):
            src = build / "meson-logs" / name
            if src.is_file(): shutil.copy2(src, output / name)
        if (output / "testlog.json").is_file(): unit["cases"] = meson_cases(output / "testlog.json")
        if unit["exit_code"]: record["complete"] = True; return 1
        if args.suite in ("hardware", "smoke"):
            live = run("discovery", [str(build / "test_video_probe"), "--live"])
            if live["status"] == "FAIL": record["complete"] = True; return 1
            if shutil.which("v4l2-ctl") and any(Path("/dev").glob("video[0-9]*")):
                inventory = run("capabilities", [sys.executable, str(ROOT / "tools/collect_capabilities.py"), "--output", str(output / "capabilities.json")])
                if inventory["exit_code"]: record["complete"] = True; return 1
            spec = importlib.util.spec_from_file_location("hardware", ROOT / "tools/hardware.py")
            hardware = importlib.util.module_from_spec(spec); spec.loader.exec_module(hardware)
            # Individual phases are already recorded by run(); attach final pixel
            # results to those same objects and add explicit unsupported cases.
            for result in hardware.run_hardware(run, output, build, args.render_node, extended=args.suite == "hardware"):
                if not any(result is old for old in record["suites"]): record["suites"].append(result)
            if any(item["status"] == "FAIL" for item in record["suites"]): record["complete"] = True; return 1
        elif args.suite in ("acceptance", "rk3588"):
            env = dict(os.environ, VAV4L2_BUILD_DIR=str(build), VAV4L2_REPORT_FILE=str(output / "hardware.tar.gz"), VAV4L2_RENDER_NODE=args.render_node)
            result = run("acceptance", ["bash", "tests/run_dmabuf_completion.sh"], env)
            if (output / "hardware.tar.gz").is_file():
                spec = importlib.util.spec_from_file_location("hardware", ROOT / "tools/hardware.py")
                hardware = importlib.util.module_from_spec(spec); spec.loader.exec_module(hardware)
                result["cases"] = hardware.acceptance_cases(output / "hardware.tar.gz")
                if any(case["status"] == "FAIL" for case in result["cases"]): result.update(status="FAIL", exit_code=1)
            if result["exit_code"]: record["complete"] = True; return 1
        record["complete"] = True; code = 0; return code
    except KeyboardInterrupt:
        code = interrupted_code; print("Interrupted; preserving partial report", file=sys.stderr); return code
    except (OSError, RuntimeError, ValueError, ET.ParseError, tarfile.TarError) as error:
        record["error"] = str(error); record["suites"].append({"name": "runner-error", "status": "FAIL", "exit_code": 1, "duration_s": 0}); print(error, file=sys.stderr); return 1
    finally:
        signal.signal(signal.SIGTERM, previous_term)
        record["exit_code"] = code
        sanitation_failed = False
        try: sanitize_report_logs(output)
        except (OSError, ValueError, tarfile.TarError) as error:
            sanitation_failed = True
            record["exit_code"] = 1
            record["error"] = str(error)
            record["suites"].append({"name": "report-logs", "status": "FAIL", "exit_code": 1, "duration_s": 0})
            # A partially filtered directory must not enter the shared archive.
            for path in output.iterdir():
                if path.is_file() and (path.name.endswith(("testlog.json", "testlog.txt")) or path.name == "hardware.tar.gz"): path.unlink()
            print(f"Cannot prepare report logs: {error}", file=sys.stderr)
        write_report(record, output)
        archive = parent / f"libva-v4l2-{output.name}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar: tar.add(output, arcname=output.name)
        print((output / "summary.txt").read_text().strip()); print(f"REPORT={archive}")
        if sanitation_failed: return 1


if __name__ == "__main__": sys.exit(main())
