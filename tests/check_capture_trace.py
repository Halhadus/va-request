"""A decoder PRIME import is not proof of direct CAPTURE DMA-BUF use."""
import csv
from collections import Counter
from pathlib import Path
import sys


def check(path):
    with Path(path).open() as file:
        reader = csv.DictReader(file, delimiter="\t")
        required = {"STAGE", "CODEC", "FORMAT", "MEMORY", "ERRNO", "SURFACE", "SIZEIMAGE", "STRIDE"}
        if not required.issubset(reader.fieldnames or []): raise ValueError("capture trace lacks negotiated memory/format/completion")
        counts = Counter()
        for row in reader:
            if row["STAGE"] != "complete": continue
            if row["MEMORY"] != "4" or row["FORMAT"] != "3231564e" or row["ERRNO"] != "0" or int(row["SIZEIMAGE"]) <= 0 or int(row["STRIDE"]) <= 0:
                raise ValueError("invalid direct CAPTURE completion")
            counts[int(row["CODEC"])] += 1
    # H264=0, HEVC=1, VP8=3, VP9=4, MPEG2=5; AV1 remains deferred.
    for codec in (0, 1, 3, 5):
        if counts[codec] < 8: raise ValueError(f"codec{codec}: {counts[codec]}/8 direct frames; staged imports cannot substitute")
    return counts


def main():
    try: counts = check(sys.argv[1])
    except (OSError, ValueError, KeyError, IndexError, csv.Error) as error:
        print("Direct CAPTURE evidence:", error, file=sys.stderr); return 1
    print("Direct CAPTURE evidence:", dict(counts)); return 0


if __name__ == "__main__": sys.exit(main())
