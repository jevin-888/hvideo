"""Compile production replay/controller code with deterministic media doubles (Linux g++)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/layer/LayerVideo_Playback.cpp').read_text(encoding='utf-8')
start = source.index('bool LayerVideo::tryReplayCurrent(')
end = source.index('\ndouble LayerVideo::getCurrentPosition()', start)
replay = source[start:end]

stub = r'''
#pragma once
#include "playcontrol/LayerPlaybackController.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
namespace hsvj {
enum class DecodeErrorCode { None, UnsupportedFormat, DecoderUnavailable, OpenCodecFailed, DecodeFailed, OpenFileFailed, FindStreamFailed, ResourceError };
struct License { bool blocked=false; bool shouldBlockVideoPlayback(){return blocked;} };
struct Decoder { int loop=0; void setLoop(int n){loop=n;} };
struct FileUtils { static std::string normalizePath(const std::string& p){return p;} };
inline long long elapsedMillisSince(std::chrono::steady_clock::time_point t) {
 return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t).count();
}
class LayerVideo {
public:
 enum class PlayState { STOPPED, PLAYING, PAUSED };
 std::mutex lifecycleOpMutex_; std::timed_mutex mutex_;
 License* licenseManager_=nullptr; Decoder decoderStorage; Decoder* decoder_=&decoderStorage;
 std::atomic<bool> isShuttingDown_{false}, isPlayingPureAudio_{false};
 std::string currentPath_="http://song"; PlayState state_=PlayState::PLAYING;
 bool syncPending=false, seekOK=true, playOK=true; int layerId_=1,loop_=0, seeks=0,opens=0;
 float volume=.75f; int audioTrack=1; double position=20;
 std::mutex openMutex; std::condition_variable openCv; bool blockOpen=false, entered=false, released=false;
 bool isSyncPresentationPending()const{return syncPending;}
 bool tryReplayCurrent(const std::string&,int);
 bool seekWithLifecycleLock(double p,const std::string&,std::chrono::steady_clock::time_point){++seeks;if(seekOK)position=p;return seekOK;}
 void resume(){state_=PlayState::PLAYING;}
 bool play(const std::string& p,int loop,double){
  ++opens; std::unique_lock<std::mutex> l(openMutex);entered=true;openCv.notify_all();
  if(blockOpen)openCv.wait(l,[&]{return released;});
  currentPath_=p;loop_=loop;state_=PlayState::PLAYING;return playOK;
 }
 bool isPlayingPureAudio()const{return isPlayingPureAudio_.load();}
 int getVideoWidth()const{return 1920;} int getVideoHeight()const{return 1080;}
 static size_t getPendingDecoderReleaseTaskCount(){return 0;}
 DecodeErrorCode getLastPlaybackErrorCode()const{return DecodeErrorCode::None;}
 std::string getLastPlaybackErrorMessage()const{return "";}
 void stop(){state_=PlayState::STOPPED;}
};
struct DecoderAdmissionInfo {
 std::string path,codecName; bool known=false,hasVideo=true;int width=0,height=0;double frameRate=0;int64_t bitRate=0;
 bool isFourK()const{return known&&hasVideo&&int64_t(width)*height>=7500000;}
};
struct DecoderResourceGate {
 int opens=0;bool allowed=true;
 static DecoderResourceGate& getInstance(){static DecoderResourceGate r;return r;}
 bool requiresAdmissionProbe(int)const{return false;}
 bool tryBeginOpen(int,const DecoderAdmissionInfo&,std::string*){++opens;return allowed;}
 void onDecoderOpenFinished(int,const DecoderAdmissionInfo&,bool){}
 void onLayerStopped(int){}
};
struct MediaVideoInfo {bool valid=false,hasVideo=true;int width=0,height=0;double frameRate=0;int64_t bitRate=0;std::string codecName;};
struct MediaUtils {static bool probeVideoInfo(const std::string&,MediaVideoInfo&,std::string*){return false;}};
struct LocalVodPlayer {static void suppressIdlePlaybackForMs(int){}};
struct MediaRuntimeDiagnostics {
 std::atomic<uint64_t> id{0};
 static MediaRuntimeDiagnostics& getInstance(){static MediaRuntimeDiagnostics d;return d;}
 uint64_t nextRequestId(){return ++id;}uint64_t nextSwitchId(){return ++id;}
 template<class... T> void logPlaybackTrace(T...){}
 template<class... T> void logPlaybackResult(T...){}
};
inline const char* toString(PlaybackResultCode){return "result";}
}
'''
tests = r'''
using namespace hsvj;
int main(){
 // The production layer method preserves controls and restarts paused playback.
 LayerVideo l;
 assert(l.tryReplayCurrent("http://song",3));
 assert(l.seeks==1 && l.opens==0 && l.position==0 && l.loop_==3 && l.decoderStorage.loop==3);
 assert(l.volume==.75f && l.audioTrack==1);
 l.state_=LayerVideo::PlayState::PAUSED;l.position=12;
 assert(l.tryReplayCurrent("http://song",1) && l.state_==LayerVideo::PlayState::PLAYING && l.position==0);
 int n=l.seeks;
 assert(!l.tryReplayCurrent("http://other",1));
 assert(!l.tryReplayCurrent("",1));
 l.state_=LayerVideo::PlayState::STOPPED;assert(!l.tryReplayCurrent("http://song",1));l.state_=LayerVideo::PlayState::PLAYING;
 l.isPlayingPureAudio_=true;assert(!l.tryReplayCurrent("http://song",1));l.isPlayingPureAudio_=false;
 l.isShuttingDown_=true;assert(!l.tryReplayCurrent("http://song",1));l.isShuttingDown_=false;
 l.syncPending=true;assert(!l.tryReplayCurrent("http://song",1));l.syncPending=false;
 License lic;lic.blocked=true;l.licenseManager_=&lic;assert(!l.tryReplayCurrent("http://song",1));l.licenseManager_=nullptr;
 l.decoder_=nullptr;assert(!l.tryReplayCurrent("http://song",1));l.decoder_=&l.decoderStorage;
 assert(l.seeks==n);
 l.seekOK=false;assert(!l.tryReplayCurrent("http://song",1));l.seekOK=true;

 // Execute the real controller, including resource admission and pending queue.
 PlaybackRequest r;r.layerId=1;r.path="http://song";r.loop=3;r.restartFromBeginning=true;
 auto& gate=DecoderResourceGate::getInstance();
 LayerPlaybackController c(1);LayerVideo v;
 assert(c.requestPlay(r,&v).code==PlaybackResultCode::Started);
 assert(v.seeks==1 && v.opens==0 && gate.opens==0);
 assert(c.getState()==LayerPlaybackState::Playing && c.getCurrentPath()==r.path);
 v.seekOK=false;assert(c.requestPlay(r,&v).isSuccess());assert(v.opens==1 && gate.opens==1);
 gate.allowed=false;assert(c.requestPlay(r,&v).softBlocked);assert(v.opens==1);gate.allowed=true;
 v.seekOK=true;r.restartFromBeginning=false;assert(c.requestPlay(r,&v).isSuccess());assert(v.opens==2);
 r.restartFromBeginning=true;r.syncStartTime=123;assert(c.requestPlay(r,&v).isSuccess());assert(v.opens==3);r.syncStartTime=0;
 r.path="http://other";assert(c.requestPlay(r,&v).isSuccess());assert(v.opens==4);

 // A replay received while initial open is running queues, then seeks once.
 LayerVideo queued;queued.blockOpen=true;
 {
  LayerPlaybackController qc(1);
  PlaybackRequest initial=r;initial.path="http://new";initial.restartFromBeginning=false;
  std::thread first([&]{assert(qc.requestPlay(initial,&queued).isSuccess());});
  {std::unique_lock<std::mutex> lock(queued.openMutex);queued.openCv.wait(lock,[&]{return queued.entered;});}
  auto again=initial;again.restartFromBeginning=true;
  assert(qc.requestPlay(again,&queued).code==PlaybackResultCode::Accepted);
  {std::lock_guard<std::mutex> lock(queued.openMutex);queued.released=true;}queued.openCv.notify_all();
  first.join();
 } // Joins pending worker.
 assert(queued.opens==1 && queued.seeks==1 && queued.position==0);
}
'''

with tempfile.TemporaryDirectory(prefix='hsvj-replay-') as tmp:
    tmp = Path(tmp)
    (tmp / 'stub.h').write_text(stub)
    for name in ['playcontrol/PlaybackCoordinator.h', 'playcontrol/DecoderResourceGate.h',
                 'playcontrol/MediaRuntimeDiagnostics.h', 'layer/LayerVideo.h',
                 'utils/MediaUtils.h', 'utils/Logger.h', 'vod/LocalVodPlayer.h']:
        p = tmp / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('#include "stub.h"\n')
    (tmp / 'test.cpp').write_text('#include "stub.h"\nnamespace hsvj {\n' + replay + '\n}\n' + tests)
    subprocess.run(['g++', '-std=c++17', '-pthread', '-I'+str(tmp), '-I'+str(root/'include'),
                    str(tmp/'test.cpp'), str(root/'src/playcontrol/LayerPlaybackController.cpp'),
                    '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True, timeout=15)
print('PASS: replay reuse, paused restart, eligibility, fallback, resource gate, and replay queued during initial open')
