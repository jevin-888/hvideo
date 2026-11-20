"""Opt-in device test: NEXT, observe new song starting, immediately REPLAY.

Changes live playback/queue. Run only on an authorized test device.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--device', required=True)
p.add_argument('--room', required=True)
p.add_argument('--version', required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--rounds', type=int, default=3)
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
base = f'http://{args.device}:8080'
token = None


def request(path, data=None):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(base + path,
                                 json.dumps(data).encode() if data is not None else None,
                                 headers)
    with urllib.request.urlopen(req, timeout=30) as response:
        result = json.load(response)
    assert result.get('ok'), result
    return result['data']


def status():
    return request('/api/v1/video/status?layerId=1')


def screenshot(name):
    result = subprocess.run(['adb', '-s', args.device + ':5555', 'exec-out', 'screencap', '-p'],
                            capture_output=True, check=True, timeout=10)
    assert result.stdout.startswith(b'\x89PNG'), result.stderr
    (args.output / name).write_bytes(result.stdout)


token = request('/api/v1/auth/login', {
    'username': 'admin', 'password': 'admin*', 'loginType': 'local'})['token']
report = {'version': args.version, 'device': args.device, 'room': args.room, 'rounds': []}
try:
    for i in range(args.rounds):
        old = status()
        switch_start = time.monotonic()
        request(f'/api/v1/rooms/{args.room}/actions/next', {})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            new = status()
            if (new['path'] != old['path'] and new['state'] == 'playing'
                    and new['current_position'] >= .12):
                break
            time.sleep(.04)
        else:
            raise AssertionError(('New song did not start', old, new))
        assert new['current_position'] < 2, ('Missed startup window', new)
        at_replay = status()
        assert at_replay['path'] == new['path'] and at_replay['current_position'] < 2
        start = time.monotonic()
        response = request('/api/v1/video/replay', {'layerId': 1})
        elapsed = round((time.monotonic() - start) * 1000, 1)
        after = status()
        if i == 0:
            screenshot(f'{args.version}-immediately-after-replay.png')
        time.sleep(1.2)
        later = status()
        row = {'round': i + 1, 'path': new['path'],
               'newSongObservedMs': round((start - switch_start) * 1000, 1),
               'positionWhenReplaySent': at_replay['current_position'],
               'replayHttpMs': elapsed, 'response': response,
               'immediatelyAfter': after, 'later': later}
        report['rounds'].append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
        assert after['path'] == new['path'] == later['path']
        assert after['state'] == later['state'] == 'playing'
        assert after['current_position'] < 1 and later['current_position'] > after['current_position'] + .5
        for key in ('volume', 'audioTrack', 'audioChannel', 'playbackRate'):
            assert after[key] == at_replay[key], (key, at_replay[key], after[key])
    report['passed'] = True
finally:
    (args.output / f'{args.version}-switch-then-replay.json').write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
