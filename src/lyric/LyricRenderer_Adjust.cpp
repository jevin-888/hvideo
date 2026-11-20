#include "lyric/LyricRenderer.h"

#include "lyric/ASSRenderer.h"
#include "lyric/LyricPinyin.h"
#include "lyric/LyricCountdown.h"
#include "lyric/AssOverrideTags.h"
#include "lyric/LyricVocalColor.h"

#include "utils/Logger.h"



#include <algorithm>

#include <array>

#include <cstring>

#include <cctype>

#include <string>

#include <vector>



extern "C" {

#include <ass.h>

}



namespace hsvj {



namespace {



constexpr double kLyricFontSize = 150.0;

constexpr double kLyricCharacterSpacing = 0.0;

constexpr int32_t kLyricSecondaryColour = 0xFFFFFF00;

constexpr int32_t kLyricUnsungOutlineColour = 0x00000000;

constexpr int32_t kLyricSungOutlineColour = 0xFFFFFF00;


constexpr double kLyricBackgroundPadding = 12.0;



enum class HeaderCategory {

  kSongName = 0,

  kSinger = 1,

  kProducer = 2,

  kUnknown = 3,

};



struct LyricEventSnapshot {

  int eventIndex = -1;

  int64_t start = 0;

  int64_t duration = 0;

  int readOrder = 0;

  int layer = 0;

  int style = 0;

  int marginL = 0;

  int marginR = 0;

  int marginV = 0;

  std::string text;

};



bool containsKaraokeTag(const char *text) {
  if (!text) return false;
  bool found = false;
  stripAssTags(text, AssTagGroup::Karaoke, &found);
  return found;
}

void replaceOwnedString(char **dest, const std::string &value) {

  if (!dest) {

    return;

  }

  if (*dest) {

    free(*dest);

    *dest = nullptr;

  }



  char *copied = ::strdup(value.c_str());

  if (!copied) {

    LOG_ERROR("[Lyric] strdup failed length=%zu", value.size());

    return;

  }

  *dest = copied;

}



std::string stripPositioningTags(const std::string &text) {
  return stripAssTags(text, AssTagGroup::Position);
}

std::string stripKaraokeTags(const std::string &text) {
  return stripAssTags(text, AssTagGroup::Karaoke);
}

// Countdown dots are ASS vector events so every output uses the same atlas,
// composite texture and timeline (including pause, seek, slices and device reset).
int appendCountdownEvents(ASS_Track *track,
                          const std::vector<LyricEventSnapshot> &lyrics,
                          int anchorX, int anchorY) {
  std::vector<LyricVocalInterval> vocals;
  for (const auto &line : lyrics) vocals.push_back({line.start, line.start + line.duration});
  const auto windows = lyricCountdownWindows(std::move(vocals));
  if (windows.empty()) return 0;

  const int styleIndex = ass_alloc_style(track);
  if (styleIndex < 0) return 0;
  ASS_Style &style = track->styles[styleIndex];
  style.Name = ::strdup("Countdown");
  style.FontName = ::strdup("sans-serif");
  style.FontSize = 72;
  style.ScaleX = style.ScaleY = 1.0;
  // libass colors are RRGGBBAA; alpha 00 is opaque.
  style.PrimaryColour = 0x0011FF00;
  style.OutlineColour = 0xFFFFFF00;
  style.BorderStyle = 1;
  style.Alignment = 7;

  int added = 0;
  for (const auto &window : windows) {
    for (int slot = 0; slot < 3; ++slot) {
      const int64_t end = window.end - slot * 1000;
      if (end <= window.start) continue;
      const int index = ass_alloc_event(track);
      if (index < 0) continue;
      ASS_Event &event = track->events[index];
      event.Start = window.start;
      event.Duration = end - window.start;
      event.Style = styleIndex;
      event.Layer = 1;
      event.ReadOrder = index;
      // 72px circle, 5px white rim; fixed overrides isolate it from font settings.
      const std::string text = "{\\an7\\pos(" + std::to_string(anchorX + slot * 108) +
          "," + std::to_string(anchorY) +
          ")\\fscx100\\fscy100\\bord5\\shad0\\1c&HFF1100&\\3c&HFFFFFF&\\alpha&H00&\\p1}"
          "m 36 5 b 53 5 67 19 67 36 b 67 53 53 67 36 67 "
          "b 19 67 5 53 5 36 b 5 19 19 5 36 5{\\p0}";
      replaceOwnedString(&event.Text, text);
      ++added;
    }
  }
  return added;
}

const char *getEventStyleName(const ASS_Track *track, const ASS_Event *event) {

  if (!track || !event || event->Style < 0 || event->Style >= track->n_styles) {

    return nullptr;

  }

  return track->styles[event->Style].Name;

}



std::string normalizeHeaderStyleName(std::string styleName) {
  while (!styleName.empty() && std::isspace(static_cast<unsigned char>(styleName.front()))) styleName.erase(styleName.begin());
  while (!styleName.empty() && std::isspace(static_cast<unsigned char>(styleName.back()))) styleName.pop_back();
  for (char &ch : styleName) if (static_cast<unsigned char>(ch) < 0x80) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return styleName;
}

HeaderCategory getHeaderCategory(const std::string &styleName) {
  const std::string normalized = normalizeHeaderStyleName(styleName);
  if (normalized == "songname" || normalized == "songname:" || normalized == "title" ||
      normalized == "song" || normalized == "songtitle" || normalized == "tracktitle" ||
      normalized == "歌名" || normalized == "歌名:" || normalized == "歌曲" || normalized == "曲名") {
    return HeaderCategory::kSongName;
  }
  if (normalized == "singer" || normalized == "artist" || normalized == "vocal" || normalized == "演唱") {
    return HeaderCategory::kSinger;
  }
  if (normalized == "producer" || normalized == "album" || normalized == "credit" ||
      normalized == "credits" || normalized == "出品" || normalized == "专辑") {
    return HeaderCategory::kProducer;
  }
  return HeaderCategory::kUnknown;
}



void applyDefaultStyles(ASS_Track *track, double titleFontSize,
                        double titleOutlineWidth,
                        double textFontSize, double textOutlineWidth,
                        double textSpacing) {

  for (int i = 0; i < track->n_styles; ++i) {

    ASS_Style *style = &track->styles[i];

    if (!style) {

      continue;

    }



    const std::string styleName = style->Name ? style->Name : "";

    // Our libass uses BackColour for the sung outline. BorderStyle 4 adds
    // a separate translucent black rectangle; Shadow controls its padding.
    style->BorderStyle = 4;
    style->BackColour = kLyricSungOutlineColour;
    style->Shadow = kLyricBackgroundPadding;

    if (styleName == "Lyric" || styleName == "\xe6\xad\x8c\xe8\xaf\x8d") {

      style->FontSize = textFontSize;

      // Primary color is selected per line by the external vocal-role rule.

      style->SecondaryColour = kLyricSecondaryColour;

      style->OutlineColour = kLyricUnsungOutlineColour;

      style->Outline = textOutlineWidth;
      style->Spacing = textSpacing;

      continue;

    }



    if (getHeaderCategory(styleName) == HeaderCategory::kSongName) {

      style->FontSize = titleFontSize;

      style->PrimaryColour = 0x0021FF00;

      style->OutlineColour = 0xFFFFFF00;

      style->Outline = titleOutlineWidth;

      continue;

    }



    if (getHeaderCategory(styleName) == HeaderCategory::kSinger ||
        getHeaderCategory(styleName) == HeaderCategory::kProducer) {

      style->FontSize = 60.0;

      style->PrimaryColour = 0xFFFFFF00;

      style->OutlineColour = 0x00000000;

      style->Outline = 2.6;

    }

  }

}



void clearHeaderEvent(ASS_Event *event) {

  if (!event) {

    return;

  }

  replaceOwnedString(&event->Text, "");

  event->Duration = 0;

}



} // 命名空间



void LyricRenderer::applyHeaderVisibilityToTrack() {
  ASS_Track* track = currentTrack();
  if (headerVisible_ || !track) return;
  for (int i = 0; i < track->n_events; ++i) {
    ASS_Event* event = &track->events[i];
    const char* styleName = getEventStyleName(track, event);
    if (styleName && getHeaderCategory(styleName) != HeaderCategory::kUnknown) {
      clearHeaderEvent(event);
    }
  }
}

int LyricRenderer::adjustSubtitleTimingAndTracks() {

  if (!assRenderer_ || !assRenderer_->isInitialized()) {

    return 0;

  }



  ASS_Track *track = assRenderer_->getTrack();

  if (!track || track->n_events <= 0 || track->n_styles <= 0) {

    return 0;

  }



  // External layout owns typography and the vocal palette. Resolve voice before
  // pinyin conversion and line duplication; unlabeled continuations inherit it.
  std::vector<int> vocalOrder;
  for (int i = 0; i < track->n_events; ++i)
    if (containsKaraokeTag(track->events[i].Text)) vocalOrder.push_back(i);
  std::stable_sort(vocalOrder.begin(), vocalOrder.end(), [track](int a, int b) {
    return track->events[a].Start < track->events[b].Start;
  });
  LyricVocalRole previousRole = LyricVocalRole::Unknown;
  for (int index : vocalOrder) {
    ASS_Event& event = track->events[index];
    const char* name = getEventStyleName(track, &event);
    const uint32_t color = name ? track->styles[event.Style].PrimaryColour : 0;
    previousRole = resolveLyricVocalRole(event.Text, name ? name : "", color, previousRole);
    if (name) track->styles[event.Style].Spacing = kLyricCharacterSpacing;
    const auto spacedText = stripAssTags(event.Text, AssTagGroup::CharacterSpacing);
    replaceOwnedString(&event.Text, applyLyricVocalColor(spacedText, previousRole));
  }
  applyDefaultStyles(track, titleFontSize_, titleOutlineWidth_,
                     textFontSize_, textOutlineWidth_, textSpacing_);

  // Convert the original events before building waiting lines and listening
  // mode copies. Karaoke tags and timestamps keep their original ownership.
  if (pinyinEnabled_) {
    for (int i = 0; i < track->n_events; ++i) {
      ASS_Event &event = track->events[i];
      if (!event.Text) continue;
      const std::string text = convertAssToPinyin(event.Text);
      if (text != event.Text) replaceOwnedString(&event.Text, text);
    }
  }



  const int originalEventCount = track->n_events;

  int adjustedCount = 0;

  int64_t trackEndTime = 0;



  const int playResX = track->PlayResX > 0 ? track->PlayResX : 1920;

  const int playResY = track->PlayResY > 0 ? track->PlayResY : 1080;

  centerX_ = playResX / 2;



  if (displayMode_ == DisplayMode::LISTENING) {

    const int centerX = centerX_;

    const int highlightY = static_cast<int>(playResY * 0.68f);

    const int lineSpacing = static_cast<int>(playResY * 0.095f);

    const int scrollDurationMs = 420;



    std::vector<LyricEventSnapshot> lyricEvents;

    lyricEvents.reserve(static_cast<size_t>(track->n_events));

    for (int i = 0; i < originalEventCount; ++i) {

      ASS_Event *event = &track->events[i];

      if (!event || !event->Text || !containsKaraokeTag(event->Text)) {

        continue;

      }



      const int64_t eventEnd = static_cast<int64_t>(event->Start) +

                               static_cast<int64_t>(event->Duration);

      trackEndTime = std::max(trackEndTime, eventEnd);



      LyricEventSnapshot snapshot;

      snapshot.eventIndex = i;

      snapshot.start = event->Start;

      snapshot.duration = event->Duration;

      snapshot.readOrder = event->ReadOrder;

      snapshot.layer = event->Layer;

      snapshot.style = event->Style;

      snapshot.marginL = event->MarginL;

      snapshot.marginR = event->MarginR;

      snapshot.marginV = event->MarginV;

      snapshot.text = event->Text;

      lyricEvents.push_back(std::move(snapshot));

    }



    const int baseTitleY = std::max(24, playResY / 60);
    const int titleY = baseTitleY + 5;

    const int singerY = std::max(baseTitleY + 92, playResY / 12);

    const int producerY = std::max(singerY + 64, playResY / 8);

    const std::array<int, 3> headerPosY = {

        titleY,

        singerY,

        producerY,

    };



    std::array<int, 3> primaryHeaderIndices = {-1, -1, -1};

    for (int i = 0; i < originalEventCount; ++i) {

      ASS_Event *event = &track->events[i];

      if (!event || !event->Text) {

        continue;

      }



      const char *s = getEventStyleName(track, event);
      const std::string styleName = s ? s : "";

      const HeaderCategory category = getHeaderCategory(styleName);

      if (category == HeaderCategory::kUnknown) {

        continue;

      }



      const size_t categoryIndex = static_cast<size_t>(category);

      if (primaryHeaderIndices[categoryIndex] < 0 ||

          event->Start < track->events[primaryHeaderIndices[categoryIndex]].Start) {

        primaryHeaderIndices[categoryIndex] = i;

      }

    }



    for (int i = 0; i < originalEventCount; ++i) {

      ASS_Event *event = &track->events[i];

      if (!event || !event->Text) {

        continue;

      }



      const char *s = getEventStyleName(track, event);
      const std::string styleName = s ? s : "";

      const HeaderCategory category = getHeaderCategory(styleName);

      if (category == HeaderCategory::kUnknown) {

        continue;

      }



      const size_t categoryIndex = static_cast<size_t>(category);

      if (primaryHeaderIndices[categoryIndex] != i) {

        clearHeaderEvent(event);

        ++adjustedCount;

      }

    }



    for (size_t categoryIndex = 0; categoryIndex < primaryHeaderIndices.size(); ++categoryIndex) {

      const int eventIndex = primaryHeaderIndices[categoryIndex];

      if (eventIndex < 0) {

        continue;

      }



      ASS_Event *event = &track->events[eventIndex];

      if (!event || !event->Text) {

        continue;

      }



      const int64_t originalDuration = event->Duration;

      std::string headerText = stripPositioningTags(event->Text);
      if (categoryIndex == 2 && !producerText_.empty()) {
        headerText = producerText_;
      }

      if (categoryIndex == 0) {

        headerText = "{\\fs" + std::to_string(titleFontSize_) +
                     "}{\\c&HFFFFFF&}{\\3c&H000000&}{\\bord" +
                     std::to_string(titleOutlineWidth_) + "}" + headerText;

      } else if (categoryIndex == 1) {

        headerText = "{\\fs62}{\\c&HFFFFFF&}{\\3c&H000000&}{\\bord1.8}" + headerText;

      } else {

        headerText = "{\\fs54}{\\c&HE9EEF5&}{\\3c&H000000&}{\\bord1.6}" + headerText;

      }

      headerText = "{\\an8}{\\pos(" + std::to_string(centerX_) + "," +
                   std::to_string(headerPosY[categoryIndex]) + ")}" + headerText;

      replaceOwnedString(&event->Text, headerText);

      event->Start = 0;

      event->Duration = static_cast<long long>(std::max<int64_t>(
          trackEndTime > 0 ? trackEndTime + 1000 : originalDuration,
          originalDuration));

      ++adjustedCount;

    }



    auto buildLineText = [&](const std::string &rawText, int offset, bool animate) {

      std::string lineText = stripPositioningTags(rawText);

      lineText = stripKaraokeTags(lineText);



      float offsetUnits = static_cast<float>(offset);

      if (offset == -1) {

        offsetUnits = -1.18f;

      } else if (offset == -2) {

        offsetUnits = -2.22f;

      } else if (offset == -3) {

        offsetUnits = -3.26f;

      } else if (offset == 1) {

        offsetUnits = 1.08f;

      } else if (offset == 2) {

        offsetUnits = 2.12f;

      } else if (offset >= 3) {

        offsetUnits = 3.16f;

      }



      const int yEnd = highlightY + static_cast<int>(offsetUnits * lineSpacing);

      const int yStart = yEnd + static_cast<int>(lineSpacing * 0.90f);

      std::string motionTag;

      if (animate) {

        motionTag = "{\\move(" + std::to_string(centerX) + "," +
                    std::to_string(yStart) + "," + std::to_string(centerX) + "," +
                    std::to_string(yEnd) + ",0," + std::to_string(scrollDurationMs) + ")}";

      } else {

        motionTag = "{\\an5}{\\pos(" + std::to_string(centerX) + "," +

                    std::to_string(yEnd) + ")}";

      }



      if (offset == 0) {

        if (listeningEffectStyle_ == ListeningEffectStyle::DREAM) {

          return motionTag +

                 "{\\c&HFFF6FB&}{\\alpha&H00&}{\\3c&H5B2258&}{\\bord1.2}" +

                 lineText;

        }

        if (listeningEffectStyle_ == ListeningEffectStyle::NEON) {

          return motionTag +

                 "{\\c&HFEFFF6&}{\\alpha&H00&}{\\3c&H0A3A36&}{\\bord1.1}" +

                 lineText;

        }

        return motionTag +

               "{\\c&HFFFDF8&}{\\alpha&H00&}{\\3c&H24160E&}{\\bord1.1}" +

               lineText;

      }

      if (offset == -1) {

        if (listeningEffectStyle_ == ListeningEffectStyle::DREAM) {

          return motionTag +

                 "{\\c&HE7D8EC&}{\\alpha&H8E&}{\\3c&H241126&}{\\bord0.45}" +

                 lineText;

        }

        if (listeningEffectStyle_ == ListeningEffectStyle::NEON) {

          return motionTag +

                 "{\\c&HC9FFF8&}{\\alpha&H88&}{\\3c&H072826&}{\\bord0.42}" +

                 lineText;

        }

        return motionTag +

               "{\\c&HBDC9D7&}{\\alpha&H92&}{\\3c&H0F1419&}{\\bord0.44}" +

               lineText;

      }

      if (offset == 1) {

        if (listeningEffectStyle_ == ListeningEffectStyle::DREAM) {

          return motionTag +

                 "{\\c&HF7E8FA&}{\\alpha&H72&}{\\3c&H2B1230&}{\\bord0.52}" +

                 lineText;

        }

        if (listeningEffectStyle_ == ListeningEffectStyle::NEON) {

          return motionTag +

                 "{\\c&HE7FFF9&}{\\alpha&H68&}{\\3c&H0A3B37&}{\\bord0.48}" +

                 lineText;

        }

        return motionTag +

               "{\\c&HDCE4EE&}{\\alpha&H72&}{\\3c&H12171D&}{\\bord0.5}" +

                 lineText;

      }

      if (listeningEffectStyle_ == ListeningEffectStyle::DREAM) {

        return motionTag +

               "{\\c&HAB99B7&}{\\alpha&HDA&}{\\3c&H160B18&}{\\bord0.24}" +

               lineText;

      }

      if (listeningEffectStyle_ == ListeningEffectStyle::NEON) {

        return motionTag +

               "{\\c&H78C9C0&}{\\alpha&HDA&}{\\3c&H061A18&}{\\bord0.2}" +

               lineText;

      }

      return motionTag +

             "{\\c&H8794A3&}{\\alpha&HDA&}{\\3c&H080C10&}{\\bord0.22}" +

             lineText;

    };



    for (size_t i = 0; i < lyricEvents.size(); ++i) {

      const LyricEventSnapshot &snapshot = lyricEvents[i];

      ASS_Event *baseEvent = &track->events[snapshot.eventIndex];

      if (!baseEvent) {

        continue;

      }



      const int64_t nextStart = (i + 1 < lyricEvents.size())
          ? lyricEvents[i + 1].start
          : (snapshot.start + std::max<int64_t>(snapshot.duration, 1600));

      const int64_t displayUntil = std::max<int64_t>(nextStart, snapshot.start + snapshot.duration);

      const int64_t displayDuration = std::max<int64_t>(0, displayUntil - snapshot.start);



      baseEvent->Start = snapshot.start;

      baseEvent->Duration = static_cast<long long>(displayDuration);

      replaceOwnedString(&baseEvent->Text, buildLineText(snapshot.text, 0, i > 0));

      ++adjustedCount;



      for (int offset = -3; offset <= 3; ++offset) {

        if (offset == 0) {

          continue;

        }

        const int targetIndex = static_cast<int>(i) + offset;

        if (targetIndex < 0 || targetIndex >= static_cast<int>(lyricEvents.size())) {

          continue;

        }



        const LyricEventSnapshot &neighbor = lyricEvents[targetIndex];

        const int eventIndex = ass_alloc_event(track);

        if (eventIndex < 0) {

          continue;

        }



        ASS_Event *ghostEvent = &track->events[eventIndex];

        ghostEvent->Start = snapshot.start;

        ghostEvent->Duration = static_cast<long long>(displayDuration);

        ghostEvent->ReadOrder = snapshot.readOrder + 100 + (offset + 3);

        ghostEvent->Layer = snapshot.layer;

        ghostEvent->Style = snapshot.style;

        ghostEvent->MarginL = neighbor.marginL;

        ghostEvent->MarginR = neighbor.marginR;

        ghostEvent->MarginV = neighbor.marginV;



        replaceOwnedString(&ghostEvent->Text,

                           buildLineText(neighbor.text, offset, i > 0));

        ++adjustedCount;

      }

    }



    LOG_INFO("[Lyric诊断] 听歌模式轨道事件: original=%d lyricLines=%zu adjusted=%d final=%d",

             originalEventCount, lyricEvents.size(), adjustedCount, track->n_events);



    adjustedCount += appendCountdownEvents(track, lyricEvents,
        std::max(0, centerX_ - 144), std::max(0, highlightY - 240));
    return adjustedCount;

  }



  const int effectiveLeft = displayMargin_.left;

  const int effectiveRight = playResX - displayMargin_.right;



  topX_ = 200;

  topY_ = playResY * 860 / 1080;

  bottomX_ = playResX - 200;

  bottomY_ = playResY * 1010 / 1080;

  centerX_ = effectiveLeft + (effectiveRight - effectiveLeft) / 2;



  // Progressive snapshots can contain headers only: position them before any lyric early return.
  std::array<int, 3> primaryHeaderIndices = {-1, -1, -1};

  for (int i = 0; i < originalEventCount; ++i) {

    ASS_Event *event = &track->events[i];

    if (!event || !event->Text) {

      continue;

    }



    const char *s = getEventStyleName(track, event);
    const std::string styleName = s ? s : "";

    const HeaderCategory category = getHeaderCategory(styleName);

    if (category == HeaderCategory::kUnknown) {

      continue;

    }



    const size_t categoryIndex = static_cast<size_t>(category);

    if (primaryHeaderIndices[categoryIndex] < 0 ||

        event->Start < track->events[primaryHeaderIndices[categoryIndex]].Start) {

      primaryHeaderIndices[categoryIndex] = i;

    }

  }



  for (int i = 0; i < originalEventCount; ++i) {

    ASS_Event *event = &track->events[i];

    if (!event || !event->Text) {

      continue;

    }



    const char *s = getEventStyleName(track, event);
    const std::string styleName = s ? s : "";

    const HeaderCategory category = getHeaderCategory(styleName);

    if (category == HeaderCategory::kUnknown) {

      continue;

    }



    const size_t categoryIndex = static_cast<size_t>(category);

    if (primaryHeaderIndices[categoryIndex] != i) {

      clearHeaderEvent(event);

      ++adjustedCount;

    }

  }



  const std::array<int, 3> headerPosY = {
      playResY * 20 / 100 + 5,
      playResY * 35 / 100,
      playResY * 42 / 100,
  };



  for (size_t categoryIndex = 0; categoryIndex < primaryHeaderIndices.size(); ++categoryIndex) {

    const int eventIndex = primaryHeaderIndices[categoryIndex];

    if (eventIndex < 0) {

      continue;

    }



    ASS_Event *event = &track->events[eventIndex];

    if (!event || !event->Text) {

      continue;

    }



    const int64_t originalDuration = event->Duration;

    std::string headerText = stripPositioningTags(event->Text);
    if (categoryIndex == 2 && !producerText_.empty()) {
      headerText = producerText_;
    }

    headerText = "{\\an8}{\\pos(" + std::to_string(centerX_) + "," +

                 std::to_string(headerPosY[categoryIndex]) + ")}" + headerText;

    replaceOwnedString(&event->Text, headerText);

    event->Duration = originalDuration;

    ++adjustedCount;

  }



  std::vector<LyricEventSnapshot> lyricEvents;

  lyricEvents.reserve(static_cast<size_t>(track->n_events));

  for (int i = 0; i < originalEventCount; ++i) {

    ASS_Event *event = &track->events[i];

    if (!event || !event->Text || !containsKaraokeTag(event->Text)) {

      continue;

    }



    LyricEventSnapshot snapshot;

    snapshot.eventIndex = i;

    snapshot.start = event->Start;

    snapshot.duration = event->Duration;

    snapshot.readOrder = event->ReadOrder;

    snapshot.layer = event->Layer;

    snapshot.style = event->Style;

    snapshot.marginL = event->MarginL;

    snapshot.marginR = event->MarginR;

    snapshot.marginV = event->MarginV;

    snapshot.text = event->Text;

    lyricEvents.push_back(std::move(snapshot));

  }



  if (lyricEvents.empty()) {

    return adjustedCount;

  }



  std::stable_sort(lyricEvents.begin(), lyricEvents.end(),
      [](const auto &a, const auto &b) { return a.start < b.start; });

  const int64_t firstLyricStartTime = lyricEvents.front().start;

  const int64_t countdownStartTime = std::max<int64_t>(0, firstLyricStartTime - 3000);



  // Preserve the fully sung line for one second without shifting karaoke tags.
  // A new lyric in the same row takes priority once its singing starts.
  constexpr int64_t kCompletedLineHoldMs = 1000;
  std::vector<int64_t> lineDisplayEndTimes;
  lineDisplayEndTimes.reserve(lyricEvents.size());
  for (size_t i = 0; i < lyricEvents.size(); ++i) {
    const int64_t vocalEnd = lyricEvents[i].start + lyricEvents[i].duration;
    int64_t displayEnd = vocalEnd + kCompletedLineHoldMs;
    if (i + 2 < lyricEvents.size()) {
      displayEnd = std::min(displayEnd,
                           std::max(vocalEnd, lyricEvents[i + 2].start));
    }
    lineDisplayEndTimes.push_back(displayEnd);
  }

  for (size_t i = 0; i < lyricEvents.size(); ++i) {

    const LyricEventSnapshot &snapshot = lyricEvents[i];

    const bool isUpperLine = (i % 2) == 0;

    const int posX = isUpperLine ? topX_ : bottomX_;

    const int posY = isUpperLine ? topY_ : bottomY_;

    const int alignment = isUpperLine ? 1 : 3;





    const int64_t waitStart = std::min(
        snapshot.start, i >= 2 ? lineDisplayEndTimes[i - 2] : countdownStartTime);

    ASS_Event *waitEvent = &track->events[snapshot.eventIndex];

    if (!waitEvent) {

      continue;

    }



    waitEvent->Start = waitStart;

    waitEvent->Duration = std::max<int64_t>(0, snapshot.start - waitStart);



    std::string waitText = stripPositioningTags(snapshot.text);

    waitText = stripKaraokeTags(waitText);
    // Waiting lines remain white; vocal colors apply when their singing starts.
    waitText = stripAssTags(waitText, AssTagGroup::PrimaryColor);

    waitText = "{\\an" + std::to_string(alignment) + "}{\\pos(" +

               std::to_string(posX) + "," + std::to_string(posY) + ")}" +

               "{\\c&HFFFFFF&}{\\3c&H000000&}" + waitText;

    replaceOwnedString(&waitEvent->Text, waitText);

    ++adjustedCount;



    const int scrollIndex = ass_alloc_event(track);

    if (scrollIndex < 0) {

      continue;

    }



    ASS_Event *scrollEvent = &track->events[scrollIndex];

    scrollEvent->Start = snapshot.start;

    scrollEvent->Duration = lineDisplayEndTimes[i] - snapshot.start;

    scrollEvent->ReadOrder = snapshot.readOrder;

    scrollEvent->Layer = snapshot.layer;

    scrollEvent->Style = snapshot.style;

    scrollEvent->MarginL = snapshot.marginL;

    scrollEvent->MarginR = snapshot.marginR;

    scrollEvent->MarginV = snapshot.marginV;



    std::string scrollText = stripPositioningTags(snapshot.text);

    scrollText = "{\\an" + std::to_string(alignment) + "}{\\pos(" +

                 std::to_string(posX) + "," + std::to_string(posY) + ")}" +

                 scrollText;

    replaceOwnedString(&scrollEvent->Text, scrollText);

    ++adjustedCount;

  }



  adjustedCount += appendCountdownEvents(track, lyricEvents, topX_,
      std::max(0, topY_ - static_cast<int>(kLyricFontSize * 1.1) - 92));
  return adjustedCount;

}



} // 命名空间 hsvj
