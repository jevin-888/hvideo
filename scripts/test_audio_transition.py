#!/usr/bin/env python3
"""Compile the real AudioPlayer callback test and run it on an Android device.

No AAudio stream is opened and no audible test signal is emitted.
Usage: python scripts/test_audio_transition.py --device 192.168.2.108:5555
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--device')
    target.add_argument('--compile-only', action='store_true',
                        help='Validate the Android test binary without a connected device')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get('ANDROID_HOME') or
               Path.home() / 'AppData/Local/Android/Sdk')
    compiler = sdk / ('ndk/26.1.10909125/toolchains/llvm/prebuilt/'
                      'windows-x86_64/bin/clang++.exe')
    adb = ['adb', '-s', args.device]
    remote = '/data/local/tmp/hsvj-audio-test-' + uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix='hsvj-audio-test-') as temporary:
        binary = Path(temporary) / 'callback-test'
        subprocess.run([str(compiler), '--target=aarch64-linux-android27',
                        '-std=c++17', '-static-libstdc++', '-I' + str(root / 'include'),
                        str(root / 'scripts/test_audio_transition.cpp'),
                        str(root / 'src/audio/AudioPlayer.cpp'),
                        '-laaudio', '-llog', '-o', str(binary)], check=True)
        if args.compile_only:
            print('PASS: Android arm64 audio regression binary compiled; execution not requested.')
            return
        try:
            subprocess.run(adb + ['push', str(binary), remote], check=True)
            subprocess.run(adb + ['shell', 'chmod', '700', remote], check=True)
            subprocess.run(adb + ['shell', remote], check=True)
        finally:
            subprocess.run(adb + ['shell', 'rm', '-f', remote], check=True)


if __name__ == '__main__':
    main()
