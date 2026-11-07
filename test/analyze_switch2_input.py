"""Compare raw 057e:2069 input reports, physical IN completions and SDL events.

ETW completion is host-stack evidence, not an electrical USB analyzer capture.
Only this controller's interrupt IN 0x81 is counted; bulk ACKs are excluded.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import statistics
import xml.etree.ElementTree as ET

from analyze_switch2_transport import NS, EPOCH_NS, wall_ns


def summary(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    def percentile(p):
        at = (len(values)-1)*p
        lo = int(at)
        hi = min(lo+1, len(values)-1)
        return values[lo] + (values[hi]-values[lo])*(at-lo)
    return dict(n=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=percentile(.95), p99=percentile(.99), min=values[0], max=values[-1])


def gaps(times):
    return summary([(b-a)/1e6 for a, b in zip(times, times[1:])])


def kernel_input(path, offset):
    devices, descriptors, events, lost = {}, {}, [], {}
    for _, event in ET.iterparse(path, events=('end',)):
        if not event.tag.endswith('}Event'):
            continue
        system = event.find('w:System', NS)
        provider = system.find('w:Provider', NS).attrib.get('Name', '')
        event_id = system.findtext('w:EventID', namespaces=NS)
        data = {v.attrib.get('Name', ''): v.text for v in event.findall('.//w:Data', NS)}
        if int(data.get('fid_idVendor', '0'), 0) == 0x057e and int(data.get('fid_idProduct', '0'), 0) == 0x2069:
            devices[data['fid_UsbDevice']] = {k: data.get(k) for k in
                ('DeviceSpeed', 'PortPathDepth', 'PortPath', 'fid_bcdUSB', 'fid_bcdDevice')}
        for key in ('EventsLost', 'BuffersLost'):
            if key in data:
                lost[key] = int(data[key])
        if provider.endswith('USB-UCX') and event_id == '6':
            descriptors[data.get('fid_PipeHandle')] = data
        if provider.endswith('USB-UCX') and event_id in ('26', '27'):
            at = wall_ns(system.find('w:TimeCreated', NS).attrib['SystemTime'], offset)
            events.append((event_id, at, data))
        event.clear()
    if not devices or any(lost.values()):
        raise ValueError(f'Missing target device or lost ETW events: {lost}')
    pending, completed = {}, []
    for event_id, at, data in events:
        if data.get('fid_UsbDevice') not in devices:
            continue
        key = data.get('fid_IRP_Ptr'), data.get('fid_URB_Ptr')
        if event_id == '26':
            pending[key] = at, data
        elif key in pending:
            begin, before = pending.pop(key)
            desc = descriptors.get(data.get('fid_PipeHandle'), {})
            if int(desc.get('fid_bEndpointAddress', '0'), 0) != 0x81:
                continue
            if int(desc.get('fid_bmAttributes', '0'), 0) & 3 != 3:
                raise ValueError('Target IN is not interrupt')
            if int(before.get('fid_URB_TransferFlags', '0'), 0) & 1 != 1:
                raise ValueError('Target direction is not IN')
            if data.get('fid_IRP_NtStatus') == '0x0':
                completed.append(dict(begin=begin, end=at, requested=int(before['fid_URB_TransferBufferLength'], 0)))
    completed.sort(key=lambda r: r['end'])
    endpoint = [{k: d.get(k) for k in ('fid_bEndpointAddress', 'fid_bmAttributes', 'fid_wMaxPacketSize', 'fid_bInterval')}
                for d in descriptors.values() if d.get('fid_UsbDevice') in devices and
                int(d.get('fid_bEndpointAddress', '0'), 0) == 0x81]
    if not completed or not endpoint:
        raise ValueError('No target interrupt IN completions')
    return dict(devices=list(devices.values()), endpoint=endpoint, loss=lost), completed


def analyze(base, offset):
    text = base.with_suffix('.log').read_text(encoding='utf-8-sig')
    if 'RESULT' not in text:
        raise ValueError('Incomplete capture')
    if re.search(r'failures=[1-9]', text):
        raise ValueError('Failed native reads')
    raw = [(int(m[1]), bytes.fromhex(m[3])) for m in re.finditer(
        r'Switch2 raw IN: ns=(\d+) size=(\d+) data=([a-f0-9]+)', text)
        if len(bytes.fromhex(m[3])) == int(m[2])]
    if not raw:
        raise ValueError('No raw input reports')
    meta, kernel = kernel_input(Path(str(base)+'-etw.xml'), offset)
    anchor = re.search(r'ANCHOR ns=(\d+) filetime_100ns=(\d+) uncertainty_ns=(\d+)', text)
    clock = int(anchor[2])*100 - EPOCH_NS - int(anchor[1]) if anchor else None
    result = dict(capture=base.name, **meta, raw_reports=len(raw),
                  report_ids=dict(Counter(f'0x{b[0]:02x}' for _, b in raw)),
                  kernel_successes=len(kernel), kernel_completion_gap_ms=gaps([k['end'] for k in kernel]),
                  kernel_mean_hz=(len(kernel)-1)*1e9/(kernel[-1]['end']-kernel[0]['end']),
                  clock_anchor_uncertainty_ns=int(anchor[3]) if anchor else None, phases={})
    for m in re.finditer(r'SAMPLE phase=([\w-]+) begin_ns=(\d+) end_ns=(\d+) gyro=(\d+) accel=(\d+) axes=(\d+) buttons=(\d+)', text):
        phase, begin, end, gyro, accel, axes, buttons = m.groups()
        begin, end = int(begin), int(end)
        frames = [(at, b) for at, b in raw if begin <= at < end]
        duration = (end-begin)/1e9
        ids = Counter(f'0x{b[0]:02x}' for _, b in frames)
        counters = [int.from_bytes(b[1:5], 'little') for _, b in frames if b[0] == 5 and len(b) == 64]
        steps = Counter((b-a) & 0xffffffff for a, b in zip(counters, counters[1:]))
        sensor_times = [int.from_bytes(b[43:47], 'little') for _, b in frames if b[0] == 5 and len(b) == 64]
        sensor_steps = Counter((b-a) & 0xffffffff for a, b in zip(sensor_times, sensor_times[1:]))
        row = dict(seconds=duration, raw_count=len(frames), raw_hz=len(frames)/duration,
                   raw_read_gap_ms=gaps([at for at, _ in frames]), report_ids=dict(ids),
                   exact_adjacent_duplicates=sum(a[1] == b[1] for a, b in zip(frames, frames[1:])),
                   counter05_steps=dict(steps), sensor05_timestamp_steps=dict(sensor_steps),
                   gyro_events=int(gyro), accel_events=int(accel), axes_events=int(axes), button_events=int(buttons),
                   gyro_hz=int(gyro)/duration, accel_hz=int(accel)/duration,
                   combined_sensor_event_hz=(int(gyro)+int(accel))/duration)
        if clock is not None:
            physical = [k['end'] for k in kernel if begin+clock <= k['end'] < end+clock]
            row.update(kernel_count=len(physical), kernel_hz=len(physical)/duration,
                       kernel_gap_ms=gaps(physical))
        result['phases'][phase] = row
    result['scope'] = 'One report versus software events; host USB completions, not electrical ACKs. Sensor timestamp units are not assumed.'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--xml-wall-offset-minutes', type=int, required=True)
    parser.add_argument('--runs', nargs='+', required=True)
    args = parser.parse_args()
    for name in args.runs:
        result = analyze(args.directory/name, args.xml_wall_offset_minutes)
        (args.directory/f'{name}-analysis.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(name, 'kernel_hz=', round(result['kernel_mean_hz'], 3), 'raw_reports=', result['raw_reports'],
              'ids=', result['report_ids'], 'phases=',
              {k: round(v['raw_hz'], 3) for k, v in result['phases'].items()})
