#include "lyric/LyricPinyin.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <vector>

namespace hsvj {
namespace {

struct CharacterReading {
    uint32_t codepoint;
    uint16_t syllable;
};
struct PhraseReading {
    const char* phrase;
    const char* syllables;
};
struct CharacterVariant {
    uint32_t original;
    uint32_t simplified;
};

#include "LyricPinyinData.inc"

struct Token {
    size_t start;
    size_t length;
    uint32_t codepoint = 0;
    std::string_view pinyin{};
    bool spaceAfter = false;
    bool overrideBlock = false;
};

bool isHan(uint32_t cp) {
    return cp == 0x3007 || (cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0x20000 && cp <= 0x2fa1f) || (cp >= 0x30000 && cp <= 0x323af);
}

// Invalid UTF-8 consumes just its original byte; no replacement or truncation.
uint32_t readCodepoint(const std::string& text, size_t offset, size_t& length) {
    const auto first = static_cast<unsigned char>(text[offset]);
    length = 1;
    if (first < 0x80) return first;
    const size_t count = first >= 0xc2 && first <= 0xdf ? 2 :
        first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!count || count > text.size() - offset) return 0;
    uint32_t cp = first & (0x7f >> count);
    for (size_t i = 1; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if ((byte & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (byte & 0x3f);
    }
    if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
        (count == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    length = count;
    return cp;
}

void appendCodepoint(std::string& text, uint32_t cp) {
    if (cp < 0x80) {
        text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        text.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        text.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string_view characterPinyin(uint32_t cp) {
    const auto found = std::lower_bound(std::begin(kCharacterReadings), std::end(kCharacterReadings), cp,
        [](const CharacterReading& entry, uint32_t value) { return entry.codepoint < value; });
    return found != std::end(kCharacterReadings) && found->codepoint == cp ?
        std::string_view(kPinyinSyllables[found->syllable]) : std::string_view();
}

uint32_t phraseCodepoint(uint32_t cp) {
    const auto found = std::lower_bound(std::begin(kTraditionalVariants), std::end(kTraditionalVariants), cp,
        [](const CharacterVariant& entry, uint32_t value) { return entry.original < value; });
    return found != std::end(kTraditionalVariants) && found->original == cp ? found->simplified : cp;
}

const PhraseReading* findPhrase(std::string_view phrase) {
    const auto found = std::lower_bound(std::begin(kPhraseReadings), std::end(kPhraseReadings), phrase,
        [](const PhraseReading& entry, std::string_view value) { return std::string_view(entry.phrase) < value; });
    return found != std::end(kPhraseReadings) && std::string_view(found->phrase) == phrase ? found : nullptr;
}

void convertRun(std::vector<Token>& tokens, std::vector<size_t>& run) {
    for (size_t i = 0; i < run.size();) {
        std::string original;
        std::string normalized;
        const PhraseReading* longest = nullptr;
        size_t longestLength = 0;
        const size_t remaining = std::min(kMaxPhraseCodepoints, run.size() - i);
        for (size_t length = 1; length <= remaining; ++length) {
            const auto cp = tokens[run[i + length - 1]].codepoint;
            appendCodepoint(original, cp);
            appendCodepoint(normalized, phraseCodepoint(cp));
            if (length < 2) continue;
            auto match = findPhrase(original);
            if (!match && original != normalized) match = findPhrase(normalized);
            if (match) {
                longest = match;
                longestLength = length;
            }
        }
        if (longest) {
            std::string_view syllables(longest->syllables);
            for (size_t j = 0; j < longestLength; ++j) {
                const size_t space = syllables.find(' ');
                tokens[run[i + j]].pinyin = syllables.substr(0, space);
                if (space != std::string_view::npos) syllables.remove_prefix(space + 1);
            }
            i += longestLength;
        } else {
            auto& token = tokens[run[i]];
            token.pinyin = characterPinyin(token.codepoint);
            // pinyin-pro's isolated 了 defaults to liǎo, but follows a Han
            // character as the sentence particle le unless a phrase matched.
            if (token.codepoint == 0x4e86 && i > 0 &&
                !characterPinyin(tokens[run[i - 1]].codepoint).empty()) token.pinyin = "le";
            ++i;
        }
    }
    for (size_t i = 1; i < run.size(); ++i) {
        if (!tokens[run[i - 1]].pinyin.empty() && !tokens[run[i]].pinyin.empty()) {
            // Put separation in the previous syllable's span, before the next
            // karaoke tag; each original timed character stays one syllable.
            tokens[run[i - 1]].spaceAfter = true;
        }
    }
    run.clear();
}

bool isLatinWordCharacter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || (cp >= 0xc0 && cp <= 0xd6) ||
        (cp >= 0xd8 && cp <= 0xf6) || (cp >= 0xf8 && cp <= 0x24f) ||
        (cp >= 0x1e00 && cp <= 0x1eff);
}

// Top-level \p changes drawing mode (\r does not reset it in libass).
// In particular, \pos, \pbo, \clip and tags nested in \t are not drawing toggles.
void updateDrawingMode(std::string_view block, bool& drawing) {
    unsigned depth = 0;
    for (size_t i = 1; i + 1 < block.size(); ++i) {
        if (block[i] == '(') { ++depth; continue; }
        if (block[i] == ')') { if (depth) --depth; continue; }
        if (depth || block[i] != '\\') continue;
        if (block[i + 1] != 'p') continue;
        size_t value = i + 2;
        while (value < block.size() && (block[value] == ' ' || block[value] == '\t')) ++value;
        if (value == block.size() || block[value] == '}' || block[value] == '\\') {
            drawing = false;
            continue;
        }
        bool negative = false;
        if (value < block.size() && (block[value] == '+' || block[value] == '-')) {
            negative = block[value] == '-';
            ++value;
        }
        if (value >= block.size() || block[value] < '0' || block[value] > '9') continue;
        bool nonzero = false;
        while (value < block.size() && block[value] >= '0' && block[value] <= '9') {
            nonzero |= block[value++] != '0';
        }
        drawing = nonzero && !negative;
    }
}

} // namespace

std::string convertAssToPinyin(const std::string& text) {
    std::vector<Token> tokens;
    std::vector<size_t> run;
    tokens.reserve(text.size() / 2);
    bool drawing = false;
    for (size_t offset = 0; offset < text.size();) {
        if (text[offset] == '{') {
            const size_t end = text.find('}', offset + 1);
            if (end != std::string::npos) {
                const size_t length = end - offset + 1;
                updateDrawingMode(std::string_view(text).substr(offset, length), drawing);
                if (drawing) convertRun(tokens, run);
                Token token{offset, length};
                token.overrideBlock = true;
                tokens.push_back(token);
                offset += length;
                continue;
            }
        }
        // Retain ASS line/space/brace escapes verbatim and prevent phrase lookup
        // from crossing them. An escaped brace cannot begin an override block.
        if (text[offset] == '\\' && offset + 1 < text.size() &&
            (text[offset + 1] == 'N' || text[offset + 1] == 'n' || text[offset + 1] == 'h' ||
             text[offset + 1] == '{' || text[offset + 1] == '}')) {
            convertRun(tokens, run);
            tokens.push_back({offset, 2});
            offset += 2;
            continue;
        }
        size_t length = 0;
        const uint32_t cp = readCodepoint(text, offset, length);
        if (!drawing && isHan(cp)) {
            run.push_back(tokens.size());
        } else {
            convertRun(tokens, run);
        }
        tokens.push_back({offset, length, drawing ? 0 : cp});
        offset += length;
    }
    convertRun(tokens, run);

    // English words keep all original characters and internal spacing, while
    // a Chinese/English boundary receives one space even across karaoke tags.
    size_t previousVisible = tokens.size();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].overrideBlock) continue;
        if (previousVisible != tokens.size()) {
            auto& previous = tokens[previousVisible];
            if ((!previous.pinyin.empty() && isLatinWordCharacter(tokens[i].codepoint)) ||
                (isLatinWordCharacter(previous.codepoint) && !tokens[i].pinyin.empty())) {
                previous.spaceAfter = true;
            }
        }
        previousVisible = i;
    }

    std::string converted;
    converted.reserve(text.size() * 2);
    for (const auto& token : tokens) {
        if (token.pinyin.empty()) {
            converted.append(text, token.start, token.length);
        } else {
            converted.append(token.pinyin.data(), token.pinyin.size());
        }
        if (token.spaceAfter) converted.push_back(' ');
    }
    return converted;
}

} // namespace hsvj
