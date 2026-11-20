"""Verify the actual Android file-hash branch with bundled FFmpeg MD5 (WSL)."""
from pathlib import Path
import hashlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/utils/FileUtils.cpp').read_text(encoding='utf-8')
function = source[source.index('std::string FileUtils::calculateMD5('):]
function = function[:function.index('#elif !defined(HAVE_OPENSSL_CRYPTO)')].replace('#ifdef __ANDROID__', '') + '}\n'
harness = r'''
#include "utils/FileUtils.h"
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <iostream>
extern "C" {
#include <libavutil/md5.h>
#include <libavutil/mem.h>
void* av_mallocz(size_t size) { return calloc(1,size); }
void av_free(void* ptr) { free(ptr); }
}
namespace hsvj {
''' + function + r'''
}
int main(int argc,char** argv) { for(int i=1;i<argc;++i) std::cout<<hsvj::FileUtils::calculateMD5(argv[i])<<"\n"; }
'''
with tempfile.TemporaryDirectory(prefix='hplayer-real-md5-') as directory:
    temp = Path(directory)
    (temp / 'main.cpp').write_text(harness, encoding='utf-8')
    ffmpeg = ROOT / 'vendor/ffmpeg-rockchip'
    subprocess.run(['gcc', '-O2', '-c', '-DHAVE_FAST_UNALIGNED=0', '-I'+str(ffmpeg), str(ffmpeg / 'libavutil/md5.c'), '-o', str(temp / 'md5.o')], check=True)
    subprocess.run(['g++', '-std=c++17', '-O2', '-I'+str(ROOT/'include'), '-I'+str(ffmpeg),
                    str(temp/'main.cpp'), str(temp/'md5.o'), '-o', str(temp/'test')], check=True)
    vectors = [b'', b'a', b'abc', b'message digest', b'a'*65535, b'b'*65536, b'c'*65537, bytes(range(256))*10000]
    paths = []
    for number, data in enumerate(vectors):
        path = temp / f'{number}.bin'
        path.write_bytes(data)
        paths.append(str(path))
    actual = subprocess.check_output([str(temp/'test'), *paths, str(temp/'missing')], text=True).splitlines()
    assert actual == [hashlib.md5(data).hexdigest() for data in vectors] + ['']
    print('PASS actual Android file MD5 + bundled FFmpeg: RFC vectors, empty, 64KB boundaries, binary streaming, missing file')
