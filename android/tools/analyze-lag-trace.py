#!/usr/bin/env python3
"""Validate a private schema-44 capture and correlate host/GPU dependencies.

Intervals are observations, not proof of a causal bottleneck. GPU timestamps
include device dependencies; MMIO bursts include gaps between repeated reads.
No nested or cross-thread durations are summed into a frame budget.
"""
import argparse
import bisect
import csv
import io
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


def tick_delta(begin, end, bits):
    return (end - begin) & ((1 << bits) - 1)


def signed_ticks(value, reference, bits):
    delta = tick_delta(reference, value, bits)
    return delta - (1 << bits) if delta >= (1 << (bits - 1)) else delta


def parse(text):
    lines = text.splitlines()
    if len(lines) < 3:
        raise ValueError('Incomplete capture header, records or footer')
    match = re.fullmatch(
        r'# xemu_lag schema=44 token=(\d+) pid=(\d+) begin_us=(\d+) '
        r'end_us=(\d+) phase_min_us=50 capacity=(\d+) clock=MONOTONIC', lines[0])
    if not match:
        raise ValueError('Unsupported or missing capture header')
    token, pid, begin, end, capacity = map(int, match.groups())
    footer = re.fullmatch(r'# complete records=(\d+) dropped=(\d+)', lines[-1])
    if not footer:
        raise ValueError('Incomplete export: capture footer is missing')
    count, dropped = map(int, footer.groups())
    if dropped:
        raise ValueError(f'Capture overflowed: {dropped} records lost; reduce capture length')
    if not begin <= end <= begin + 30100000 or count > capacity:
        raise ValueError('Invalid capture bounds or capacity')
    events = list(csv.DictReader(io.StringIO('\n'.join(lines[1:-1]))))
    if len(events) != count:
        raise ValueError('Record count differs from footer')
    keys = ['seq', 'tid', 'kind', 'start_us', 'end_us', 'frame', 'id', 'link', 'a', 'b', 'c']
    for i, event in enumerate(events, 1):
        if list(event) != keys:
            raise ValueError('Unexpected event schema')
        for key in keys:
            if key != 'kind':
                event[key] = int(event[key])
                if event[key] < 0:
                    raise ValueError('Negative unsigned field')
        if event['seq'] != i or not 0 < event['tid']:
            raise ValueError('Missing, duplicate or reordered records')
        if not event['start_us'] <= event['end_us'] <= end:
            raise ValueError('Invalid event interval')
        if event['end_us'] < begin:
            raise ValueError('Event predates capture')
    return {'token': token, 'pid': pid, 'begin_us': begin, 'end_us': end,
            'records': count, 'dropped': dropped}, events


def analyze(text, begin_us=None, end_us=None):
    meta, events = parse(text)
    caps = {(e['a'], e['b']) for e in events if e['kind'] == 'gpu_caps'}
    if any(not 36 <= bits <= 64 or not period for bits, period in caps) or len(caps) > 1:
        raise ValueError('Inconsistent GPU timestamp capabilities')
    bits, period = next(iter(caps), (0, 0))
    calibrations = [e for e in events if e['kind'] == 'calibration']
    calibrations = [e for e in calibrations if
                    e['start_us'] * 1000 - e['c'] <= e['b'] <=
                    e['end_us'] * 1000 + e['c'] + 1000]
    by_id = defaultdict(list)
    for e in events:
        if e['id'] and e['kind'] != 'flip':
            by_id[e['id']].append(e)
    gpu, invalid_alignments = [], []
    for e in events:
        if e['kind'] not in ('gpu_main', 'gpu_aux'):
            continue
        if not bits or e['c'] != period:
            raise ValueError('GPU result has no matching timestamp capabilities')
        result = {'id': e['id'], 'kind': e['kind'],
                  'duration_us': tick_delta(e['a'], e['b'], bits) * period / 1e6,
                  'aligned': False}
        if calibrations:
            cal = min(calibrations, key=lambda c: abs(c['end_us'] - e['end_us']))
            delta0 = signed_ticks(e['a'], cal['a'], bits)
            delta1 = signed_ticks(e['b'], cal['a'], bits)
            start = (cal['b'] + delta0 * period / 1000) / 1000
            end = (cal['b'] + delta1 * period / 1000) / 1000
            # Include encoding error from rounding timestampPeriod to ps.
            error = (cal['c'] + max(abs(delta0), abs(delta1)) / 2000) / 1000
            submits = [x for x in by_id[e['id']] if x['kind'] == 'submit']
            plausible = (end >= start and end <= e['end_us'] + error + 2 and
                         (not submits or start >= min(x['start_us'] for x in submits) - error - 2))
            if plausible and abs(cal['end_us'] - e['end_us']) <= 250000:
                result.update(aligned=True, start_us=start, end_us=end,
                              calibration_error_us=error)
            else:
                invalid_alignments.append(e['id'])
        gpu.append(result)
    selected_begin = meta['begin_us'] if begin_us is None else begin_us
    selected_end = meta['end_us'] if end_us is None else end_us
    if not meta['begin_us'] <= selected_begin < selected_end <= meta['end_us']:
        raise ValueError('Driving markers are outside the complete capture')
    meta['analysis_begin_us'], meta['analysis_end_us'] = selected_begin, selected_end
    frames = sorted([e for e in events if e['kind'] == 'flip' and
                     selected_begin <= e['start_us'] < e['end_us'] <= selected_end],
                    key=lambda e: e['end_us'])
    slow = []
    suspects = sorted([e for e in events if e['kind'] != 'flip' and
                       e['end_us'] - e['start_us'] >= 2000],
                      key=lambda e: e['start_us'])
    cursor, active = 0, []
    for f in frames:
        while cursor < len(suspects) and suspects[cursor]['start_us'] < f['end_us']:
            active.append(suspects[cursor]); cursor += 1
        active = [e for e in active if e['end_us'] > f['start_us']]
        duration = f['end_us'] - f['start_us']
        if duration < 33334:
            continue
        overlaps = []
        for e in active:
            overlap = min(e['end_us'], f['end_us']) - max(e['start_us'], f['start_us'])
            overlaps.append({'kind': e['kind'], 'tid': e['tid'],
                             'id': e['id'], 'waits_for_id': e['link'],
                             'elapsed_us': e['end_us'] - e['start_us'],
                             'overlap_us': overlap,
                             'detail': [e['a'], e['b'], e['c']]})
        slow.append({'frame': f['id'], 'start_us': f['start_us'],
                     'end_us': f['end_us'], 'duration_us': duration,
                     'overlapping_observations': sorted(overlaps,
                         key=lambda e: e['overlap_us'], reverse=True)[:20]})
    gpu_by_id = defaultdict(list)
    for g in gpu:
        gpu_by_id[g['id']].append(g)
    linked_waits = []
    for e in events:
        if e['kind'] not in ('slot_wait', 'ack', 'fence', 'completion_wait'):
            continue
        target = e['link'] or e['id']
        related = by_id.get(target, [])
        chain = {kind: [{k: x[k] for k in ('tid','start_us','end_us','a','b','c')}
                       for x in related if x['kind'] == kind]
                 for kind in ('cb_begin','cb_end','enqueue','worker','submit')}
        linked_waits.append({'kind': e['kind'], 'id': e['id'], 'target': target,
                             'start_us': e['start_us'], 'end_us': e['end_us'],
                             'elapsed_us': e['end_us'] - e['start_us'],
                             'chain': chain,
                             'gpu': gpu_by_id[target],
                             'missing_submit': not chain['submit']})
    return {'capture': meta, 'interpretation': 'Observations; nested/cross-thread intervals are not additive. GPU intervals include dependencies; MMIO bursts include gaps.',
            'event_counts': dict(Counter(e['kind'] for e in events)),
            'complete_frames': len(frames),
            'frames_over_50ms': sum(f['end_us']-f['start_us'] > 50000 for f in frames),
            'slow_frames': sorted(slow, key=lambda f:f['duration_us'], reverse=True),
            'gpu_calibration_samples': len(calibrations),
            'invalid_gpu_alignment_ids': sorted(set(invalid_alignments)),
            'gpu_intervals': gpu,
            'wait_dependencies': sorted(linked_waits,
                key=lambda e:e['elapsed_us'], reverse=True)}, events


def timeline(report, events):
    output = []
    for e in events:
        output.append({'name': e['kind'], 'cat':'host', 'ph':'X',
                       'pid': report['capture']['pid'], 'tid': e['tid'],
                       'ts': e['start_us'], 'dur': e['end_us']-e['start_us'],
                       'args':{k:e[k] for k in ('seq','frame','id','link','a','b','c')}})
    for e in report['gpu_intervals']:
        if e['aligned']:
            output.append({'name':e['kind'], 'cat':'gpu dependency interval',
                           'ph':'X', 'pid': report['capture']['pid'], 'tid':-1,
                           'ts':e['start_us'], 'dur':e['end_us']-e['start_us'],
                           'args':{'id':e['id'], 'calibration_error_us':e['calibration_error_us']}})
    return {'traceEvents':output, 'displayTimeUnit':'ms'}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--timeline', type=Path)
    p.add_argument('--begin-us', type=int)
    p.add_argument('--end-us', type=int)
    args=p.parse_args()
    report, events=analyze(args.capture.read_text(encoding='utf-8'),args.begin_us,args.end_us)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if args.timeline:
        args.timeline.write_text(json.dumps(timeline(report,events)), encoding='utf-8')
    print(f"Validated {report['capture']['records']} events, {report['complete_frames']} frames, {len(report['slow_frames'])} frames over the 30 FPS budget; {report['gpu_calibration_samples']} clock calibrations")
