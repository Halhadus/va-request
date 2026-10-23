"""Require every public row-layout case and the expected staging transport."""
import csv
from collections import Counter
from pathlib import Path
import sys

FORMATS = ("NV12", "NV21", "I420", "YV12", "P010", "422H", "YV16")
CASES = {f"dmabuf-rows-{name}-{layer}-{order}" for name in FORMATS for layer in ("composed", "separate") for order in ("forward", "reverse")}


def check(results, trace):
    with Path(results).open() as file: rows = list(csv.reader(file, delimiter="\t"))
    cases = [row for row in rows if row and row[0] in ("PASS", "FAIL", "SKIP")]
    if sum(row[:2] == ["COMPLETE", "1"] for row in rows) != 1 or len(cases) != 28 or {row[1] for row in cases if len(row) > 1} != CASES or any(row[0] != "PASS" for row in cases):
        raise ValueError("missing, duplicate, failed, skipped or incomplete row-layout case")
    counts = Counter()
    with Path(trace).open() as file:
        reader = csv.DictReader(file, delimiter="\t")
        required = {"STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY"}
        if not required.issubset(reader.fieldnames or []): raise ValueError("row trace lacks queue evidence")
        for row in reader:
            if any(row[key] != value for key, value in (("STATUS", "0"), ("QUEUE_VALID", "1"), ("OUTPUT_MEMORY", "1"), ("CAPTURE_MEMORY", "1"))):
                raise ValueError("row-layout processing failed or bypassed staging")
            if row["SRC_FOURCC"] != row["DST_FOURCC"]: raise ValueError("unexpected row conversion")
            counts[row["SRC_FOURCC"]] += 1
    expected = {f"{int.from_bytes(name.encode(), 'little'):08x}": 4 for name in FORMATS if name != "P010"}
    if counts != expected: raise ValueError("missing or unexpected row-layout VPP jobs")
    return sum(counts.values())


if __name__ == "__main__":
    try: jobs = check(*sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        print("DMA row evidence:", error, file=sys.stderr); sys.exit(1)
    print(f"DMA row evidence:28 pixel/lifetime/layout cases,{jobs} staged VPP jobs; P010 image transfers only")
