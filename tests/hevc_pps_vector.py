"""Make an exact decode oracle with a nonzero PPS ID; no codec data is replaced."""
from pathlib import Path
import re


def rbsp(data):
    out = bytearray(); zeros = 0; index = 0
    while index < len(data):
        value = data[index]; index += 1
        if zeros == 2 and value == 3:
            if index == len(data) or data[index] > 3: raise ValueError('invalid emulation prevention')
            zeros = 0; continue
        out.append(value); zeros = min(zeros + 1, 2) if value == 0 else 0
    return bytes(out)


def ebsp(data):
    out = bytearray(); zeros = 0
    for value in data:
        if zeros == 2 and value <= 3: out.append(3); zeros = 0
        out.append(value); zeros = min(zeros + 1, 2) if value == 0 else 0
    return bytes(out)


def ue(bits, start):
    end = bits.find('1', start)
    if end < 0 or end - start > 6 or 2 * end - start + 1 > len(bits): raise ValueError('invalid PPS ue(v)')
    stop = 2 * end - start + 1
    return int(bits[end:stop], 2) - 1, stop


def rewrite_pps_id(path, new_id=15):
    """Replace PPS0 by PPS15,adding exactly one byte to each affected RBSP.

    Whole-byte growth preserves slice CABAC alignment and entry-point offsets.
    This is a bounded test fixture,not a general parameter-set transcoder.
    """
    if not 0 <= new_id <= 63: raise ValueError('PPS ID range')
    code = f'{new_id + 1:b}'; code = '0' * (len(code) - 1) + code
    if (len(code) - 1) % 8: raise ValueError('PPS replacement must preserve byte alignment')
    path = Path(path); data = path.read_bytes(); starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    if not starts or any(data[:starts[0].start()]): raise ValueError('Annex-B input required')
    out = bytearray(); pps_count = vcl_count = 0
    for index, match in enumerate(starts):
        nal = data[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(data)]
        if len(nal) < 2 or nal[0] & 128 or not nal[1] & 7: raise ValueError('invalid HEVC NAL')
        kind = (nal[0] >> 1) & 63
        if kind == 34 or kind <= 31:
            bits = ''.join(f'{byte:08b}' for byte in rbsp(nal[2:]))
            begin = 0 if kind == 34 else 2 if 16 <= kind <= 23 else 1
            original, end = ue(bits, begin)
            if original: raise ValueError('fixture requires a consistent original PPS0')
            bits = bits[:begin] + code + bits[end:]
            nal = nal[:2] + ebsp(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)))
            if kind == 34: pps_count += 1
            else: vcl_count += 1
        out.extend(b'\x00\x00\x00\x01' + nal)
    if not pps_count or not vcl_count: raise ValueError('missing PPS or VCL')
    path.write_bytes(out)
    return pps_count, vcl_count
