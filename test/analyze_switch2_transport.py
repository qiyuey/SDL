"""Analyze only the selected controller's OUT/64-byte ETW requests.

Output is aggregate timing, not raw game/controller assets. Keep ETL/XML/JSONL
captures under ignored build output. Kernel URB completion is not an actuator
measurement or a link-layer USB analyzer trace.
"""
import argparse
import calendar
import datetime
import json
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET

NS = {'w': 'http://schemas.microsoft.com/win/2004/08/events/event'}
EPOCH_NS = 11644473600000000000


def wall_ns(value, offset_minutes):
    # tracerpt on this host printed a malformed +07:59 suffix for UTC+08 wall
    # times. An explicit caller-supplied wall-clock offset avoids silently using it.
    base = datetime.datetime.strptime(value[:19], '%Y-%m-%dT%H:%M:%S')
    fraction = value[20:].split('+')[0].split('-')[0] if value[19:20] == '.' else ''
    seconds = calendar.timegm(base.timetuple()) - offset_minutes * 60
    return seconds * 10**9 + int((fraction + '000000000')[:9])


def stats(values):
    return {'count': len(values), 'mean_ms': statistics.mean(values) if values else None,
            'min_ms': min(values) if values else None, 'max_ms': max(values) if values else None}


def intervals(times):
    return stats([(b-a) / 1e6 for a, b in zip(times, times[1:])])


def read_etw(path, offset_minutes):
    devices, endpoint_descriptors, events = set(), {}, []
    lost = {}
    for _, event in ET.iterparse(path, events=('end',)):
        if not event.tag.endswith('}Event'):
            continue
        system = event.find('w:System', NS)
        provider = system.find('w:Provider', NS).attrib.get('Name', '')
        event_id = system.findtext('w:EventID', namespaces=NS)
        data = {v.attrib.get('Name', ''): v.text for v in event.findall('.//w:Data', NS)}
        if int(data.get('fid_idVendor', '0'), 0) == 0x057e and int(data.get('fid_idProduct', '0'), 0) == 0x2069:
            devices.add(data['fid_UsbDevice'])
        for key in ('EventsLost', 'BuffersLost'):
            if key in data:
                lost[key] = int(data[key])
        if provider.endswith('USB-UCX') and event_id == '6':
            endpoint_descriptors[data.get('fid_PipeHandle')] = data
        if provider.endswith('USB-UCX') and event_id in ('26', '27'):
            at = wall_ns(system.find('w:TimeCreated', NS).attrib['SystemTime'], offset_minutes)
            events.append((event_id, at, data))
        event.clear()
    if not devices:
        raise ValueError('No 057e:2069 descriptor rundown; cannot identify the controller')
    if any(lost.values()):
        raise ValueError(f'ETW dropped events: {lost}')
    outstanding, completed = {}, []
    for event_id, at, data in events:
        if data.get('fid_UsbDevice') not in devices or int(data.get('fid_URB_TransferFlags', '1'), 0) & 1:
            continue
        key = data.get('fid_IRP_Ptr'), data.get('fid_URB_Ptr')
        if event_id == '26':
            outstanding[key] = at, data
        elif key in outstanding:
            began, before = outstanding.pop(key)
            if int(before.get('fid_URB_TransferBufferLength', '0'), 0) == 64:
                desc = endpoint_descriptors.get(data.get('fid_PipeHandle'), {})
                if int(desc.get('fid_bEndpointAddress', '0'), 0) != 1:
                    continue
                completed.append({'begin_ns': began, 'end_ns': at,
                                  'status': data.get('fid_IRP_NtStatus'),
                                  'endpoint': {k: desc.get(k) for k in
                                      ('fid_bEndpointAddress', 'fid_wMaxPacketSize', 'fid_bInterval')}})
    return completed, lost


def analyze(directory, name, offset_minutes):
    rows = [json.loads(line) for line in (directory / f'{name}.jsonl').read_text().splitlines()
            if line.startswith('{')]
    anchor = rows[0]
    utc = anchor['filetime_100ns'] * 100 - EPOCH_NS
    to_utc = lambda q: utc + q - anchor['qpc_ns']
    successful = [r for r in rows if r.get('event') == 'complete' and r['status'] == 0]
    active = [r for r in successful if not r['stop']]
    summary = rows[-1]
    stop_request = next(r for r in rows if r['event'] == 'stop_requested')['ns']
    if any(r['event'] == 'submit' and not r['stop'] and r['begin_ns'] > stop_request for r in rows):
        raise ValueError('Active submission after Stop')
    if summary['pending_after_close']:
        raise ValueError('Unfinished requests at close')
    stop = [r for r in successful if r['stop']]
    if not summary['first_device_error_ns']:
        if len(stop) != 1 or stop[0]['bytes'] != 64:
            raise ValueError('Expected one successful stereo stop')
        if any(r['event'] == 'complete' and not r['stop'] and r['complete_ns'] > stop[0]['complete_ns'] for r in rows):
            raise ValueError('Old active completion after successful Stop')
    result = {'mode': summary['mode'], 'active_successes': len(active),
              'application_completion_spacing': intervals([r['complete_ns'] for r in active]),
              'submission_call': stats([(r['return_ns']-r['submit_ns']) / 1e6 for r in active]),
              'submit_to_completion': stats([(r['complete_ns']-r['submit_ns']) / 1e6 for r in active]),
              'stop_ms': (stop[0]['complete_ns']-stop_request) / 1e6 if stop else None,
              'cancelled': summary['cancelled'], 'failed': summary['failed'],
              'coalesced': summary['coalesced'], 'pending_after_close': summary['pending_after_close'],
              'no_active_submission_after_stop': True,
              'no_active_completion_after_stop_ack': bool(stop)}
    etw_path = directory / f'{name}-etw.xml'
    if etw_path.exists():
        etw, lost = read_etw(etw_path, offset_minutes)
        matches = []
        for call in successful:
            at = to_utc(call['submit_ns'])
            candidates = [r for r in etw if abs(r['begin_ns']-at) < 2_000_000]
            if candidates:
                match = min(candidates, key=lambda r: abs(r['begin_ns']-at))
                etw.remove(match)
                if not call['stop']:
                    matches.append((call, match))
        if len(matches) < len(active) * .95:
            raise ValueError(f'Only {len(matches)}/{len(active)} matched ETW requests; check wall offset')
        if any(e['status'] != '0x0' for _, e in matches):
            raise ValueError('Non-success kernel completion')
        result.update({
            'matched_etw_active': len(matches), 'etw_lost': lost,
            'user_to_ucx_dispatch': stats([(e['begin_ns']-to_utc(r['submit_ns'])) / 1e6 for r, e in matches]),
            'ucx_dispatch_to_complete': stats([(e['end_ns']-e['begin_ns']) / 1e6 for r, e in matches]),
            'ucx_complete_to_user': stats([(to_utc(r['complete_ns'])-e['end_ns']) / 1e6 for r, e in matches]),
            'ucx_completion_spacing': intervals([e['end_ns'] for _, e in matches]),
            'endpoint': matches[0][1]['endpoint'] if matches else None})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--xml-wall-offset-minutes', required=True, type=int)
    parser.add_argument('--runs', nargs='+', default=['native1', 'native2', 'usb1', 'usb2',
                                                   'usb2-long', 'native2-cancel-full', 'usb2-cancel-full'])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = {'measurement': 'Windows host USB request completion, not actuator or link-layer ACK timing',
              'xml_wall_offset_minutes': args.xml_wall_offset_minutes,
              'runs': {name: analyze(args.directory, name, args.xml_wall_offset_minutes) for name in args.runs}}
    text = json.dumps(output, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
