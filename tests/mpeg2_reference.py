#!/usr/bin/env python3
"""Align progressive MPEG-2 reference height within the same macroblock grid."""
import argparse
from pathlib import Path


def align_sequence_height(data, width, height):
    aligned = (height + 15) & ~15
    if not 0 < width < 4096 or not 0 < height <= aligned < 4096:
        raise ValueError('reference dimensions exceed the 12-bit sequence header')
    result = bytearray(data)
    offset = count = 0
    while True:
        offset = result.find(b'\x00\x00\x01\xb3', offset)
        if offset < 0:
            break
        if offset + 12 > len(result):
            raise ValueError('truncated MPEG-2 sequence header')
        actual_width = result[offset + 4] * 16 + (result[offset + 5] >> 4)
        actual_height = (result[offset + 5] & 15) * 256 + result[offset + 6]
        if (actual_width, actual_height) != (width, height):
            raise ValueError(f'unexpected sequence dimensions {actual_width}x{actual_height}')
        result[offset + 5] = (result[offset + 5] & 0xf0) | (aligned >> 8)
        result[offset + 6] = aligned & 255
        offset += 12
        count += 1
    if not count:
        raise ValueError('no MPEG-2 sequence header')
    offset = 0
    while True:
        offset = result.find(b'\x00\x00\x01\xb5', offset)
        if offset < 0:
            break
        if offset + 7 > len(result):
            raise ValueError('truncated MPEG-2 extension')
        bits = int.from_bytes(result[offset + 4:offset + 7], 'big')
        if bits >> 20 == 1:
            if not bits & (1 << 11) or bits & (15 << 5):
                raise ValueError('reference normalization requires progressive 12-bit dimensions')
        offset += 7
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('width', type=int)
    parser.add_argument('height', type=int)
    args = parser.parse_args()
    try:
        data = align_sequence_height(args.source.read_bytes(), args.width, args.height)
        args.destination.write_bytes(data)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(f'MPEG2_REFERENCE {args.width}x{args.height} -> {args.width}x{(args.height + 15) & ~15}; crop after decode')


if __name__ == '__main__':
    main()
