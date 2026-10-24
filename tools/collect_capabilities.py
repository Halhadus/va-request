#!/usr/bin/env python3
"""Read-only V4L2 inventory for the bridge's existing codec and VPP nodes."""
import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DECODERS = {"H264": "H264", "HEVC": "HEVC", "VP80": "VP8", "MPG2": "MPEG2", "S264": "H264", "S265": "HEVC", "VP8F": "VP8", "MG2S": "MPEG2", "AV1F": "AV1", "AV01": "AV1", "VP9F": "VP9", "VP90": "VP9"}
EXCLUDED = {"AV1F", "AV01", "VP9F", "VP90"}
RAW_FORMATS = {"NV12", "NV21", "YU12", "YV12", "NV16", "NV61", "422P", "YUYV", "UYVY", "VYUY", "YVYU", "RGBP", "AB24", "XB24", "BA24", "AR24", "XR24", "NM12", "NM21", "YM12"}


def query(executable, node, option):
    # stdout is report data; child stderr stays attached to the terminal.
    try:
        p = subprocess.run([executable, "--device", node, option], stdout=subprocess.PIPE,
                           text=True, timeout=30, check=False)
        return {"option": option, "exit": p.returncode, "stdout": p.stdout}
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"{node}: {option}: {exc}", file=sys.stderr)
        return {"option": option, "exit": None, "error": str(exc), "stdout": ""}


def fourccs(result):
    return set(re.findall(r"'([^'\n]{4})'", result["stdout"]))


def collect(executable, nodes):
    report = {"schema": 1, "complete": True, "read_only": True,
              "kernel": {"release": platform.release(), "version": platform.version(), "machine": platform.machine()},
              "scope": "All video nodes; implementation and validation are separate from kernel capability",
              "limits": ["No S_FMT, controls writes, REQBUFS, STREAMON or decode operations.",
                         "CAPTURE enumeration reflects the default format/control state on a new fd; it is not proof of all post-SPS formats.",
                         "Control/format presence is not proof of every combination or of pixel correctness."],
              "nodes": [], "skipped": []}
    for node in nodes:
        output = query(executable, node, "--list-formats-out")
        codes = fourccs(output)
        roles = sorted({DECODERS[c] for c in codes if c in DECODERS})
        capture = query(executable, node, "--list-formats") if not roles else None
        if capture and "JPEG" in fourccs(capture) and "JPEG" not in codes:
            roles.append("JPEG encode")
        if capture and (codes & RAW_FORMATS) and (fourccs(capture) & RAW_FORMATS) and not (codes & (set(DECODERS) | EXCLUDED | {"JPEG", "MJPG"})):
            roles.append("raw M2M candidate")
        if not roles: roles.append("unclassified interface")
        sections = [output]
        if capture: sections.append(capture)
        sections.extend(query(executable, node, option) for option in
                        ["--all", "--list-ctrls-menus", "--list-formats-out-ext", "--list-formats-ext"])
        entry = {"node": node, "roles": roles, "sections": sections}
        entry["decoder_interfaces"] = [{"fourcc": code, "codec": DECODERS[code],
            "api": "stateless" if code in {"S264", "S265", "VP8F", "MG2S", "AV1F", "VP9F"} else "stateful"}
            for code in sorted(codes & set(DECODERS))]
        # Optional selection queries failing can mean unsupported targets.
        if "raw M2M candidate" in roles:
            entry["selection_queries"] = [query(executable, node, option) for option in
                ["--get-selection-output=target=crop_bounds", "--get-selection=target=compose_bounds"]]
        if any(section["exit"] != 0 for section in sections): report["complete"] = False
        report["nodes"].append(entry)
    if not report["nodes"]: report["complete"] = False
    return report


def write_report(report, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=path.name + ".", delete=False) as f:
        temporary = Path(f.name)
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nodes", nargs="*", help="optional /dev/videoN subset")
    parser.add_argument("--output", type=Path, default=Path("verify/capabilities/report.json"))
    parser.add_argument("--v4l2-ctl", default="v4l2-ctl")
    args = parser.parse_args()
    executable = shutil.which(args.v4l2_ctl)
    if not executable:
        print("v4l2-ctl is required (v4l-utils); no device settings were changed.", file=sys.stderr)
        write_report({"schema": 1, "complete": False, "error": "v4l2-ctl missing"}, args.output)
        return 2
    nodes = args.nodes or sorted(glob.glob("/dev/video[0-9]*"))
    report = collect(executable, nodes)
    write_report(report, args.output)
    for entry in report["nodes"]: print(f"{entry['node']}: {', '.join(entry['roles'])}")
    print(f"COMPLETE={int(report['complete'])} REPORT={args.output}")
    return 0 if report["complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
