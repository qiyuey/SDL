"""Validate production Switch2 async trace against its controller's ETW OUT pipe."""
import argparse
import json
from pathlib import Path
import re
import statistics
from analyze_switch2_transport import read_etw, intervals


def analyze(log, xml, offset):
    text = log.read_text(encoding='utf-8')
    submissions = [tuple(map(int, row)) for row in re.findall(
        r'async submit: order=(\d+) seq=(\d+) start_ns=(\d+) stop=(\d+) inflight=(\d+)', text)]
    callbacks = [tuple(map(int, row)) for row in re.findall(
        r'async callback: order=(\d+) ns=(\d+) status=(\d+) bytes=(\d+) stop=(\d+)', text)]
    retired = [tuple(map(int, row)) for row in re.findall(
        r'async retire: order=(\d+) stop=(\d+) ok=(\d+)', text)]
    if not submissions:
        raise ValueError('No production async submissions')
    expected = list(range(len(submissions)))
    if [r[0] for r in submissions] != expected or [r[0] for r in retired] != expected:
        raise ValueError('Submission/retirement out of order or incomplete')
    if sorted(r[0] for r in callbacks) != expected:
        raise ValueError('Missing/duplicate callback')
    if any(seq != order % 16 for order, seq, *_ in submissions):
        raise ValueError('Firmware sequence out of order')
    if max(r[4] for r in submissions) > 2:
        raise ValueError('More than two requests in flight')
    if any(r[2] not in (0, 3) or (r[2] == 0 and r[3] != 64) for r in callbacks):
        raise ValueError('Transport failure or short write')
    by_order = {r[0]: r for r in callbacks}
    for order, seq, at, stop, _ in submissions:
        if stop:
            if any(by_order[i][1] > at for i in range(order)):
                raise ValueError('Stop submitted before an older callback drained')
            if by_order[order][2:4] != (0, 64):
                raise ValueError('Stop was not acknowledged successfully')
    if not submissions[-1][3]:
        raise ValueError('Last output was not silence')
    phases = {name: int(at) for name, at in re.findall(r'PHASE ([\w-]+) ns=(\d+)', text)}
    if 'reopen' in phases:
        if any(not r[3] and phases['reopen'] <= r[2] < phases['second-active'] for r in submissions):
            raise ValueError('Stale effect replayed on reopen')
    kernel, lost = read_etw(xml, offset)
    kernel.sort(key=lambda r: r['begin_ns'])
    if len(kernel) != len(submissions):
        raise ValueError('Kernel/user request counts differ; cannot prove one-to-one order')
    # User timestamps have a process-local origin. Monotonic one-to-one matching
    # must fit a constant clock offset within 1 ms, much less than a report period.
    offsets = [k['begin_ns']-s[2] for s, k in zip(submissions, kernel)]
    middle = int(statistics.median(offsets))
    if max(abs(d-middle) for d in offsets) > 1_000_000:
        raise ValueError('Kernel dispatch order/timestamps do not match user submissions')
    active = [(s, k) for s, k in zip(submissions, kernel)
              if not s[3] and by_order[s[0]][2] == 0 and k['status'] == '0x0']
    if 'active' in phases:
        active = [(s, k) for s, k in active if phases['active'] <= s[2] < phases['stop']]
    groups = [[]]
    for pair in active:
        if groups[-1] and pair[0][2] - groups[-1][-1][0][2] > 20_000_000:
            groups.append([])
        groups[-1].append(pair)
    steady = max(groups, key=len)
    return {'log': str(log), 'submissions': len(submissions),
            'successful_active_in_largest_continuous_group': len(steady),
            'kernel_completion_spacing': intervals([k['end_ns'] for s, k in steady]),
            'callbacks_cancelled': sum(r[2] == 3 for r in callbacks),
            'kernel_user_fifo_verified': True, 'firmware_sequence_mod16_verified': True,
            'max_in_flight': max(r[4] for r in submissions), 'stop_after_drain_verified': True,
            'no_stale_reopen': True if 'reopen' in phases else None,
            'last_report_silence_successful': True, 'etw_loss': lost,
            'scope': 'Host transfer order/completion; not actuator latency or physical unplug acceptance'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('xml', type=Path)
    p.add_argument('--xml-wall-offset-minutes', type=int, required=True)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    text = json.dumps(analyze(a.log, a.xml, a.xml_wall_offset_minutes), indent=2) + '\n'
    if a.output:
        a.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
