"""Require actual internal DMA pool completion,not a native fallback."""
from pathlib import Path
import sys


def check(path, frames):
    events = [dict(field.split('=', 1) for field in line.split('\t')) for line in Path(path).read_text().splitlines()]
    configs = [e for e in events if e.get('STAGE') == 'capture-config']
    if not configs or any(e.get('POOL') != '1' or e.get('MEMORY') != '4' or int(e.get('ALLOCATED', '0')) < 24 or e.get('WIDTH') != '3840' or e.get('HEIGHT') != '2160' or int(e.get('SIZEIMAGE', '0')) < 3840 * 2160 * 3 // 2 for e in configs):
        raise ValueError('missing full4K internal DMA pool configuration')
    completes = [e for e in events if e.get('STAGE') == 'capture-pool-complete']
    ends = [e for e in events if e.get('STAGE') == 'end-picture']
    for rows in [completes, ends]:
        if len(rows) < frames or any(e.get('STATUS') != '0' or e.get('POOL') != '1' or e.get('MEMORY') != '4' for e in rows): raise ValueError('missing or failed internal pool decode completion')
        identities = [(e['PID'], e['CONTEXT'], e['FRAME']) for e in rows]
        if len(set(identities)) != len(identities): raise ValueError('duplicate internal pool completion')
    if {(e['PID'], e['CONTEXT'], e['FRAME']) for e in completes} != {(e['PID'], e['CONTEXT'], e['FRAME']) for e in ends}: raise ValueError('pool copy and VA completion identities differ')
    return len(completes)


if __name__ == '__main__':
    try: count = check(sys.argv[1], int(sys.argv[2]))
    except (OSError, ValueError, KeyError, IndexError) as error: print('CAPTURE pool evidence:', error, file=sys.stderr); sys.exit(1)
    print(f'CAPTURE_POOL_VERIFIED frames={count} memory=DMABUF allocation=internal')
