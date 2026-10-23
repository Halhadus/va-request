"""Read-only, best-effort RGA/DMA kernel messages; stderr stays on the terminal.

No privilege changes, debug controls, ring clearing or unrelated messages.
Missing permission is recorded, never interpreted as a transport failure.
"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    prefix = Path(sys.argv[1])
    record = {"available": bool(shutil.which("dmesg")), "exit": None, "matched": 0}
    lines = []
    if record["available"]:
        try:
            result = subprocess.run(["dmesg", "--color=never"], stdout=subprocess.PIPE, text=True,
                                    errors="replace", timeout=10)
            record["exit"] = result.returncode
            lines = [line for line in result.stdout.splitlines()
                     if re.search(r"\b(rga[23]?|rockchip.rga|swiotlb|iommu|dma.buf)\b", line, re.I)]
            record["matched"] = len(lines)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr)
            record["error"] = str(error)
    prefix.with_suffix(".txt").write_text("\n".join(lines[-400:]) + ("\n" if lines else ""))
    prefix.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    print("DMA kernel messages:", prefix, record)
    return 0


if __name__ == "__main__":
    sys.exit(main())
