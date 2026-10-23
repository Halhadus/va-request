"""Record the source and exact binaries before starting hardware feature checks."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    if not path.is_file():
        return None
    value = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    root = Path(__file__).resolve().parent.parent
    build, output = map(Path, sys.argv[1:])
    record = {"schema": 1, "tree": None, "dirty": None,
              "driver_sha256": digest(build / "v4l2request_drv_video.so"),
              "client_sha256": digest(build / "va_feature_client"),
              "vpp_dmabuf": os.environ.get("VAREQ_VPP_DMABUF", "auto"),
              "export": os.environ.get("VAREQ_EXPBUF_EXPORT", "default")}
    if (root / ".git").exists():
        record["tree"] = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
        status = subprocess.run(["git", "-C", str(root), "diff", "--quiet", "HEAD", "--"]).returncode
        if status not in (0, 1):
            return 1
        record["dirty"] = status == 1
    output.write_text(json.dumps(record, indent=2) + "\n")
    if not record["driver_sha256"] or not record["client_sha256"]:
        print("Feature source: required build driver/client file is missing", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
