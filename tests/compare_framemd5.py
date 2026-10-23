#!/usr/bin/env python3
"""Reject empty/truncated output before comparing ordered frame sizes/hashes."""
import argparse
from pathlib import Path


def read_frames(path, expected):
    frames = []
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split(',')]
        if len(parts) != 6 or int(parts[0]) != 0 or int(parts[4]) <= 0:
            raise ValueError(f'{path}: invalid frame record')
        if len(parts[5]) != 32 or any(c not in '0123456789abcdef' for c in parts[5].lower()):
            raise ValueError(f'{path}: invalid MD5')
        frames.append((int(parts[4]), parts[5].lower()))
    if not frames or len(frames) != expected:
        raise ValueError(f'{path}: expected {expected} frames, found {len(frames)}')
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('expected', type=int)
    parser.add_argument('actual')
    parser.add_argument('reference', nargs='?')
    parser.add_argument('--min-unique', type=int, default=1)
    args = parser.parse_args()
    try:
        actual = read_frames(args.actual, args.expected)
        if len(set(actual)) < args.min_unique:
            raise ValueError(f'{args.actual}: output is unexpectedly static')
        if args.reference and actual != read_frames(args.reference, args.expected):
            raise ValueError('frame sizes/hashes differ')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
