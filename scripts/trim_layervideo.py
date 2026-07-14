# Trim LayerVideo.cpp: keep lines 1-131, then comment and namespace 关闭; remove 132-1551
path = r'd:\Hvideo\src\layer\LayerVideo.cpp'
with open(path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# 保留 1-131 行（索引 0-130），然后添加注释和右花括号
kept = lines[:131]
suffix = '''
// update, updateFlashState, render, renderSliceWithEffect, updateCaptureTexture, updateVideoTexture → LayerVideo_Render.cpp
// play, pause, stop, capture, audio, seek, replay, prepare, ... → LayerVideo_Playback.cpp

} // namespace hsvj
'''
with open(path, 'w', encoding='utf-8') as f:
    f.writelines(kept)
    f.write(suffix)
print('Trimmed LayerVideo.cpp')
