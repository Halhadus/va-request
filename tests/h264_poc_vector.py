"""Express a progressive POC2 fixture as equivalent POC1 syntax."""
from pathlib import Path
import re
from hevc_pps_vector import rbsp, ebsp


def rewrite_poc_type1(path):
    path = Path(path)
    data = path.read_bytes()
    starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    out = bytearray()
    changed = 0
    for i, match in enumerate(starts):
        nal = data[match.end():starts[i + 1].start() if i + 1 < len(starts) else len(data)]
        if nal[0] & 31 == 7:
            bits = ''.join(f'{x:08b}' for x in rbsp(nal[1:]))
            pos = 24
            def get_ue():
                nonlocal pos
                end = bits.find('1', pos)
                if end < 0 or end - pos > 31: raise ValueError('truncated SPS')
                stop = 2 * end - pos + 1
                if stop > len(bits): raise ValueError('truncated SPS suffix')
                value = int(bits[end:stop], 2) - 1
                pos = stop
                return value
            get_ue()  # SPS id
            if nal[1] >= 100:
                if get_ue() != 1 or get_ue() or get_ue(): raise ValueError('fixture must be 8-bit 4:2:0')
                if bits[pos:pos + 2] != '00': raise ValueError('fixture must omit SPS scaling lists')
                pos += 2
            get_ue()  # log2_max_frame_num_minus4
            begin = pos
            if get_ue() != 2: raise ValueError('fixture requires POC2')
            # POC1: delta_always_zero=1, nonref=-1, top-bottom=0,
            # cycle=[2]. This gives exactly the POC2 frame-order sequence.
            bits = bits[:begin] + '010' + '1' + '011' + '1' + '010' + '00100' + bits[pos:]
            bits = bits.rstrip('0')
            bits += '0' * (-len(bits) % 8)
            nal = nal[:1] + ebsp(bytes(int(bits[j:j + 8], 2) for j in range(0, len(bits), 8)))
            changed += 1
        out.extend(b'\x00\x00\x00\x01' + nal)
    if not changed: raise ValueError('missing SPS')
    path.write_bytes(out)
