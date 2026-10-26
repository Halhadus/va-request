"""Turn an all-key IVF fixture into hidden keys followed by show-existing."""
from pathlib import Path
import struct


def hide_key_frames(path):
    path = Path(path)
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != b'DKIF' or data[8:12] != b'VP90': raise ValueError('VP9 IVF fixture required')
    pos = 32
    out = bytearray(data[:32])
    frames = 0
    while pos < len(data):
        if pos + 12 > len(data): raise ValueError('truncated IVF frame header')
        length, timestamp = struct.unpack_from('<IQ', data, pos)
        pos += 12
        frame = bytearray(data[pos:pos + length])
        if len(frame) != length or not frame or frame[0] & 0xce != 0x82: raise ValueError('shown VP9 keyframe required')
        pos += length
        profile = (frame[0] >> 5 & 1) | (frame[0] >> 4 & 1) << 1
        if profile not in (0, 2): raise ValueError('4:2:0 profile required')
        frame[0] &= ~2  # show_frame=0; entropy-coded pixels are unchanged.
        # The input container groups two frames; V4L2 OUTPUT submissions must
        # keep the hidden key and show-existing frames separate.
        frame += bytes([0x88 | profile << 3, 0xd9]) + struct.pack('<II', length, 1) + b'\xd9'
        out += struct.pack('<IQ', len(frame), timestamp) + frame
        frames += 1
    if frames != 8: raise ValueError('fixture requires eight keys')
    path.write_bytes(out)
