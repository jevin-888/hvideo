"""Read-only, bounded ADB monitoring of USB capture/decode/display progress."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=3600)
    parser.add_argument('--interval', type=int, default=30)
    args = parser.parse_args()
    if args.seconds < 1 or not 1 <= args.interval <= 60:
        parser.error('Use a positive duration and interval between 1 and 60 seconds')
    args.output.mkdir(parents=True, exist_ok=True)

    def shell(command):
        return subprocess.check_output(['adb', '-s', args.device, 'shell', command],
                                       timeout=20).decode('utf-8', errors='replace')

    def now():
        return dt.datetime.now().astimezone().isoformat(timespec='seconds')

    def config_hash():
        obj = json.loads(shell('cat /huoshan/config/config.json'))
        return hashlib.sha256(json.dumps(obj, sort_keys=True, ensure_ascii=False).encode()).hexdigest()

    def tombstones():
        return shell('ls -l /data/tombstones/tombstone_* 2>/dev/null').splitlines()

    def logs():
        paths = shell('ls -1t /huoshan/logs/*/*_init.log 2>/dev/null | head -3').splitlines()
        found = []
        for path in paths:
            if not re.fullmatch(r'/huoshan/logs/\d{8}/\d{6}_init\.log', path):
                continue
            found.extend(shell('cat ' + path).splitlines())
        return found

    # Seed existing lines so prior versions and pre-monitor faults never count.
    seen = set(logs())
    initial_pid = shell('pidof com.hsvj.engine').strip()
    if not initial_pid.isdecimal():
        raise RuntimeError('Player is not running')
    initial_usb = shell('cat /sys/bus/usb/devices/5-1/devnum').strip()
    initial_tombstones = tombstones()
    before_config = config_hash()
    before_license = shell('sha256sum /huoshan/license/license.dat').split()[0]
    started = time.monotonic()
    started_at = now()
    deadline = started + args.seconds
    totals = {key: 0 for key in ('capture', 'decoded', 'published', 'decodeFailed', 'importFailed')}
    maxima = {key: 0 for key in ('maxInputGapMs', 'maxPublishGapMs', 'maxDecodeUs', 'maxImportUs')}
    windows = 0
    coverage_ms = 0
    anomalies = []
    last_telemetry = started
    samples = 0
    active_log = args.output / 'hour-pipeline.log'
    sample_log = args.output / 'hour-monitor.jsonl'
    state_path = args.output / 'hour-monitor-summary.json'
    with active_log.open('w', encoding='utf-8') as events, sample_log.open('w', encoding='utf-8') as samples_file:
        while True:
            at = now()
            current_anomalies = []
            fresh_rows = []
            try:
                pid = shell('pidof com.hsvj.engine').strip()
                usb = shell('cat /sys/bus/usb/devices/5-1/devnum').strip()
                if pid != initial_pid:
                    current_anomalies.append(dict(kind='process_changed', pid=pid))
                if usb != initial_usb:
                    current_anomalies.append(dict(kind='usb_device_changed', deviceNumber=usb))
                if tombstones() != initial_tombstones:
                    current_anomalies.append(dict(kind='tombstones_changed'))
                for line in logs():
                    if line in seen:
                        continue
                    seen.add(line)
                    if '[UsbPipeline]' in line or '[Capture][USB]' in line or '[RkmppMjpegDecoder]' in line:
                        events.write(line + '\n')
                    if '[UsbPipeline] windowMs=' not in line:
                        continue
                    row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
                    fresh_rows.append(row)
                    windows += 1
                    coverage_ms += row['windowMs']
                    last_telemetry = time.monotonic()
                    for key in totals:
                        totals[key] += row[key]
                    for key in maxima:
                        maxima[key] = max(maxima[key], row[key])
                    # Any quarter-second freeze is an observation failure, not a pass.
                    if row['maxInputGapMs'] >= 250 or row['maxPublishGapMs'] >= 250:
                        current_anomalies.append(dict(kind='frame_gap', sourceLogTime=line[:21], values=row))
                    if row['decodeFailed'] or row['importFailed']:
                        current_anomalies.append(dict(kind='decode_or_import_failure', values=row))
                    if not row['capture'] or not row['decoded'] or not row['published']:
                        current_anomalies.append(dict(kind='pipeline_stage_stopped', values=row))
                if time.monotonic() - last_telemetry > 45:
                    current_anomalies.append(dict(kind='telemetry_missing'))
                sample = dict(at=at, pid=pid, usbDeviceNumber=usb, newWindows=len(fresh_rows),
                              latest=fresh_rows[-1] if fresh_rows else None)
            except Exception as exc:
                current_anomalies.append(dict(kind='monitor_error', message=str(exc)))
                sample = dict(at=at, error=str(exc))
            for anomaly in current_anomalies:
                anomaly['observedAt'] = at
            anomalies.extend(current_anomalies)
            elapsed = time.monotonic() - started
            samples += 1
            sample.update(elapsedSeconds=round(elapsed, 1), anomalies=current_anomalies)
            samples_file.write(json.dumps(sample, ensure_ascii=False) + '\n')
            samples_file.flush()
            events.flush()
            done = elapsed >= args.seconds
            state = dict(status='completed' if done else 'running', startedAt=started_at,
                         updatedAt=now(), expectedFinishAt=(dt.datetime.fromisoformat(started_at) + dt.timedelta(seconds=args.seconds)).isoformat(),
                         durationSeconds=round(elapsed, 1), requiredSeconds=args.seconds,
                         samples=samples, pipelineWindows=windows, telemetryCoverageSeconds=round(coverage_ms / 1000, 3),
                         initialPid=initial_pid, initialUsbDevice=initial_usb, totals=totals, maxima=maxima,
                         anomalyCount=len(anomalies), anomalies=anomalies,
                         beforeConfigSha256=before_config, beforeLicenseSha256=before_license,
                         logStabilityPassed=False)
            if done:
                try:
                    state['afterConfigSha256'] = config_hash()
                    state['afterLicenseSha256'] = shell('sha256sum /huoshan/license/license.dat').split()[0]
                    state['configurationUnchanged'] = state['afterConfigSha256'] == before_config
                    state['licenseUnchanged'] = state['afterLicenseSha256'] == before_license
                    state['logStabilityPassed'] = (not anomalies and coverage_ms / 1000 >= args.seconds * .95
                                                  and state['configurationUnchanged'] and state['licenseUnchanged'])
                except Exception as exc:
                    state['finalVerificationError'] = str(exc)
            temporary = state_path.with_suffix('.tmp')
            temporary.write_text(json.dumps(state, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            temporary.replace(state_path)
            print(json.dumps(dict(at=at, elapsedSeconds=round(elapsed, 1), status=state['status'],
                                  windows=windows, anomalyCount=len(anomalies), maxima=maxima), ensure_ascii=False), flush=True)
            if done:
                break
            time.sleep(min(args.interval, max(0, deadline - time.monotonic())))


if __name__ == '__main__':
    main()
