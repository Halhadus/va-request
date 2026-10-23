"""Require fresh successful direct-DMA jobs, not a passing staging substitute."""
import csv
from collections import Counter, defaultdict
from pathlib import Path
import sys

FORMATS = {"NV12": "NV12", "NV21": "NV21", "I420": "I420", "YUY2": "YUY2",
           "UYVY": "UYVY", "VYUY": "VYUY", "YVYU": "YVYU", "BGRX": "BGRX"}


def check(results, trace):
    results, trace = Path(results), Path(trace)
    if not results.is_file() or not trace.is_file():
        raise ValueError("current results or VPP trace missing")
    with results.open() as file:
        rows = list(csv.reader(file, delimiter="\t"))
    if sum(row[:2] == ["COMPLETE", "1"] for row in rows) != 1:
        raise ValueError("public client did not complete exactly once")
    for name in FORMATS:
        for prefix in ("dmabuf-direct-stress-", "dmabuf-cold-stress-"):
            cases = [row for row in rows if len(row) > 1 and row[1] == prefix + name]
            if len(cases) != 1 or cases[0][0] != "PASS":
                raise ValueError("missing or failed stress case: " + prefix + name)
    for name in ("NV12", "NV21"):
        cases = [row for row in rows if len(row) > 1 and row[1] == "dmabuf-multiplanar-stress-" + name]
        if len(cases) != 1 or cases[0][0] != "PASS": raise ValueError("missing or failed multi-object stress case: " + name)
    counts = Counter(); contexts = defaultdict(set)
    with trace.open() as file:
        reader = csv.DictReader(file, delimiter="\t")
        required = {"STATUS", "SRC_FOURCC", "DST_FOURCC", "QUEUE_VALID", "OUTPUT_MEMORY", "CAPTURE_MEMORY", "OUTPUT_PLANES", "CAPTURE_PLANES", "CONTEXT"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError("VPP trace lacks negotiated DMA queue evidence")
        for row in reader:
            if row["STATUS"] == "0" and row["QUEUE_VALID"] == "1" and row["OUTPUT_MEMORY"] == row["CAPTURE_MEMORY"] == "4" and row["SRC_FOURCC"] == row["DST_FOURCC"]:
                key = (row["SRC_FOURCC"], row["OUTPUT_PLANES"], row["CAPTURE_PLANES"])
                counts[key] += 1
                if not row["CONTEXT"].isdigit() or not int(row["CONTEXT"]): raise ValueError("invalid context identity")
                contexts[key].add(int(row["CONTEXT"]))
    for name, fourcc in FORMATS.items():
        value = f"{int.from_bytes(fourcc.encode(), 'little'):08x}"
        count = counts[(value, "1", "1")]
        if count < 42:
            raise ValueError(f"{name}: {count}/42 successful direct jobs; staging is not proof")
        if len(contexts[(value, "1", "1")]) < 9:
            raise ValueError(name + ": missing eight fresh contexts alongside warm stress")
    for fourcc in ("NV12", "NV21"):
        value = f"{int.from_bytes(fourcc.encode(), 'little'):08x}"
        if counts[(value, "2", "2")] < 34: raise ValueError(fourcc + ": fewer than34 completed two-plane DMA jobs")
    return counts


def main():
    try:
        check(*sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        print("DMA-BUF evidence:", error, file=sys.stderr)
        return 1
    print("DMA-BUF evidence: eight formats, at least404 completed direct jobs and eight fresh contexts per format including two-object NV12/NV21")
    return 0


if __name__ == "__main__":
    sys.exit(main())
