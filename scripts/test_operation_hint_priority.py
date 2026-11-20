"""Run the production hint-state methods without Android/Vulkan (Linux g++)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/text/MessageHintRenderer.cpp').read_text(encoding='utf-8')


def method(name):
    start = source.index('MessageHintRenderer::' + name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


header = r'''
#include <string>
#include <mutex>
#include <chrono>
#include <cassert>
#define LOG_DEBUG(...) ((void)0)
enum class HintType { NONE, PLAYLIST, PLAY, PAUSE, NEXT, PREV, VOLUME_UP, VOLUME_DOWN, MUTE, UNMUTE, AUDIO_TRACK, BACKING_TRACK, CUSTOM };
struct HintItem { HintType type=HintType::NONE; std::string text; bool isPermanent=false; float displayDuration=1.5f; std::chrono::steady_clock::time_point startTime; };
struct MessageHintRenderer {
 std::mutex hintMutex_; HintItem currentOperationHint_;
 bool protectedOperationHint_=false, protectedHintInterruptible_=false, operationHintActive_=false, playlistHintVisible_=false, dirty_=false;
 bool playbackOperationHintEnabled_=true;
 float operationHintDuration_=1.5f; int currentTextureType_=0;
 void showOperationHint(HintType,const std::string& = "");
 void setPlaybackOperationHintEnabled(bool);
 bool showProtectedOperationHint(HintType,const std::string&,bool=false,bool=false);
 bool clearProtectedOperationHint();
 void clearOperationHint();
 bool shouldHidePermanentHint(HintType) const;
};
'''
body = '\n'.join(method(name) for name in [
    'setPlaybackOperationHintEnabled', 'showOperationHint', 'showProtectedOperationHint',
    'clearProtectedOperationHint', 'clearOperationHint', 'shouldHidePermanentHint'])
tests = r'''
int main() {
 for (auto type : {HintType::VOLUME_UP,HintType::VOLUME_DOWN,HintType::PAUSE,HintType::PLAY,HintType::NEXT,HintType::PREV,HintType::MUTE,HintType::UNMUTE,HintType::AUDIO_TRACK,HintType::BACKING_TRACK}) {
  MessageHintRenderer r;
  assert(r.showProtectedOperationHint(HintType::CUSTOM,"ready",true));
  assert(r.showProtectedOperationHint(HintType::CUSTOM,"ready refreshed",true,true));
  r.showOperationHint(type,"control");
  assert(r.operationHintActive_ && !r.protectedOperationHint_);
  assert(r.currentOperationHint_.type==type);
  assert(!r.showProtectedOperationHint(HintType::CUSTOM,"stale refresh",true,true));
  assert(!r.clearProtectedOperationHint());
  assert(r.operationHintActive_ && r.currentOperationHint_.type==type);
  r.operationHintActive_=false;
  assert(!r.showProtectedOperationHint(HintType::CUSTOM,"late refresh",true,true));
  r.showOperationHint(type,"visible");
  r.currentTextureType_=1;
  r.setPlaybackOperationHintEnabled(false);
  assert(!r.operationHintActive_ && r.currentTextureType_==0);
  r.playlistHintVisible_=true;
  r.showOperationHint(type,"hidden");
  assert(!r.operationHintActive_ && r.playlistHintVisible_);
  r.setPlaybackOperationHintEnabled(true);
  assert(!r.operationHintActive_); // enabling does not replay a stale pause/mute
  r.showOperationHint(type,"enabled again");
  assert(r.operationHintActive_ && r.currentOperationHint_.text=="enabled again");
 }
 MessageHintRenderer system;
 system.showOperationHint(HintType::CUSTOM,"network disconnected");
 system.setPlaybackOperationHintEnabled(false);
 assert(system.operationHintActive_ && system.currentOperationHint_.text=="network disconnected");
 system.showOperationHint(HintType::PLAY);
 assert(system.operationHintActive_ && system.currentOperationHint_.text=="network disconnected");
 system.showOperationHint(HintType::CUSTOM,"network restored");
 assert(system.currentOperationHint_.text=="network restored");
 MessageHintRenderer r;
 assert(r.showProtectedOperationHint(HintType::CUSTOM,"license required"));
 r.setPlaybackOperationHintEnabled(false);
 r.showOperationHint(HintType::VOLUME_UP);
 assert(r.protectedOperationHint_ && r.currentOperationHint_.text=="license required");
 assert(r.clearProtectedOperationHint());
 assert(!r.operationHintActive_);
 assert(!r.clearProtectedOperationHint());
 assert(r.showProtectedOperationHint(HintType::CUSTOM,"ready",true));
 r.showOperationHint(HintType::PLAY);
 assert(r.protectedOperationHint_ && r.currentOperationHint_.text=="ready");
 r.setPlaybackOperationHintEnabled(true);
 assert(r.showProtectedOperationHint(HintType::CUSTOM,"expired",false,true));
 r.showOperationHint(HintType::PLAY);
 assert(r.currentOperationHint_.text=="expired");
}
'''
with tempfile.TemporaryDirectory(prefix='hplayer-hint-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(header + body + tests, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-pthread', str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: 10 controls, enable/disable, stale pause/mute cleanup, playlist/system hints, protected hints')
