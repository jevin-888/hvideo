# Split Lyric渲染器.cpp into _Ass, _Render and trim main
import os
base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(base, 'src', 'lyric', 'LyricRenderer.cpp')
ass_path = os.path.join(base, 'src', 'lyric', 'LyricRenderer_Ass.cpp')
render_path = os.path.join(base, 'src', 'lyric', 'LyricRenderer_Render.cpp')

with open(src, 'r', encoding='utf-8') as f:
    lines = f.readlines()

header_common = r'''/**
 * @file LyricRenderer_XXX.cpp
 * @brief LyricRenderer split module
 */

#include "lyric/LyricRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>
extern "C" {
#include <ass.h>
}

namespace hsvj {

'''

# ASS 分拆：adjustSubtitleTimingAndTracks(76-430)，getSubtitleCount/setASSStyle/getASSStyle/getASSStyleNames/getFirstLyricStartTime(1161-1377)
ass_header = header_common.replace('LyricRenderer_XXX', 'LyricRenderer_Ass')
ass_content = ''.join(lines[75:430]) + '\n' + ''.join(lines[1160:1377])
with open(ass_path, 'w', encoding='utf-8') as f:
    f.write(ass_header + ass_content + '\n} // namespace hsvj\n')
print('Written LyricRenderer_Ass.cpp')

# 渲染: helpers (27-75), 渲染/prepareTexture/renderASSImages (653-1160), prepareCountdownDots renderCountdownDots (1381-1486)
render_header = header_common.replace('LyricRenderer_XXX', 'LyricRenderer_Render')
render_content = ''.join(lines[26:75]) + '\n' + ''.join(lines[652:1160]) + '\n' + ''.join(lines[1380:1486])
with open(render_path, 'w', encoding='utf-8') as f:
    f.write(render_header + render_content + '\n} // namespace hsvj\n')
print('Written LyricRenderer_Render.cpp')

# Main: keep 1-26 (header + namespace), 431-652 (ctor, dtor, setRenderSize..unload), then comment and 关闭
kept = lines[:26] + lines[430:652]
suffix = '''
// adjustSubtitleTimingAndTracks, setASSStyle, getASSStyle, getASSStyleNames, getSubtitleCount, getFirstLyricStartTime -> LyricRenderer_Ass.cpp
// isValidImage*, extractColor, render, prepareTexture, renderASSImages, prepareCountdownDots, renderCountdownDots -> LyricRenderer_Render.cpp

} // namespace hsvj
'''
with open(src, 'w', encoding='utf-8') as f:
    f.writelines(kept)
    f.write(suffix)
print('Trimmed LyricRenderer.cpp')
