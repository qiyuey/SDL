"""Compare bounded production queue depths using public calls and matched ETW.

Host request completion is not a link ACK or an actuator-onset measurement.
Raw ETL/XML and per-state captures must stay in ignored build output.
"""
import argparse
from bisect import bisect_left
import json
from pathlib import Path
import re
import statistics

from analyze_switch2_transport import read_etw


def stats(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    def percentile(p):
        pos = (len(values) - 1) * p
        lo = int(pos)
        hi = min(lo + 1, len(values) - 1)
        return values[lo] + (values[hi] - values[lo]) * (pos - lo)
    return dict(n=len(values), mean=statistics.mean(values), p50=percentile(.5),
                p95=percentile(.95), p99=percentile(.99), max=values[-1])


def analyze(base, offset):
    text = base.with_suffix('.log').read_text(encoding='utf-8-sig')
    result = re.search(r'RESULT slots=(\d+) failures=(\d+) calls=(\d+)', text)
    if not result or int(result[2]):
        raise ValueError(f'{base}: missing result or API failure')
    depth = int(result[1])
    pubs = [dict(id=int(m[1]), phase=m[2], trial=int(m[3]), width=int(m[4]),
                 active=int(m[5]), due=int(m[6]), begin=int(m[7]), end=int(m[8]))
            for m in re.finditer(r'PUB id=(\d+) phase=(\w+) trial=(\d+) width=(\d+) active=(\d+) due_ns=(\d+) begin_ns=(\d+) end_ns=(\d+) ok=1', text)]
    if len(pubs) != int(result[3]):
        raise ValueError('Missing publication records')
    begins = [p['begin'] for p in pubs]
    states = {}
    for m in re.finditer(r'async publish: state=(\d+) ns=(\d+) stop=(\d+)', text):
        state, at, stop = map(int, m.groups())
        index = bisect_left(begins, at)
        if index == len(pubs) or begins[index] > at:
            index -= 1
        source = pubs[index] if index >= 0 and pubs[index]['begin'] <= at <= pubs[index]['end'] else None
        states[state] = dict(at=at, stop=stop, source=source)
    submissions = []
    for m in re.finditer(r'async submit: order=(\d+) seq=(\d+) start_ns=(\d+) stop=(\d+) inflight=(\d+) .*? state=(\d+) queued_ns=(\d+)', text):
        order, seq, at, stop, inflight, state, queued = map(int, m.groups())
        if state not in states or states[state]['at'] != queued:
            raise ValueError('Missing/mismatched state identity')
        submissions.append(dict(order=order, seq=seq, at=at, stop=stop, inflight=inflight,
                                state=state, source=states[state]['source']))
    callbacks = {int(m[1]): dict(at=int(m[2]), status=int(m[3]), size=int(m[4]), stop=int(m[5]))
                 for m in re.finditer(r'async callback: order=(\d+) ns=(\d+) status=(\d+) bytes=(\d+) stop=(\d+)', text)}
    retirement = [int(m[1]) for m in re.finditer(r'async retire: order=(\d+)', text)]
    expected = list(range(len(submissions)))
    if [s['order'] for s in submissions] != expected or retirement != expected or sorted(callbacks) != expected:
        raise ValueError('Incomplete or unordered submit/retirement')
    if len(re.findall(r'async callback:', text)) != len(callbacks):
        raise ValueError('Duplicate callbacks')
    if any(s['seq'] != s['order'] % 16 or s['inflight'] > depth for s in submissions):
        raise ValueError('Sequence/depth violation')
    if any(c['status'] not in (0, 3) or (c['status'] == 0 and c['size'] != 64) for c in callbacks.values()):
        raise ValueError('Failed/short USB transfer')
    for s in submissions:
        if s['stop']:
            if any(callbacks[i]['at'] > s['at'] for i in range(s['order'])):
                raise ValueError('Stop submitted before old callbacks drained')
            if callbacks[s['order']]['status'] != 0:
                raise ValueError('Stop did not succeed')
    if not submissions or not submissions[-1]['stop']:
        raise ValueError('Final report was not silence')
    kernel, lost = read_etw(Path(str(base) + '-etw.xml'), offset)
    kernel.sort(key=lambda r: r['begin_ns'])
    if len(kernel) != len(submissions):
        raise ValueError(f'Cannot match user/kernel requests: {len(submissions)} vs {len(kernel)}')
    offsets = [k['begin_ns'] - s['at'] for s, k in zip(submissions, kernel)]
    clock = int(statistics.median(offsets))
    deviation = max(abs(d-clock) for d in offsets)
    if deviation > 1_000_000:
        raise ValueError(f'User/kernel dispatch mismatch: {deviation} ns')
    for s, k in zip(submissions, kernel):
        s['kernel_end'] = k['end_ns'] - clock
        s['success'] = callbacks[s['order']]['status'] == 0
        if s['success'] and k['status'] != '0x0':
            raise ValueError('Kernel/user status mismatch')
    success_ends = [s['kernel_end'] for s in submissions if s['success']]
    if success_ends != sorted(success_ends):
        raise ValueError('Successful kernel completions out of submit order')
    out = dict(slots=depth, calls=len(pubs), requests=len(submissions), etw_loss=lost,
               clock_matching_max_deviation_us=deviation/1000, curves={}, pulses={}, stops={},
               verified='ordered submit/retirement, sequence, bounded depth, callback drain before zero, successful final zero')
    for phase in ('curve4', 'curve5'):
        source = [p for p in pubs if p['phase'] == phase and p['active']]
        sent = [s for s in submissions if not s['stop'] and s['source'] and s['source']['phase'] == phase]
        completed = [s for s in sent if s['success']]
        matched = {s['source']['id'] for s in sent}
        gaps = [(b['kernel_end']-a['kernel_end'])/1e6 for a, b in zip(completed, completed[1:])]
        # Subtract source spacing: 5 ms input naturally maps to 4/8 ms USB service.
        extra = [(b['kernel_end']-a['kernel_end'] - (b['source']['begin']-a['source']['begin']))/1e6
                 for a, b in zip(completed, completed[1:])]
        out['curves'][phase] = dict(
            produced=len(source), submitted=len(sent), completed=len(completed),
            max_inflight=max(s['inflight'] for s in sent),
            not_submitted=sum(p['id'] not in matched for p in source),
            generation_to_submit_ms=stats([(s['at']-s['source']['begin'])/1e6 for s in sent]),
            generation_to_kernel_ms=stats([(s['kernel_end']-s['source']['begin'])/1e6 for s in completed]),
            submit_to_kernel_ms=stats([(s['kernel_end']-s['at'])/1e6 for s in completed]),
            api_call_ms=stats([(p['end']-p['begin'])/1e6 for p in source]),
            source_lateness_ms=stats([(p['begin']-p['due'])/1e6 for p in source]),
            kernel_gap_ms=stats(gaps), gaps_6_to_10ms=sum(6 < g <= 10 for g in gaps),
            gaps_over_10ms=sum(g > 10 for g in gaps),
            gaps_over_source_spacing_plus_4_5ms=sum(g > 4.5 for g in extra))
        out['curves'][phase]['_samples'] = dict(
            generation_to_kernel_ms=[(s['kernel_end']-s['source']['begin'])/1e6 for s in completed],
            generation_to_submit_ms=[(s['at']-s['source']['begin'])/1e6 for s in sent],
            source_lateness_ms=[(p['begin']-p['due'])/1e6 for p in source], kernel_gap_ms=gaps)
    for width in (1, 2, 4, 5, 8, 12):
        starts = [p for p in pubs if p['phase'] == 'pulse' and p['width'] == width and p['active']]
        delivered = 0
        submitted = cancelled = 0
        completed_before_stop = 0
        latencies, stop_latencies, host_durations = [], [], []
        for p in starts:
            stop = next(q for q in pubs if q['phase'] == 'pulse' and q['width'] == width and q['trial'] == p['trial'] and not q['active'])
            attempted = [s for s in submissions if not s['stop'] and s['source'] and s['source']['id'] == p['id']]
            submitted += bool(attempted)
            cancelled += bool(attempted) and not any(s['success'] for s in attempted)
            active = [s for s in attempted if s['success']]
            zero = [s for s in submissions if s['stop'] and s['success'] and s['source'] and s['source']['id'] == stop['id']]
            if active:
                delivered += 1
                first = min(s['kernel_end'] for s in active)
                latencies.append((first-p['begin'])/1e6)
                completed_before_stop += first <= stop['begin']
                if zero:
                    host_durations.append((zero[0]['kernel_end']-first)/1e6)
            if zero:
                stop_latencies.append((zero[0]['kernel_end']-stop['begin'])/1e6)
        out['pulses'][str(width)] = dict(trials=len(starts), active_submitted=submitted,
            active_cancelled_without_success=cancelled, active_never_submitted=len(starts)-submitted,
            active_success=delivered,
            active_completed_before_stop=completed_before_stop,
            active_generation_to_kernel_ms=stats(latencies), stop_to_kernel_ms=stats(stop_latencies),
            host_completion_active_to_zero_ms=stats(host_durations))
    burst_stops = [s for s in submissions if s['stop'] and s['source'] and s['source']['phase'] == 'burst']
    expected_burst_stops = sum(p['phase'] == 'burst' and not p['active'] for p in pubs)
    if len(burst_stops) != expected_burst_stops:
        raise ValueError('Missing burst stop')
    out['stops'] = dict(trials=expected_burst_stops,
        request_to_zero_kernel_ms=stats([(s['kernel_end']-s['source']['begin'])/1e6 for s in burst_stops]),
        request_to_zero_callback_ms=stats([(callbacks[s['order']]['at']-s['source']['begin'])/1e6 for s in burst_stops]),
        max_inflight=max(s['inflight'] for s in submissions), cancelled=sum(c['status'] == 3 for c in callbacks.values()))
    out['stops']['_samples'] = dict(request_to_zero_kernel_ms=[(s['kernel_end']-s['source']['begin'])/1e6 for s in burst_stops])
    out['endpoint'] = kernel[0]['endpoint']
    return out


def compare(results):
    comparison = {}
    for depth in range(1, 5):
        runs = [r for r in results if r['slots'] == depth]
        if not runs:
            continue
        row = dict(runs=len(runs), curves={}, pulses={})
        for phase in ('curve4', 'curve5'):
            records = [r['curves'][phase] for r in runs]
            row['curves'][phase] = {
                key: sum(r[key] for r in records) for key in
                ('produced', 'submitted', 'completed', 'not_submitted', 'gaps_6_to_10ms',
                 'gaps_over_10ms', 'gaps_over_source_spacing_plus_4_5ms')}
            row['curves'][phase].update({key: stats([v for r in records for v in r['_samples'][key]])
                                        for key in records[0]['_samples']})
            row['curves'][phase]['max_inflight'] = max(r['max_inflight'] for r in records)
        row['stop_to_kernel_ms'] = stats([v for r in runs for v in r['stops']['_samples']['request_to_zero_kernel_ms']])
        for width in runs[0]['pulses']:
            row['pulses'][width] = {key: sum(r['pulses'][width][key] for r in runs)
                for key in ('trials', 'active_submitted', 'active_cancelled_without_success',
                            'active_never_submitted', 'active_success', 'active_completed_before_stop')}
        comparison[str(depth)] = row
    return comparison


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--xml-wall-offset-minutes', type=int, required=True)
    parser.add_argument('--runs', nargs='+', required=True)
    args = parser.parse_args()
    results = []
    for name in args.runs:
        result = analyze(args.directory / name, args.xml_wall_offset_minutes)
        results.append(result)
        (args.directory / f'{name}-analysis.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(name, json.dumps({p: {k: v for k, v in r.items() if k in ('produced', 'completed', 'not_submitted', 'gaps_6_to_10ms', 'generation_to_kernel_ms')}
                                for p, r in result['curves'].items()}))
    (args.directory / 'comparison.json').write_text(json.dumps(compare(results), indent=2)+'\n', encoding='utf-8')
