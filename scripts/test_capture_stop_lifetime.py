"""Exercise production capture stop against a blocked driver (WSL/Linux + g++)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/capture/V4L2Capture.cpp').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


methods = '\n'.join(function(signature) for signature in (
    'void V4L2Capture::stopCapture(',
    'bool V4L2Capture::stopCaptureAsync(',
    'void V4L2Capture::finishStopCapture(',
))
program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <linux/videodev2.h>
template<class... T> void logStub(T...) {}
#define LOG_DEBUG(...) logStub(__VA_ARGS__)
#define LOG_WARN(...) logStub(__VA_ARGS__)
using namespace std::chrono_literals;
struct V4L2Capture {
  std::mutex stopMutex_,mutex_;
  std::future<void> stopTask_;
  std::thread captureThread_;
  std::atomic<bool> shouldStop_{false},isUnhealthy_{false},isCapturing_{true},hasSignal_{true};
  bool streamOn_=true;
  int fd_=7,bufferType_=1;
  std::function<void()> frameCallback_=[]{ };
  std::promise<void> driverEntered, releaseDriver;
  std::shared_future<void> released=releaseDriver.get_future().share();
  std::atomic<int> streamOffCalls{0};
  int v4l2Ioctl(int,unsigned long op,void*) {
    assert(op==VIDIOC_STREAMOFF);
    assert(!isCapturing_ && !hasSignal_);
    ++streamOffCalls;driverEntered.set_value();released.wait();return 0;
  }
  void stopCapture(); bool stopCaptureAsync(); void finishStopCapture();
};
''' + methods + r'''
int main(){
  V4L2Capture c;
  std::atomic<int> callbacks{0};
  c.captureThread_=std::thread([&]{while(!c.shouldStop_){++callbacks;std::this_thread::yield();}});
  assert(!c.stopCaptureAsync());
  assert(c.driverEntered.get_future().wait_for(2s)==std::future_status::ready);
  int joinedCount=callbacks;
  // A driver held indefinitely must not block the tick polling stop completion.
  auto renderTick=std::async(std::launch::async,[&]{
    for(int i=0;i<100;++i) assert(!c.stopCaptureAsync());
    assert(c.mutex_.try_lock());c.mutex_.unlock();
  });
  assert(renderTick.wait_for(200ms)==std::future_status::ready);renderTick.get();
  assert(callbacks==joinedCount && c.streamOffCalls==1);
  // Destruction/shutdown must join the worker before fd/buffers can be released.
  auto shutdown=std::async(std::launch::async,[&]{c.stopCapture();});
  assert(shutdown.wait_for(25ms)==std::future_status::timeout);
  c.releaseDriver.set_value();shutdown.get();
  assert(!c.frameCallback_ && !c.streamOn_);
  c.stopCapture();c.stopCapture();
  assert(c.streamOffCalls==1 && !c.captureThread_.joinable());
  V4L2Capture done;assert(!done.stopCaptureAsync());
  assert(done.driverEntered.get_future().wait_for(2s)==std::future_status::ready);
  done.releaseDriver.set_value();
  auto deadline=std::chrono::steady_clock::now()+2s;
  while(!done.stopCaptureAsync()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
  done.stopCapture();assert(done.streamOffCalls==1);
}
'''
with tempfile.TemporaryDirectory(prefix='capture-stop-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(program, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: blocked STREAMOFF, nonblocking render ticks, callback join, shutdown ownership, idempotent stop')
