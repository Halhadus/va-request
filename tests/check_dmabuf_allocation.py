"""Verify that every planar422 allocation retained the measured RGA2 provider."""
import csv
import os
from pathlib import Path
import sys


def check(path):
    with Path(path).open() as file:
        reader = csv.DictReader(file, delimiter="\t")
        fields = {"NODE", "STAGE", "WIDTH_BYTES", "ROWS", "SIZE", "ALLOCATED", "ERRNO", "FORMAT", "ALLOC_WIDTH", "ALLOC_HEIGHT", "STRIDE", "IMAGE"}
        if not fields.issubset(reader.fieldnames or []):
            raise ValueError("allocation trace lacks negotiated storage geometry")
        rows = [row for row in reader if row["WIDTH_BYTES"] == "128" and row["ROWS"] == "256" and row["STAGE"].startswith("exported")]
    if len(rows) != 8:
        raise ValueError(f"expected8 planar422 native allocations, found{len(rows)}")
    page = os.sysconf("SC_PAGESIZE")
    for row in rows:
        need, actual, image = (int(row[key]) for key in ("SIZE", "ALLOCATED", "IMAGE"))
        width, height, stride = (int(row[key]) for key in ("ALLOC_WIDTH", "ALLOC_HEIGHT", "STRIDE"))
        if row["STAGE"] != "exported-rga2-capture" or row["ERRNO"] != "0" or need != 32768 or not need <= image <= actual <= (need + page - 1) // page * page:
            raise ValueError("planar422 selected a different provider or exceeded its page span")
        bpp = {"34325258": 4, "56595559": 2, "3231564e": 1}.get(row["FORMAT"])
        capacity = stride * height * 3 // 2 if bpp == 1 else stride * height
        if not bpp or not 34 <= width <= 8192 or not 34 <= height <= 8192 or stride < width * bpp or image != capacity:
            raise ValueError("planar422 allocation lacks valid negotiated storage geometry")
    return len(rows)


if __name__ == "__main__":
    try:
        count = check(*sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        print("DMA allocation evidence:", error, file=sys.stderr)
        sys.exit(1)
    print(f"DMA allocation evidence:{count} planar422 RGA2 CAPTURE objects within consumer page bounds")
