"""NUMA placement for software x265 test fixtures."""
import argparse
import os
from pathlib import Path


def x265_pool(max_workers=1, node_root=Path('/sys/devices/system/node')):
    fallback = '*' if max_workers is None else str(max_workers)
    try:
        allowed = os.sched_getaffinity(0)
        paths = list(node_root.glob('node[0-9]*/cpulist'))
    except (AttributeError, OSError):
        return fallback
    nodes = []
    for path in paths:
        try:
            node = int(path.parent.name[4:])
            cpus = set()
            for span in path.read_text().strip().split(','):
                first, separator, last = span.partition('-')
                cpus.update(range(int(first), int(last) + 1) if separator else [int(first)])
            available = cpus & allowed
            if available: nodes.append((node, len(available)))
        except (OSError, ValueError):
            continue
    if not nodes:
        return fallback
    node, count = min(nodes)
    workers = count if max_workers is None else min(count, max_workers)
    return ','.join(['-'] * node + [str(workers), '-'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--auto', action='store_true', help='use the available CPUs on the selected node')
    args = parser.parse_args()
    print(x265_pool(None if args.auto else 1))
