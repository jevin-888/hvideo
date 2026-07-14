# 从 LayerVideo.cpp 生成 LayerVideo_Playback.cpp
import os
base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(base, 'src', 'layer', 'LayerVideo.cpp')
dst = os.path.join(base, 'src', 'layer', 'LayerVideo_Playback.cpp')

with open(src, 'r', encoding='utf-8') as f:
    lines = f.readlines()

part1 = lines[52:75]
part2 = lines[478:1567]

header = r'''/**
 * @file LayerVideo_Playback.cpp
 * @brief 视频图层：播放/采集/音频/资源控制
 */

#include "layer/LayerVideo.h"
#include "capture/V4L2Capture.h"
#include "capture/V4L2SubdevQuery.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "decoder/VideoDecoder.h"
#include "decoder/VideoDecoderPool.h"
#include "effect/EffectManager.h"
#include "renderer/CaptureRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "utils/V4L2DeviceDetector.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/pixfmt.h>
}
#endif
#include "utils/SystemUtils.h"

namespace hsvj {

'''

out = header + ''.join(part1) + '\n' + ''.join(part2) + '\n} // namespace hsvj\n'
with open(dst, 'w', encoding='utf-8') as f:
    f.write(out)
print('Written', dst)
