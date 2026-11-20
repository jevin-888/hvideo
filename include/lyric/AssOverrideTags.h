#pragma once

#include <cctype>
#include <string>

namespace hsvj {
enum class AssTagGroup { Position, Karaoke, PrimaryColor, CharacterSpacing };

inline bool matchesAssTagGroup(const std::string& name, AssTagGroup group) {
  if (group == AssTagGroup::Position)
    return name == "pos" || name == "move" || name == "an" || name == "a";
  if (group == AssTagGroup::Karaoke)
    return name == "k" || name == "K" || name == "kf" || name == "ko" || name == "kt";
  if (group == AssTagGroup::CharacterSpacing) return name == "fsp";
  return name == "c" || name == "1c";
}

// A block can contain placement, color and karaoke at once. Remove only the
// requested commands, retaining all other commands and their original order.
inline std::string filterAssTagCommands(const std::string& block, AssTagGroup group,
                                        bool* found = nullptr, int depth = 0) {
  if (depth > 16) return block;
  std::string result;
  for (size_t i = 0; i < block.size();) {
    if (block[i] != '\\') { result += block[i++]; continue; }
    const size_t start = i++;
    const size_t nameStart = i;
    if (i < block.size() && block[i] >= '1' && block[i] <= '4') ++i;
    while (i < block.size() && std::isalpha(static_cast<unsigned char>(block[i]))) ++i;
    const std::string name = block.substr(nameStart, i - nameStart);
    const size_t args = i;
    int parentheses = 0;
    while (i < block.size()) {
      if (block[i] == '\\' && parentheses == 0) break;
      if (block[i] == '(') ++parentheses;
      else if (block[i] == ')' && parentheses > 0) --parentheses;
      ++i;
    }
    if (matchesAssTagGroup(name, group)) {
      if (found) *found = true;
      continue;
    }
    std::string command = block.substr(start, i - start);
    if (name == "t" && args < i && block[args] == '(' && block[i - 1] == ')') {
      const std::string inner = filterAssTagCommands(block.substr(args + 1, i - args - 2), group, found, depth + 1);
      if (inner.find('\\') == std::string::npos) continue;
      command = "\\t(" + inner + ")";
    }
    result += command;
  }
  return result;
}

inline std::string stripAssTags(const std::string& text, AssTagGroup group,
                                bool* found = nullptr) {
  std::string result;
  size_t cursor = 0;
  while (cursor < text.size()) {
    const size_t open = text.find('{', cursor);
    const size_t close = open == std::string::npos ? std::string::npos : text.find('}', open + 1);
    if (close == std::string::npos) { result += text.substr(cursor); break; }
    result += text.substr(cursor, open - cursor);
    const auto filtered = filterAssTagCommands(text.substr(open + 1, close - open - 1), group, found);
    if (!filtered.empty()) result += "{" + filtered + "}";
    cursor = close + 1;
  }
  return result;
}
} // namespace hsvj
