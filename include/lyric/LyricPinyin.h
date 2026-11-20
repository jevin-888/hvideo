#ifndef HSVJ_LYRIC_PINYIN_H
#define HSVJ_LYRIC_PINYIN_H

#include <string>

namespace hsvj {

// Converts one ASS event's visible Han text to tone-mark pinyin offline.
// Override blocks, karaoke timings, escapes, drawings and other text are retained.
// Phrase lookup spans override blocks so per-character timings retain polyphony.
std::string convertAssToPinyin(const std::string& text);

} // namespace hsvj

#endif
