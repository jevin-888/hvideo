"""Exercise the production luma detector and warning gate (Linux/WSL + g++)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/capture/V4L2Capture.cpp').read_text(encoding='utf-8')
start = source.index('bool V4L2Capture::detectVerticalDiscontinuity(')
end = source.index('\nbool V4L2Capture::queryCapabilities()', start)
method = source[start:end]
start = source.index('      // Sample mapped luma at low frequency')
end = source.index('      // MIPI 输入只要', start)
gate = source[start:end].replace('std::chrono::steady_clock::now()', 'fakeNow()')
program = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
constexpr uint32_t kFmtNV12=0x3231564E, kFmtNV16=0x3631564E,
 kFmtNV24=0x3432564E, kFmtYUYV=0x56595559, kFmtUYVY=0x59565955;
uint64_t fakeUs=1000000;
auto fakeNow(){return std::chrono::steady_clock::time_point(std::chrono::microseconds(fakeUs));}
int warnings=0;
template<class... T> void warn(T...){++warnings;}
#define LOG_WARN(...) warn(__VA_ARGS__)
#define LOG_INFO(...) ((void)0)
struct V4L2Capture {
 struct Buffer {void *start; size_t length; int dmaBufFd=-1;};
 int actualWidth_=320,actualHeight_=240,actualStride_=352;
 uint32_t actualFormat_=kFmtNV16;
 uint32_t frameIntegritySampleCount_=0;
 int frameIntegrityCandidateRow_=-1,frameIntegrityCandidateStreak_=0;
 uint64_t frameIntegrityLastWarnUs_=0,frameIntegrityCandidateSinceUs_=0;
 uint64_t integritySamples=0,integrityTotalUs=0,integrityMaxUs=0;
 std::vector<Buffer> buffers_;
 bool mipi=true;
 bool shouldSetupMipiPipeline(){return mipi;}
 std::string fourccToString(uint32_t){return "NV16";}
 bool detectVerticalDiscontinuity(const Buffer&,int&,int&)const;
 void tick(bool readable=true,int dataOffset=0){
  bool contentReadable=readable,isMplane=true;int index=0;
  struct {unsigned int data_offset;} planes[1]={{static_cast<unsigned int>(dataOffset)}};
  struct {unsigned int sequence;} buf{42};size_t frameBytesUsed=buffers_[0].length;
''' + gate + r'''
 }
};
''' + method + r'''
int main(){
 V4L2Capture c;
 auto make=[&](int seam,int mode){
  std::vector<uint8_t> data(size_t(c.actualStride_)*c.actualHeight_,128);
  bool packed=c.actualFormat_==kFmtYUYV||c.actualFormat_==kFmtUYVY;
  for(int y=0;y<c.actualHeight_;++y) for(int x=0;x<c.actualWidth_;++x){
   int value=mode==0?70+y*60/c.actualHeight_:((y<seam)?40:210);
   if(mode==2 && x>c.actualWidth_/3) value=70;
   size_t p=size_t(y)*c.actualStride_+x*(packed?2:1)+(c.actualFormat_==kFmtUYVY?1:0);
   data[p]=uint8_t(value);
  }return data;
 };
 int row=0,score=0;
 for(uint32_t fmt:{kFmtNV12,kFmtNV16,kFmtNV24,kFmtYUYV,kFmtUYVY}){
  c.actualFormat_=fmt;c.actualStride_=c.actualWidth_*(fmt==kFmtYUYV||fmt==kFmtUYVY?2:1)+32;
  auto smooth=make(0,0);V4L2Capture::Buffer b{smooth.data(),smooth.size(),-1};
  assert(!c.detectVerticalDiscontinuity(b,row,score));
  // Every seam row, including positions missed by a coarse row grid.
  for(int seam=20;seam<220;++seam){
   auto split=make(seam,1);b={split.data(),split.size(),-1};
   assert(c.detectVerticalDiscontinuity(b,row,score));assert(row==seam);
  }
  auto subtitle=make(119,2);b={subtitle.data(),subtitle.size(),-1};
  assert(!c.detectVerticalDiscontinuity(b,row,score));
  b.length=12;assert(!c.detectVerticalDiscontinuity(b,row,score));
 }
 c.actualFormat_=kFmtNV16;c.actualStride_=352;
 auto split=make(119,1);c.buffers_={{split.data(),split.size(),-1}};
 auto ticks=[&](int n){for(int i=0;i<n;++i){fakeUs+=33334;c.tick();}};
 ticks(44);assert(warnings==0);ticks(1);assert(warnings==1);
 ticks(100);assert(warnings==1); // Ten-second rate limit.
 auto smooth=make(0,0);c.buffers_[0]={smooth.data(),smooth.size(),-1};
 ticks(15);assert(c.frameIntegrityCandidateStreak_==0);
 c.buffers_[0]={split.data(),split.size(),-1};
 c.frameIntegritySampleCount_=0;ticks(15);assert(c.frameIntegrityCandidateStreak_==1);
 c.buffers_[0]={smooth.data(),smooth.size(),-1};ticks(15);
 assert(c.frameIntegrityCandidateStreak_==0); // Isolated cut does not persist.
 c.buffers_[0]={split.data(),split.size(),-1};
 auto oldSamples=c.integritySamples;
 for(int i=0;i<100;++i){c.tick(false);c.tick(true,4);}
 assert(c.integritySamples==oldSamples); // Unreadable/offset buffers are skipped.
 c.mipi=false;ticks(100);assert(c.integritySamples==oldSamples);
 c.mipi=true;c.actualFormat_=0;assert(!c.detectVerticalDiscontinuity(c.buffers_[0],row,score));
 // Benchmark only the production detector; this is host CPU, not H66 proof.
 c.actualFormat_=kFmtNV16;c.actualWidth_=3840;c.actualHeight_=2160;c.actualStride_=3840;
 auto large=make(1081,1);V4L2Capture::Buffer b{large.data(),large.size(),-1};
 auto begin=std::chrono::steady_clock::now();
 for(int i=0;i<1000;++i){assert(c.detectVerticalDiscontinuity(b,row,score));}
 auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
 std::cout<<"PASS: all supported luma layouts, arbitrary seams, smooth frames, partial-width edges, "
             "bounds, consecutive confirmation, rate limit, invalid DMA reads; 4K host avgUs="<<us/1000<<"\n";
}
'''
with tempfile.TemporaryDirectory(prefix='capture-integrity-') as folder:
    cpp = Path(folder) / 'test.cpp'
    binary = Path(folder) / 'test'
    cpp.write_text(program, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', str(cpp),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
