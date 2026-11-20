#include "core/QrCodeCard.h"
#include "qrcodegen.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace hsvj {
bool isVodMobileQr(const std::string& content) {
  const size_t start = content.rfind("http://", 0) == 0 ? 7 :
                       content.rfind("https://", 0) == 0 ? 8 : 0;
  if (!start) return false;
  const size_t path = content.find('/', start);
  if (path == std::string::npos || path == start) return false;
  return content.substr(path, content.find_first_of("?#", path) - path) ==
         "/vod_song/mobile";
}

std::string resolveVodMobileQr(const std::string& content, bool onlineVod,
                              const std::string& serverBase,
                              const std::string& localIp) {
  if (!onlineVod || !isVodMobileQr(content)) return content;
  // No loopback/hotspot guess: wait for the actual server connection address.
  std::istringstream parts(localIp);
  std::string part;
  int count = 0;
  while (std::getline(parts, part, '.')) {
    if (part.empty() || part.size() > 3 ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return "";
    const int n = std::stoi(part);
    if (n > 255 || (count == 0 && (n == 0 || n == 127 || n >= 224))) return "";
    ++count;
  }
  if (count != 4 || localIp.back() == '.' || serverBase.empty()) return "";
  std::string base = serverBase;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/vod_song/mobile?" + localIp;
}

namespace {
using Color = std::array<unsigned char, 4>;
Color parseColor(const std::string& value, Color fallback) {
  if (value.size() == 7 && value[0] == '#' &&
      value.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos) {
    for (int i = 0; i < 3; ++i)
      fallback[i] = static_cast<unsigned char>(std::stoul(value.substr(1 + i * 2, 2), nullptr, 16));
    fallback[3] = 255;
    return fallback;
  }
  std::istringstream in(value);
  float r, g, b, a = 1;
  if (!(in >> r >> g >> b)) return fallback;
  in >> a;
  const float channels[]{r, g, b, a};
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(channels[i])) return fallback;
    fallback[i] = static_cast<unsigned char>(std::lround(std::clamp(channels[i], 0.f, 1.f) * 255));
  }
  return fallback;
}
struct Font {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  ~Font() { if (face) FT_Done_Face(face); if (library) FT_Done_FreeType(library); }
};
std::vector<uint32_t> decodeCaption(const std::string& text) {
  std::vector<uint32_t> result;
  for (size_t i = 0; i < text.size();) {
    const unsigned char first = text[i++];
    const int extra = first < 0x80 ? 0 : first >= 0xC2 && first <= 0xDF ? 1 :
                      first >= 0xE0 && first <= 0xEF ? 2 : first >= 0xF0 && first <= 0xF4 ? 3 : -1;
    if (extra < 0 || i + extra > text.size()) throw std::invalid_argument("Invalid UTF-8 caption");
    uint32_t code = extra == 0 ? first : first & ((1 << (6 - extra)) - 1);
    for (int j = 0; j < extra; ++j) {
      const unsigned char next = text[i++];
      if ((next & 0xC0) != 0x80) throw std::invalid_argument("Invalid UTF-8 caption");
      code = (code << 6) | (next & 0x3F);
    }
    if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
        (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
      throw std::invalid_argument("Invalid UTF-8 caption");
    result.push_back(code);
  }
  return result;
}
} // namespace

bool writeQrCodeCard(const LayerConfigData& config, const std::string& fontPath,
                     const std::string& outputPath) {
  try {
    if (config.qrContent.empty()) return false;
    using qrcodegen::QrCode;
    const auto qr = QrCode::encodeText(config.qrContent.c_str(),
        static_cast<QrCode::Ecc>(std::clamp(config.qrErrorCorrection, 0, 3)));
    const int width = std::clamp(config.qrSize, 128, 2048);
    const int padding = std::lround(width * .04);
    const int box = width - padding * 2;
    const int height = width + std::lround(width * .16) + std::lround(width * .02);
    const Color bg = parseColor(config.qrBgColor, {0, 51, 255, 255});
    const Color fg = parseColor(config.qrFgColor, {0, 0, 0, 255});
    const Color textColor = parseColor(config.qrTextColor, {255, 255, 255, 255});
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
    auto put = [&](int x, int y, Color c) {
      if (x < 0 || y < 0 || x >= width || y >= height) return;
      std::copy(c.begin(), c.end(), pixels.begin() + (static_cast<size_t>(y) * width + x) * 4);
    };
    auto rounded = [&](int left, int top, int w, int h, int radius, Color color) {
      for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int dx = std::max({radius - x, x - (w - radius - 1), 0});
        const int dy = std::max({radius - y, y - (h - radius - 1), 0});
        if (dx * dx + dy * dy <= radius * radius) put(left + x, top + y, color);
      }
    };
    rounded(0, 0, width, height, std::lround(width * .06), bg);
    rounded(padding, padding, box, box, padding, {255, 255, 255, 255});
    // Keep a 10-pixel white inset, with all module edges aligned to pixels.
    // Rounding the scale down leaves an unnecessarily wide white border.
    constexpr int margin = 10;
    const int codeSize = box - margin * 2;
    if (codeSize < qr.getSize()) return false;
    const auto edge = [&](int module) {
      return padding + margin + static_cast<int>(std::lround(static_cast<double>(module) * codeSize / qr.getSize()));
    };
    for (int y = 0; y < qr.getSize(); ++y) for (int x = 0; x < qr.getSize(); ++x) {
      if (!qr.getModule(x, y)) continue;
      for (int yy = edge(y); yy < edge(y + 1); ++yy)
        for (int xx = edge(x); xx < edge(x + 1); ++xx) put(xx, yy, fg);
    }
    Font font;
    if (FT_Init_FreeType(&font.library) || FT_New_Face(font.library, fontPath.c_str(), 0, &font.face)) return false;
    const auto caption = decodeCaption(
        config.qrText.empty() ? "微信扫码关注" : config.qrText);
    int fontSize = std::lround(width * .12);
    int advance = 0;
    do {
      FT_Set_Pixel_Sizes(font.face, 0, fontSize);
      advance = 0;
      for (char32_t ch : caption) {
        if (!FT_Load_Char(font.face, ch, FT_LOAD_DEFAULT)) advance += font.face->glyph->advance.x >> 6;
      }
      if (advance <= box || fontSize <= 6) break;
      --fontSize;
    } while (true);
    int pen = (width - advance) / 2;
    const int baseline = width - padding + (height - width + padding + fontSize) / 2;
    for (char32_t ch : caption) {
      if (FT_Load_Char(font.face, ch, FT_LOAD_RENDER)) continue;
      const auto glyph = font.face->glyph;
      for (unsigned y = 0; y < glyph->bitmap.rows; ++y) for (unsigned x = 0; x < glyph->bitmap.width; ++x) {
        const int px = pen + glyph->bitmap_left + static_cast<int>(x);
        const int py = baseline - glyph->bitmap_top + static_cast<int>(y);
        if (px < 0 || px >= width || py < width - padding || py >= height) continue;
        const auto coverage = glyph->bitmap.buffer[y * glyph->bitmap.pitch + x];
        const float alpha = coverage / 255.f * textColor[3] / 255.f;
        auto* dst = &pixels[(static_cast<size_t>(py) * width + px) * 4];
        const float outAlpha = alpha + dst[3] / 255.f * (1 - alpha);
        if (outAlpha > 0) for (int c = 0; c < 3; ++c)
          dst[c] = std::lround((textColor[c] * alpha + dst[c] * dst[3] / 255.f * (1 - alpha)) / outAlpha);
        dst[3] = std::lround(outAlpha * 255);
      }
      pen += glyph->advance.x >> 6;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    const bool ok = png_image_write_to_file(&image, outputPath.c_str(), 0, pixels.data(), 0, nullptr) != 0;
    png_image_free(&image);
    return ok;
  } catch (const std::exception&) { return false; }
}
} // namespace hsvj
