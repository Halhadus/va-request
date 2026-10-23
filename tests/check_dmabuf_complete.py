"""Require public PRIME3/lifetime/resource cases and real new DMA transports."""
import csv
from collections import Counter, defaultdict
from pathlib import Path
import sys

FORMATS = ('NV12', 'NV21', 'I420', 'YV12', 'P010', 'YUY2', 'UYVY', 'VYUY', 'YVYU', '422H', 'YV16', 'Y210', 'ARGB', 'BGRA', 'BGRX', 'RGB565', 'RGBA', 'RGBX')
RGB = {'RGB565': 'RG16', 'RGBA': 'RGBA', 'RGBX': 'RGBX'}
CASES = {f'complete-prime3-{name}-{layer}' for name in FORMATS for layer in ('composed', 'separate')}
CASES |= {f'complete-rgb-{kind}-{name}' for name in RGB for kind in ('warm', 'cold')}
CASES |= {'complete-modifier-contract', 'complete-fd-pressure-recovery'}
CASES |= {f'complete-jpeg-dma-{name}' for name in ('NV12', 'I420', 'YV12')}

def fourcc(name):
    return f"{int.from_bytes(name.encode(), 'little'):08x}"

def table(path, fields):
    with Path(path).open() as file:
        reader = csv.DictReader(file, delimiter='\t')
        if not set(fields).issubset(reader.fieldnames or []): raise ValueError('missing evidence columns')
        return list(reader)

def check(results, vpp, jpeg):
    with Path(results).open() as file: rows = list(csv.reader(file, delimiter='\t'))
    cases = [row for row in rows if row and row[0] in ('PASS', 'FAIL', 'SKIP')]
    if sum(row[:2] == ['COMPLETE', '1'] for row in rows) != 1 or len(cases) != len(CASES) or {row[1] for row in cases if len(row) > 1} != CASES or any(row[0] != 'PASS' for row in cases):
        raise ValueError('missing, duplicate, failed, skipped or incomplete closure case')
    prime = [row for row in rows if row and row[0] == 'PRIME_EVIDENCE']
    if len(prime) != 36 or {(row[1], row[2]) for row in prime if len(row) == 6} != {(name, layer) for name in FORMATS for layer in ('composed', 'separate')} or any(row[3:] != ['32', '3', '1'] for row in prime):
        raise ValueError('missing or incomplete PRIME3 cycle/rejection/inherited-FD evidence')
    counts, contexts = Counter(), defaultdict(set)
    for row in table(vpp, ['STATUS', 'SRC_FOURCC', 'DST_FOURCC', 'QUEUE_VALID', 'OUTPUT_MEMORY', 'CAPTURE_MEMORY', 'OUTPUT_PLANES', 'CAPTURE_PLANES', 'CONTEXT']):
        if any(row[k] != value for k, value in [('STATUS', '0'), ('QUEUE_VALID', '1'), ('OUTPUT_MEMORY', '4'), ('CAPTURE_MEMORY', '4'), ('OUTPUT_PLANES', '1'), ('CAPTURE_PLANES', '1')]):
            raise ValueError('RGB processing failed or used staging')
        if row['SRC_FOURCC'] != row['DST_FOURCC'] or row['SRC_FOURCC'] not in {fourcc(f) for f in RGB.values()}:
            raise ValueError('unexpected RGB format pair')
        context = int(row['CONTEXT'])
        if context <= 0: raise ValueError('missing context identity')
        counts[row['SRC_FOURCC']] += 1; contexts[row['SRC_FOURCC']].add(context)
    if counts != {fourcc(f): 40 for f in RGB.values()} or any(len(contexts[fourcc(f)]) < 9 for f in RGB.values()):
        raise ValueError('missing RGB jobs or cold contexts')
    direct, staged = Counter(), 0
    for row in table(jpeg, ['STAGE', 'FORMAT', 'WIDTH', 'HEIGHT', 'PLANES', 'OUTPUT_MEMORY', 'CAPTURE_MEMORY']):
        if row['STAGE'] != 'complete' or row['WIDTH'] != '320' or row['HEIGHT'] != '240' or row['CAPTURE_MEMORY'] != '1':
            raise ValueError('JPEG completion/geometry/coded memory mismatch')
        if row['OUTPUT_MEMORY'] == '4':
            wanted = {fourcc('NM12'): '2', fourcc('YM12'): '3'}
            if row['FORMAT'] not in wanted or row['PLANES'] != wanted[row['FORMAT']]: raise ValueError('JPEG DMA raw-plane mismatch')
            direct[row['FORMAT']] += 1
        elif row['OUTPUT_MEMORY'] == '1': staged += 1
        else: raise ValueError('unknown JPEG transport')
    if direct != {fourcc('NM12'): 4, fourcc('YM12'): 8} or staged != 12:
        raise ValueError('missing direct JPEG inputs or independent staged references')
    return sum(counts.values()), sum(direct.values())

if __name__ == '__main__':
    try: vpp, jpeg = check(*sys.argv[1:])
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        print('DMA closure evidence:', error, file=sys.stderr); sys.exit(1)
    print(f'DMA closure evidence:47 cases,18 PRIME3 formats,1152 batch cycles,{vpp} RGB DMA jobs,{jpeg} JPEG DMA jobs')
