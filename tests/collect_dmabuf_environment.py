"""Read-only host inventory for allocator, queue and renderer follow-up."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def main():
    output = Path(sys.argv[1]); output.mkdir(parents=True, exist_ok=True)
    record = {"kernel": platform.release(), "machine": platform.machine(), "page_size": os.sysconf("SC_PAGE_SIZE"),
              "render_node": os.environ.get("VAREQ_RENDER_NODE", "/dev/dri/renderD128"), "dma_heaps": [], "commands": []}
    build = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("builddir")
    record["binary_sha256"] = {name: hashlib.sha256((build / name).read_bytes()).hexdigest() if (build / name).is_file() else None
        for name in ("v4l2request_drv_video.so", "va_feature_client", "prime_surface_interpose.so", "va_dmabuf_renderer", "vpp_dmabuf_probe")}
    if Path(".git").exists():
        record["tree"] = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], text=True).strip()
        record["dirty"] = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], text=True).strip())
    heaps = Path("/dev/dma_heap")
    if heaps.is_dir(): record["dma_heaps"] = [str(p) for p in sorted(heaps.iterdir())]
    for path in sorted(Path("/sys/class/video4linux").glob("video*/name")):
        try: record.setdefault("video_nodes", {})[path.parent.name] = path.read_text().strip()
        except OSError as error: print(error, file=sys.stderr)
    def command(name, args):
        if not shutil.which(args[0]): record["commands"].append({"name": name, "available": False}); return
        try:
            with (output / (name + ".txt")).open("w") as file: code = subprocess.run(args, stdout=file, timeout=30).returncode
            record["commands"].append({"name": name, "available": True, "exit": code})
        except (OSError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr); record["commands"].append({"name": name, "error": str(error)})
    for path in sorted(Path("/sys/class/video4linux").glob("video*")):
        command(path.name + "-formats", ["v4l2-ctl", "-d", "/dev/" + path.name, "--all", "--list-formats-out", "--list-formats"])
    command("egl-info", ["eglinfo", "-B"])
    (output / "environment.json").write_text(json.dumps(record, indent=2) + "\n")
    print("DMA-BUF environment:", output / "environment.json")
    return 0


if __name__ == "__main__": sys.exit(main())
