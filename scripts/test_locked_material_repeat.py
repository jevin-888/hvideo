"""Exercise the production locked-material EOF branch with deterministic media.

Run using Linux g++; temporary sources and binaries are automatically removed.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/core/Engine_Playlist.cpp').read_text(encoding='utf-8')
start = source.index('      if (commandRouter_ && commandRouter_->isPlaybackLocked(layerId)) {',
                     source.index('void Engine::checkAndPlayNextVideo()'))
end = source.index('\n      std::string playlistId;', start)
branch = source[start:end]
header = r'''
#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
enum class PlaybackSource { AutoPlay };
struct PlaybackResult { bool ok; int code=0; bool isSuccess()const{return ok;} };
struct LayerVideo {
 enum class PlayState { STOPPED, PLAYING, PAUSED };
 bool finished=true; int loop=3; double position=10,duration=10;
 bool isPlaybackFinished()const{return finished;}
 int getLoop()const{return loop;}
 double getCurrentPosition()const{return position;}
 double getDuration()const{return duration;}
};
struct Router { bool global=false,local=false; bool isPlaybackLocked(int)const{return global||local;} };
struct PlaybackRequestDispatcher {
 static inline int calls=0,lastLoop=-1; static inline bool success=true;
 static inline std::string path;
 static PlaybackResult requestPlay(int*,int,const std::string& p,int loop,PlaybackSource){
  ++calls;path=p;lastLoop=loop;return {success};
 }
};
struct Harness {
 using Clock=std::chrono::steady_clock;
 std::unique_ptr<int> mubu_=std::make_unique<int>(1);
 Router router; Router* commandRouter_=&router;
 LayerVideo video; LayerVideo* videoLayer=&video;
 std::unordered_map<int,Clock::time_point> g_autoPlayStoppedSince,g_autoPlayRetryAfter;
 std::unordered_map<int,std::string> g_autoPlayStoppedPath;
 int advanced=0;
 void tick(LayerVideo::PlayState curState,std::string currentPath="current.mp4") {
  const int layerId=1;
  for(int iteration=0;iteration<1;++iteration){
'''
tests = r'''
  ++advanced;
 } }
};
int main(){
 using State=LayerVideo::PlayState;
 for(bool global:{false,true}) {
  Harness h;h.router.global=global;h.router.local=!global;
  PlaybackRequestDispatcher::calls=0;
  // Repeated natural completion repeats the same material without advancing.
  for(int n=0;n<3;++n) h.tick(State::STOPPED);
  assert(PlaybackRequestDispatcher::calls==3 && h.advanced==0);
  assert(PlaybackRequestDispatcher::path=="current.mp4");
  assert(PlaybackRequestDispatcher::lastLoop==3 && h.video.loop==3);
  // Neither explicit stop, pause, startup nor a playing video is restarted.
  h.video.finished=false;h.tick(State::STOPPED);h.tick(State::PAUSED);
  h.tick(State::PLAYING);h.tick(State::STOPPED,"");
  assert(PlaybackRequestDispatcher::calls==3);
  // Missing EOF after an overrun is handled without waiting forever.
  h.video.position=10.3;h.tick(State::PLAYING);
  assert(PlaybackRequestDispatcher::calls==4);
  // Resource failure is throttled instead of retried every frame.
  PlaybackRequestDispatcher::success=false;h.video.finished=true;
  h.tick(State::STOPPED);h.tick(State::STOPPED);
  assert(PlaybackRequestDispatcher::calls==5);
  PlaybackRequestDispatcher::success=true;h.g_autoPlayRetryAfter.clear();
  // Unlock falls through to the original playlist path and original loop mode.
  h.router.global=h.router.local=false;h.tick(State::STOPPED);
  assert(PlaybackRequestDispatcher::calls==5 && h.advanced==1);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='hsvj-locked-material-') as temp:
    test = Path(temp) / 'repeat.cpp'
    binary = Path(temp) / 'repeat'
    test.write_text(header + branch + tests, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: global/layer EOF repeat, unchanged loop, unlock, manual stop/pause, overrun and retry throttle')
