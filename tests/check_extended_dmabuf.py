"""Require completed public pixel cases and real DMA/context evidence."""
import csv
from collections import Counter, defaultdict
from pathlib import Path
import sys

FORMATS = ("YV12", "422H", "YV16", "ARGB", "BGRA")
PAIRS = (("YV12", "I420"), ("I420", "YV12"), ("422H", "YV16"), ("YV16", "422H"), ("ARGB", "BGRA"), ("BGRA", "ARGB"))
CASES = {"dmabuf-extended-" + kind + "-" + name for name in FORMATS for kind in ("layout", "warm", "cold", "compose")}
CASES |= {"dmabuf-extended-alpha-" + name for name in ("ARGB", "BGRA")}
CASES |= {"dmabuf-extended-cross-" + source + "-to-" + target for source, target in PAIRS}


def fourcc(name):
    return f"{int.from_bytes(name.encode(), 'little'):08x}"


def check(results, trace):
    with Path(results).open() as file:
        rows = list(csv.reader(file, delimiter="\t"))
    if sum(row[:2] == ["COMPLETE", "1"] for row in rows) != 1:
        raise ValueError("extended client did not complete exactly once")
    cases = [row for row in rows if row and row[0] in ("PASS", "FAIL", "SKIP")]
    if len(cases) != len(CASES) or {row[1] for row in cases if len(row) > 1} != CASES or any(row[0] != "PASS" for row in cases):
        raise ValueError("missing, duplicate, skipped or failed extended pixel case")
    counts, contexts = Counter(), defaultdict(set)
    with Path(trace).open() as file:
        reader = csv.DictReader(file, delimiter="\t")
        required = {"STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY", "OUTPUT_PLANES", "CAPTURE_PLANES", "CONTEXT"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError("extended trace lacks DMA/context fields")
        for row in reader:
            if row["STATUS"] != "0":
                continue  # Expected nonopaque rejection is a public pixel case.
            if row["QUEUE_VALID"] != "1" or row["OUTPUT_MEMORY"] != "4" or row["CAPTURE_MEMORY"] != "4" or row["OUTPUT_PLANES"] != "1" or row["CAPTURE_PLANES"] != "1":
                raise ValueError("extended success used staging or incorrect plane count")
            context = int(row["CONTEXT"])
            if context <= 0:
                raise ValueError("missing real context identity")
            key = (row["SRC_FOURCC"], row["DST_FOURCC"])
            counts[key] += 1; contexts[key].add(context)
    for name in FORMATS:
        key = (fourcc(name), fourcc(name))
        minimum = 44 if name in ("ARGB", "BGRA") else 43
        if counts[key] < minimum or len(contexts[key]) < 9:
            raise ValueError(f"{name}: insufficient direct jobs or distinct cold contexts")
    for source, target in PAIRS:
        if counts[(fourcc(source), fourcc(target))] < 8:
            raise ValueError(source + " to " + target + ": fewer than8 direct jobs")
    return counts


if __name__ == "__main__":
    try:
        check(*sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        print("Extended DMA evidence:", error, file=sys.stderr)
        sys.exit(1)
    print("Extended DMA evidence:28 pixel cases,265+ direct jobs, five formats with cold contexts")
