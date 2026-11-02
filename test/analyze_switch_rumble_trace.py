"""Analyze optional SavageWind vibration logs without sending controller output.

Usage: python analyze_switch_rumble_trace.py path/to/savagewind.log --output report.json
Values describe game-service requests, not USB packets or physical measurements.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import statistics

PATTERN = re.compile(r'\[vibration\] ms (\d+) handle (0x[0-9a-fA-F]+) '
                     r'low ([\d.]+)@([\d.]+)Hz high ([\d.]+)@([\d.]+)Hz')


def parse(text):
    rows = []
    for t, handle, low, lf, high, hf in PATTERN.findall(text):
        rows.append(dict(ms=int(t), handle=int(handle, 16), low=float(low),
                         low_hz=float(lf), high=float(high), high_hz=float(hf)))
    return rows


def summarize(rows):
    if not rows:
        raise ValueError('No detailed vibration records found; use the trace launcher first.')
    result = {'records': len(rows), 'measurement': 'game service change requests, not physical output',
              'channels': {}}
    for handle in sorted({r['handle'] for r in rows}):
        channel = [r for r in rows if r['handle'] == handle]
        if any(b['ms'] < a['ms'] for a, b in zip(channel, channel[1:])):
            raise ValueError('Time went backwards; analyze separate game sessions independently.')
        active = [r for r in channel if r['low'] or r['high']]
        amplitudes = [max(r['low'], r['high']) for r in active]
        gaps = [b['ms']-a['ms'] for a, b in zip(channel, channel[1:])]
        bursts, beginning = [], None
        for r in channel:
            if r['low'] or r['high']:
                if beginning is None:
                    beginning = r['ms']
            elif beginning is not None:
                bursts.append(r['ms']-beginning)
                beginning = None
        # Integrate the held request between observed changes. The final state
        # has no known duration and is deliberately excluded.
        integrals = {'low': 0., 'high': 0.}
        for a, b in zip(channel, channel[1:]):
            for band in integrals:
                integrals[band] += a[band] * (b['ms']-a['ms'])
        pairs = Counter((r['low_hz'], r['high_hz']) for r in active)
        codes = [round(v*1023) for v in amplitudes]
        result['channels'][hex(handle)] = {
            'records': len(channel), 'active_records': len(active),
            'request_amplitude_min': min(amplitudes, default=0),
            'request_amplitude_median_by_change': statistics.median(amplitudes) if amplitudes else 0,
            'request_amplitude_max': max(amplitudes, default=0),
            'active_low_records': sum(r['low'] > 0 for r in channel),
            'active_high_records': sum(r['high'] > 0 for r in channel),
            'both_components_active_records': sum(r['low'] > 0 and r['high'] > 0 for r in channel),
            'frequency_pairs': [{'low_hz': f, 'high_hz': g, 'changes': n} for (f,g), n in pairs.most_common()],
            'full_range_code_min': min(codes, default=0), 'full_range_code_max': max(codes, default=0),
            'positive_change_intervals_below_12ms': sum(0 < d < 12 for d in gaps),
            'median_positive_change_interval_ms': statistics.median([d for d in gaps if d > 0]) if any(d > 0 for d in gaps) else None,
            'completed_bursts_ms': bursts, 'unfinished_burst': beginning is not None,
            'amplitude_time_integral_ms': integrals,
        }
    result['limits'] = [
        'Change-weighted medians are not time-weighted intensity.',
        'No inferred game event labels, motor acceleration, Hz validation or HD1 reference response.',
        'Intervals below 12 ms indicate possible output coalescing, not measured packet loss.',
        'A full-range amplitude code is not a physical vibration-strength percentage.',
    ]
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    report = summarize(parse(args.log.read_text(encoding='utf-8', errors='replace')))
    report['source'] = str(args.log.resolve())
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text+'\n', encoding='utf-8')
        print(f"Saved {report['records']} records to {args.output}")
    else:
        print(text)
