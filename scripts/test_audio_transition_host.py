"""Run production AudioPlayer Android code on Linux with inert AAudio API doubles.

Requires g++ and --ndk-include pointing to an installed NDK sysroot/usr/include.
No hardware output is opened. All unexpected API calls fail the test.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ndk-include', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/audio/AudioPlayer.cpp').read_text(encoding='utf-8')
tests = (root / 'scripts/test_audio_transition.cpp').read_text(encoding='utf-8')
header = (args.ndk_include / 'aaudio/AAudio.h').read_text(encoding='utf-8')
header_without_comments = re.sub(r'/\*.*?\*/', '', header, flags=re.S)
decls = {
    name: (ret, params)
    for ret, name, params in re.findall(
        r'AAUDIO_API\s+([\w\s*]+?)\s+(AAudio\w+)\s*\((.*?)\)\s*__INTRODUCED_IN\(\d+\)\s*;',
        header_without_comments, flags=re.S)
}
used = set(re.findall(r'\b(AAudio\w+)\s*\(', source))
defined = set(re.findall(r'extern "C"\s+\w+\s+(AAudio\w+)\(', tests))
stubs = ['#include <aaudio/AAudio.h>', '#include <cstdio>', '#include <cstdlib>',
         'extern "C" int __android_log_print(int,const char*,const char*,...){return 0;}']
for name in sorted(used - defined):
    ret, params = decls[name]
    stubs.append(f'extern "C" {ret} {name}({params}) {{ '
                 f'std::fprintf(stderr,"Unexpected AAudio call: {name}\\n"); std::abort(); }}')

with tempfile.TemporaryDirectory(prefix='hsvj-audio-host-') as folder:
    temp = Path(folder)
    for relative in ['aaudio/AAudio.h', 'android/log.h']:
        target = temp / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text((args.ndk_include / relative).read_text(encoding='utf-8'), encoding='utf-8')
    (temp / 'stubs.cpp').write_text('\n'.join(stubs), encoding='utf-8')
    binary = temp / 'test'
    subprocess.run(['g++', '-std=c++17', '-pthread', '-D__ANDROID__',
                    '-D__INTRODUCED_IN(x)=', '-I' + str(temp), '-I' + str(root / 'include'),
                    str(root / 'scripts/test_audio_transition.cpp'),
                    str(root / 'src/audio/AudioPlayer.cpp'), str(temp / 'stubs.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
