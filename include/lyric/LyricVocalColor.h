#pragma once

#include "lyric/AssOverrideTags.h"
#include <cstdint>
#include <utility>

namespace hsvj {
enum class LyricVocalRole { Unknown, Male, Female, Chorus };

inline LyricVocalRole vocalRoleFromRgb(uint32_t rgb) {
  const unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  if (b > r * 2 && b > g * 2) return LyricVocalRole::Male;
  if (r > g * 2 && r > b * 2) return LyricVocalRole::Female;
  if (g > r * 2 && g > b * 2) return LyricVocalRole::Chorus;
  return LyricVocalRole::Unknown;
}

inline LyricVocalRole vocalRoleFromLabel(const std::string& raw) {
  std::string plain;
  bool tag = false;
  for (char c : raw) {
    if (c == '{') tag = true;
    else if (c == '}') tag = false;
    else if (!tag) plain += c;
  }
  const size_t first = plain.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return LyricVocalRole::Unknown;
  plain.erase(0, first);
  const std::pair<const char*, LyricVocalRole> labels[] = {
      {"男", LyricVocalRole::Male}, {"男声", LyricVocalRole::Male},
      {"女", LyricVocalRole::Female}, {"女声", LyricVocalRole::Female},
      {"合", LyricVocalRole::Chorus}, {"合唱", LyricVocalRole::Chorus},
      {"合声", LyricVocalRole::Chorus}};
  for (const auto& label : labels) {
    const std::string name = label.first;
    if (plain == name || plain.rfind(name + ":", 0) == 0 || plain.rfind(name + "：", 0) == 0)
      return label.second;
  }
  return LyricVocalRole::Unknown;
}

inline LyricVocalRole vocalRoleFromInlineColor(const std::string& raw) {
  // ASS inline colors are BGR. Only direct primary color tags are evidence;
  // animated transform colors, outline/background and alpha are not voice tags.
  size_t cursor = 0;
  while (cursor < raw.size()) {
    const auto open = raw.find('{', cursor);
    const auto close = raw.find('}', open == std::string::npos ? cursor : open);
    if (open == std::string::npos || close == std::string::npos) break;
    const auto block = raw.substr(open + 1, close - open - 1);
    int depth = 0;
    for (size_t i = 0; i < block.size(); ++i) {
      if (block[i] == '(') ++depth;
      if (block[i] == ')') --depth;
      if (block[i] != '\\' || depth != 0) continue;
      size_t start = i + 1;
      if (block.compare(start, 2, "1c") == 0) start += 2;
      else if (block.compare(start, 1, "c") == 0) ++start;
      else continue;
      while (start < block.size() && block[start] == ' ') ++start;
      if (block.compare(start, 2, "&H") != 0 && block.compare(start, 2, "&h") != 0) continue;
      start += 2;
      uint32_t bgr = 0;
      size_t n = 0;
      for (; start + n < block.size() && n < 8; ++n) {
        unsigned char ch = block[start + n];
        if (!std::isxdigit(ch)) break;
        bgr = (bgr << 4) | (ch <= '9' ? ch - '0' : std::tolower(ch) - 'a' + 10);
      }
      if (!n) continue;
      const auto role = vocalRoleFromRgb(((bgr & 255) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 255));
      if (role != LyricVocalRole::Unknown) return role;
    }
    cursor = close + 1;
  }
  return LyricVocalRole::Unknown;
}

inline LyricVocalRole resolveLyricVocalRole(const std::string& text, const std::string& styleName,
                                           uint32_t styleRgba, LyricVocalRole previous) {
  auto role = vocalRoleFromLabel(text);
  if (role == LyricVocalRole::Unknown) role = vocalRoleFromLabel(styleName);
  if (role == LyricVocalRole::Unknown) role = vocalRoleFromInlineColor(text);
  if (role == LyricVocalRole::Unknown) role = previous;
  if (role == LyricVocalRole::Unknown) role = vocalRoleFromRgb(styleRgba >> 8);
  return role == LyricVocalRole::Unknown ? LyricVocalRole::Male : role;
}

inline std::string applyLyricVocalColor(const std::string& text, LyricVocalRole role) {
  const char* color = role == LyricVocalRole::Female ? "0000FF" :
                      role == LyricVocalRole::Chorus ? "00FF00" : "FF2100";
  return std::string("{\\1c&H") + color + "&}" + stripAssTags(text, AssTagGroup::PrimaryColor);
}
} // namespace hsvj
