#include "lyric/LyricRenderer.h"

#include "lyric/ASSRenderer.h"

#include "utils/Logger.h"



#include <algorithm>

#include <array>

#include <cstring>

#include <string>

#include <vector>



extern "C" {

#include <ass.h>

}



namespace hsvj {



namespace {



constexpr double kLyricFontSize = 150.0;

constexpr int32_t kLyricPrimaryColour = 0xFF210000;

constexpr int32_t kLyricSecondaryColour = 0xFFFFFF00;

constexpr int32_t kLyricOutlineColour = 0x00000000;

constexpr int32_t kLyricBackColour = 0xFFFFFF00;

constexpr double kLyricOutline = 1.5;

constexpr double kLyricShadow = 0.0;



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

  if (!text) {

    return false;

  }

  return std::strstr(text, "{\\K") != nullptr || std::strstr(text, "{\\k") != nullptr;

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



std::string stripTagPrefix(const std::string &text, const char *tagPrefix) {

  std::string result = text;

  size_t pos = 0;

  while ((pos = result.find(tagPrefix, pos)) != std::string::npos) {

    const size_t end = result.find('}', pos);

    if (end == std::string::npos) {

      break;

    }

    result.erase(pos, end - pos + 1);

  }

  return result;

}



std::string stripPositioningTags(const std::string &text) {

  std::string result = text;

  result = stripTagPrefix(result, "{\\pos");

  result = stripTagPrefix(result, "{\\move");

  result = stripTagPrefix(result, "{\\an");

  return result;

}



std::string stripKaraokeTags(const std::string &text) {

  std::string result = text;

  result = stripTagPrefix(result, "{\\K");

  result = stripTagPrefix(result, "{\\k");

  return result;

}



const char *getEventStyleName(const ASS_Track *track, const ASS_Event *event) {

  if (!track || !event || event->Style < 0 || event->Style >= track->n_styles) {

    return nullptr;

  }

  return track->styles[event->Style].Name;

}



HeaderCategory getHeaderCategory(const std::string &styleName) {

  if (styleName == "SongName" || styleName == "SongName:" ||

      styleName == "\xe6\xad\x8c\xe5\x90\x8d" || styleName == "\xe6\xad\x8c\xe5\x90\x8d:") {

    return HeaderCategory::kSongName;

  }

  if (styleName == "Singer" || styleName == "\xe6\xbc\x94\xe5\x94\xb1") {

    return HeaderCategory::kSinger;

  }

  if (styleName == "Producer" || styleName == "Album" ||

      styleName == "\xe5\x87\xba\xe5\x93\x81" || styleName == "\xe4\xb8\x93\xe8\xbe\x91") {

    return HeaderCategory::kProducer;

  }

  return HeaderCategory::kUnknown;

}



void applyDefaultStyles(ASS_Track *track) {

  for (int i = 0; i < track->n_styles; ++i) {

    ASS_Style *style = &track->styles[i];

    if (!style) {

      continue;

    }



    const std::string styleName = style->Name ? style->Name : "";

    if (styleName == "Lyric" || styleName == "\xe6\xad\x8c\xe8\xaf\x8d") {

      style->FontSize = kLyricFontSize;

      style->PrimaryColour = kLyricPrimaryColour;

      style->SecondaryColour = kLyricSecondaryColour;

      style->OutlineColour = kLyricOutlineColour;

      style->BackColour = kLyricBackColour;

      style->BorderStyle = 1;

      style->Outline = kLyricOutline;

      style->Shadow = kLyricShadow;

      continue;

    }



    if (styleName == "SongName" || styleName == "SongName:" ||

        styleName == "\xe6\xad\x8c\xe5\x90\x8d" || styleName == "\xe6\xad\x8c\xe5\x90\x8d:") {

      style->FontSize = 150.0;

      style->PrimaryColour = 0xFF210000;

      style->OutlineColour = 0xFFFFFF00;

      style->BorderStyle = 1;

      style->Outline = 3.0;

      style->Shadow = 0.0;

      continue;

    }



    if (styleName == "Singer" || styleName == "Album" ||

        styleName == "\xe6\xbc\x94\xe5\x94\xb1" || styleName == "\xe4\xb8\x93\xe8\xbe\x91" ||

        styleName == "Producer" || styleName == "\xe5\x87\xba\xe5\x93\x81") {

      style->FontSize = 60.0;

      style->PrimaryColour = 0xFFFFFF00;

      style->OutlineColour = 0x00000000;

      style->BorderStyle = 1;

      style->Outline = 2.0;

      style->Shadow = 0.0;

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



int LyricRenderer::adjustSubtitleTimingAndTracks() {

  if (!assRenderer_ || !assRenderer_->isInitialized()) {

    return 0;

  }



  ASS_Track *track = assRenderer_->getTrack();

  if (!track || track->n_events <= 0 || track->n_styles <= 0) {

    return 0;

  }



  applyDefaultStyles(track);



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



    const int titleY = std::max(24, playResY / 60);

    const int singerY = std::max(titleY + 92, playResY / 12);

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

      if (categoryIndex == 0) {

        headerText = "{\\fs96}{\\c&HFFFFFF&}{\\3c&H000000&}{\\bord1.8}" + headerText;

      } else if (categoryIndex == 1) {

        headerText = "{\\fs62}{\\c&HFFFFFF&}{\\3c&H000000&}{\\bord1.2}" + headerText;

      } else {

        headerText = "{\\fs54}{\\c&HE9EEF5&}{\\3c&H000000&}{\\bord1.0}" + headerText;

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



    return adjustedCount;

  }



  const int effectiveLeft = displayMargin_.left;

  const int effectiveRight = playResX - displayMargin_.right;



  topX_ = 200;

  topY_ = playResY - 300;

  bottomX_ = playResX - 200;

  bottomY_ = playResY - 150;

  karaokeCountdownAnchorX_ = topX_;

  karaokeCountdownAnchorY_ = topY_;

  centerX_ = effectiveLeft + (effectiveRight - effectiveLeft) / 2;



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

    return 0;

  }



  const int64_t firstLyricStartTime = lyricEvents.front().start;

  const int64_t countdownStartTime = std::max<int64_t>(0, firstLyricStartTime - 3000);



  std::vector<int64_t> upperLineEndTimes;

  std::vector<int64_t> lowerLineEndTimes;

  upperLineEndTimes.reserve((lyricEvents.size() + 1) / 2);

  lowerLineEndTimes.reserve(lyricEvents.size() / 2);

  for (size_t i = 0; i < lyricEvents.size(); ++i) {

    const int64_t endTime = lyricEvents[i].start + lyricEvents[i].duration;

    if ((i % 2) == 0) {

      upperLineEndTimes.push_back(endTime);

    } else {

      lowerLineEndTimes.push_back(endTime);

    }

  }



  for (size_t i = 0; i < lyricEvents.size(); ++i) {

    const LyricEventSnapshot &snapshot = lyricEvents[i];

    const bool isUpperLine = (i % 2) == 0;

    const int posX = isUpperLine ? topX_ : bottomX_;

    const int posY = isUpperLine ? topY_ : bottomY_;

    const int alignment = isUpperLine ? 1 : 3;



    if (i == 0) {

      karaokeCountdownAnchorX_ = posX;

      karaokeCountdownAnchorY_ = posY;

    }



    int64_t waitStart = countdownStartTime;

    if (isUpperLine) {

      const size_t lineIndex = i / 2;

      if (lineIndex > 0) {

        waitStart = upperLineEndTimes[lineIndex - 1];

      }

    } else {

      const size_t lineIndex = (i - 1) / 2;

      if (lineIndex > 0) {

        waitStart = lowerLineEndTimes[lineIndex - 1];

      }

    }

    if (waitStart > snapshot.start) {

      waitStart = snapshot.start;

    }



    ASS_Event *waitEvent = &track->events[snapshot.eventIndex];

    if (!waitEvent) {

      continue;

    }



    waitEvent->Start = waitStart;

    waitEvent->Duration = std::max<int64_t>(0, snapshot.start - waitStart);



    std::string waitText = stripPositioningTags(snapshot.text);

    waitText = stripKaraokeTags(waitText);

    waitText = "{\\an" + std::to_string(alignment) + "}{\\pos(" +

               std::to_string(posX) + "," + std::to_string(posY) + ")}" +

               "{\\c&HFFFFFF&}{\\3c&H000000&}{\\bord2.0}" + waitText;

    replaceOwnedString(&waitEvent->Text, waitText);

    ++adjustedCount;



    const int scrollIndex = ass_alloc_event(track);

    if (scrollIndex < 0) {

      continue;

    }



    ASS_Event *scrollEvent = &track->events[scrollIndex];

    scrollEvent->Start = snapshot.start;

    scrollEvent->Duration = snapshot.duration;

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

      playResY * 20 / 100,

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

    headerText = "{\\an8}{\\pos(" + std::to_string(centerX_) + "," +

                 std::to_string(headerPosY[categoryIndex]) + ")}" + headerText;

    replaceOwnedString(&event->Text, headerText);

    event->Duration = originalDuration;

    ++adjustedCount;

  }



  return adjustedCount;

}



} // 命名空间 hsvj


