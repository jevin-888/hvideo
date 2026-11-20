"""Execute production EOF commit and clock methods with deterministic thread doubles."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
core = (root / 'src/decoder/core/DecoderCore.cpp').read_text(encoding='utf-8')
layer = (root / 'src/layer/LayerVideo_Playback.cpp').read_text(encoding='utf-8')


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


stub = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
using namespace std::chrono_literals;
enum class DecodeErrorCode { None };
struct HookedTime {
 double value=0; mutable std::atomic<bool> hook{false};
 mutable std::promise<void> entered; std::shared_future<void> release;
 operator double() const {
   const double snapshot=value;
   if(hook.exchange(false)){entered.set_value();release.wait();}
   return snapshot;
 }
 void operator=(double v){value=v;}
 void operator-=(double v){value-=v;}
};
struct DecoderCore {
 struct FrameSync {void enterCatchupMode(){}};
 FrameSync* frameSyncManager_=nullptr;
 std::condition_variable videoPacketQueueCv_,audioPacketQueueCv_,frameQueueCv_;
 bool explicitSyncStart_=false,firstFrameAligned_=false;
 int severeDriftStreak_=0,layerId_=1;
 std::mutex eofDrainMutex_;
 mutable std::recursive_mutex playbackClockMutex_;
 std::atomic<bool> isSeeking_{false},isPaused_{false},shouldStop_{false},
                   demuxInputEnded_{false},isFinished_{false};
 std::atomic<double> pausedPosition_{0};
 double duration_=20.833,pausedTime_=0,pauseStartTime_=0;
 HookedTime startTime_; float playbackRate_=1; int loop_=3;
 DecodeErrorCode lastErrorCode_=DecodeErrorCode::None;
 mutable std::atomic<double> continuousWallClockOffset_{0},lastReportedPlayTime_{0};
 std::atomic<bool> audioMasterClockValid_{false};
 std::atomic<double> audioMasterClockPts_{0},audioMasterClockSystemTime_{0},
                     audioMasterBufferedEndPts_{0},audioClockHandoffOffset_{0};
 std::atomic<uint64_t> audioMasterClockGeneration_{0},audioClockHandoffGeneration_{0};
 std::atomic<int64_t> lastAudioMasterClockLogMs_{0};
 bool suppressed=false;
 void signalStop(){shouldStop_=true;}
 void setAudioOutputSuppressed(bool v){suppressed=v;}
 bool finishPlaybackIfAtEnd(double);
 double getCurrentPlayTime() const;
 void resetAudioMasterClock(double);
 void pauseDecoding();
 void resumeDecoding();
 bool setSyncStartTime(double);
 void setPlaybackRate(float);
 void realignClock(double,bool);
 void reset(double position){
   std::lock_guard<std::recursive_mutex> lock(playbackClockMutex_);
   startTime_=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()-position;
   pausedTime_=0;continuousWallClockOffset_=0;resetAudioMasterClock(position);
 }
};
struct AudioPlayer {void flush(){}};
struct AudioPlayerManager {
 static AudioPlayerManager& getInstance(){static AudioPlayerManager m;return m;}
 bool hasAudioFocusForLayer(int)const{return false;}
 AudioPlayer* getAudioPlayer(){return nullptr;}
};
struct LayerVideo {
 enum class PlayState {STOPPED,PLAYING,PAUSED};
 std::mutex lifecycleOpMutex_; std::timed_mutex mutex_;
 std::atomic<uint64_t> playbackGeneration_{1};
 PlayState state_=PlayState::PLAYING;std::string currentPath_="clip";
 DecoderCore* decoder_=nullptr;int layerId_=1,noFrameStuckCount_=0;
 double noFrameStuckStartTime_=0;
 int64_t duplicateFrameStuckStartMs_=0,duplicateFrameStuckFrameNumber_=-1;
 bool markPlaybackFinishedRetainingPath(const std::string&,uint64_t,double);
};
'''

tests = r'''
int main(){
 DecoderCore d; d.reset(21.212);
 d.loop_=2;
 d.demuxInputEnded_=true;assert(!d.finishPlaybackIfAtEnd(.25));d.demuxInputEnded_=false;
 d.isSeeking_=true;assert(!d.finishPlaybackIfAtEnd(.25));d.isSeeking_=false;
 d.isPaused_=true;assert(!d.finishPlaybackIfAtEnd(.25));d.isPaused_=false;
 // A competing seek owns the shared transition lock; the update must not wait.
 {
  std::unique_lock<std::mutex> lock(d.eofDrainMutex_);
  auto commit=std::async(std::launch::async,[&]{return d.finishPlaybackIfAtEnd(.25);});
  assert(commit.wait_for(1s)==std::future_status::ready && !commit.get());
 }
 // Previously sampled 21.212 is obsolete after the internal seek returns to 0.
 d.reset(0);assert(!d.finishPlaybackIfAtEnd(.25));assert(!d.shouldStop_);
 LayerVideo l;l.decoder_=&d;
 d.reset(21.212);
 assert(!l.markPlaybackFinishedRetainingPath("stale",0,.25));assert(!d.shouldStop_);
 {
  std::unique_lock<std::mutex> lock(l.lifecycleOpMutex_);
  auto commit=std::async(std::launch::async,[&]{return l.markPlaybackFinishedRetainingPath("opening",1,.25);});
  assert(commit.wait_for(1s)==std::future_status::ready && !commit.get());
 }
 l.state_=LayerVideo::PlayState::PAUSED;
 assert(!l.markPlaybackFinishedRetainingPath("paused",1,.25));
 l.state_=LayerVideo::PlayState::PLAYING;
 assert(l.markPlaybackFinishedRetainingPath("real end",1,.25));
 assert(d.shouldStop_ && d.isFinished_ && d.suppressed);
 assert(l.state_==LayerVideo::PlayState::STOPPED && l.playbackGeneration_==2);
 assert(d.getCurrentPlayTime()==d.duration_); // No phantom 20-second STOPPED clock.

 // Pause a real clock reader after sampling the old start time. Reset must wait
 // until it publishes; otherwise that old candidate repopulates the new clock.
 DecoderCore clock;clock.reset(21.212);
 std::promise<void> release;clock.startTime_.release=release.get_future().share();
 auto entered=clock.startTime_.entered.get_future();clock.startTime_.hook=true;
 auto oldReader=std::async(std::launch::async,[&]{return clock.getCurrentPlayTime();});
 assert(entered.wait_for(1s)==std::future_status::ready);
 std::promise<void> resetStarted;auto resetEntry=resetStarted.get_future();
 auto reset=std::async(std::launch::async,[&]{resetStarted.set_value();clock.reset(0);});
 resetEntry.wait();assert(reset.wait_for(50ms)==std::future_status::timeout);
 release.set_value();assert(oldReader.get()>21);reset.get();
 assert(clock.getCurrentPlayTime()<1);

 // Exercise production clock writers against continuous concurrent readers.
 DecoderCore transitions;transitions.reset(5);
 transitions.pauseDecoding();
 const double frozen=transitions.getCurrentPlayTime();
 std::this_thread::sleep_for(20ms);
 assert(transitions.getCurrentPlayTime()==frozen);
 transitions.resumeDecoding();
 assert(transitions.getCurrentPlayTime()>=frozen && transitions.getCurrentPlayTime()<6);
 transitions.setPlaybackRate(2);
 assert(transitions.getCurrentPlayTime()<6);
 std::atomic<bool> done{false};
 auto reader=std::async(std::launch::async,[&]{while(!done){transitions.getCurrentPlayTime();}});
 for(int i=0;i<1000;++i){
   transitions.realignClock(20,false);
   transitions.realignClock(0,false);
   assert(transitions.getCurrentPlayTime()<1);
   transitions.pauseDecoding();transitions.resumeDecoding();
 }
 done=true;reader.get();
 assert(transitions.setSyncStartTime(std::chrono::duration<double>(
     std::chrono::steady_clock::now().time_since_epoch()).count()+1));
 assert(transitions.getCurrentPlayTime()==0);
}
'''

production = '\n'.join([
    method(core, 'bool DecoderCore::finishPlaybackIfAtEnd('),
    method(core, 'double DecoderCore::getCurrentPlayTime() const'),
    method(core, 'void DecoderCore::resetAudioMasterClock('),
    method(core, 'void DecoderCore::pauseDecoding('),
    method(core, 'void DecoderCore::resumeDecoding('),
    method(core, 'bool DecoderCore::setSyncStartTime('),
    method(core, 'void DecoderCore::setPlaybackRate('),
    method(core, 'void DecoderCore::realignClock('),
    method(layer, 'bool LayerVideo::markPlaybackFinishedRetainingPath('),
])
with tempfile.TemporaryDirectory(prefix='hsvj-end-race-') as folder:
    source = Path(folder) / 'test.cpp'
    binary = Path(folder) / 'test'
    source.write_text(stub + production + tests, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-pthread', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
print('PASS: real EOF commit, drain/seek/pause guards, stale generation, nonblocking transitions, genuine stalled-loop fallback, natural EOF publication, concurrent clock reset, pause/resume/rate/sync and 1000 concurrent realign cycles')
