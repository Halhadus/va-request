"""Keep process exits,exact frame counts and decoder failure stages together."""
import json
from pathlib import Path
import sys


def frames(path):
    out = []
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith('#'): continue
        fields = [x.strip() for x in line.split(',')]
        if len(fields) != 6 or int(fields[0]) or int(fields[4]) <= 0 or len(fields[5]) != 32 or any(x not in '0123456789abcdef' for x in fields[5].lower()): raise ValueError('invalid frame record')
        out.append((int(fields[4]), fields[5].lower()))
    return out


def summarize(tag, expected, hardware_exit, software_exit, trace, hardware, reference):
    record = dict(case=tag, expected=expected, hardware_exit=hardware_exit, software_exit=software_exit, trace_present=Path(trace).is_file(), last_events=[], errors=[])
    for key, path in [('hardware', hardware), ('software', reference)]:
        try: record[key + '_frames'] = frames(path)
        except (OSError, ValueError) as error: record[key + '_frames'] = []; record['errors'].append(str(error))
    if record['trace_present']:
        try:
            for line in Path(trace).read_text().splitlines():
                event = dict(field.split('=', 1) for field in line.split('\t'))
                if 'STAGE' not in event or 'STATUS' not in event: raise ValueError('incomplete decoder event')
                record['last_events'] = (record['last_events'] + [event])[-12:]
        except (OSError, ValueError) as error: record['errors'].append(str(error))
    h, s = record.pop('hardware_frames'), record.pop('software_frames')
    record.update(hardware_frames=len(h), software_frames=len(s), emitted_prefix_matches=bool(h) and h == s[:len(h)])
    record['complete_match'] = not hardware_exit and not software_exit and len(h) == len(s) == expected and h == s and not record['errors']
    return record


def main():
    try:
        tag, expected, he, se, trace, hardware, reference, output = sys.argv[1:]
        record = summarize(tag, int(expected), int(he), int(se), trace, hardware, reference)
        Path(output).write_text(json.dumps(record, indent=2) + '\n')
    except (OSError, ValueError) as error: print('Decode evidence:', error, file=sys.stderr); return 1
    print(f"DECODE_EVIDENCE {tag} hardware={record['hardware_frames']}/{expected} software={record['software_frames']}/{expected} hw-exit={he} sw-exit={se} prefix={int(record['emitted_prefix_matches'])} trace={int(record['trace_present'])}")
    return 0


if __name__ == '__main__': sys.exit(main())
