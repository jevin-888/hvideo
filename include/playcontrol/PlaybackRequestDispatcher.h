#ifndef HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_DISPATCHER_H
#define HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_DISPATCHER_H

#include "playcontrol/PlaybackRequest.h"
#include "playcontrol/PlaybackResult.h"
#include <string>

namespace hsvj {

class Mubu;

class PlaybackRequestDispatcher {
public:
  static PlaybackResult requestPlay(Mubu *mubu, int layerId,
                                    const std::string &path, int loop,
                                    PlaybackSource source,
                                    bool userInitiated = false);
  static PlaybackResult stopLayer(Mubu *mubu, int layerId);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_DISPATCHER_H
