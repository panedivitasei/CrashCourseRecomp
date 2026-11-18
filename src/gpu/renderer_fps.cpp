// rexvideonative — FPS overlay rasterizer (see renderer_fps.h).
// GPL-3.0 — see LICENSE in this directory.

#include "renderer_fps.h"

// JPEG decode for CC1's domyouji font pages, a JPEG-only static copy so this TU
// does not depend on xam's linked implementation.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"  // SDK thirdparty/stb (see CMakeLists.txt)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gpu_fps, false, "GPU",
                    "Shows an FPS counter in the top-left corner while the "
                    "native video layer presents.");

namespace rex::videonative::fps {
namespace {

// ---- builtin 5x7 fallback (titles without the cardigan atlas) --------------
// One byte per row with the low 5 bits used (bit 4 = leftmost column), covering
// digits, '.', ' ' and "FPS".
struct BuiltinGlyph {
  char ch;
  uint8_t rows[7];
};
constexpr BuiltinGlyph kBuiltinFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
};

const uint8_t* BuiltinRows(char ch) {
  for (const BuiltinGlyph& g : kBuiltinFont) {
    if (g.ch == ch) return g.rows;
  }
  return nullptr;  // unknown chars render as space
}

constexpr uint32_t kMaxChars = 9;  // "999.9 FPS"
constexpr uint32_t kGlyphW = 5, kGlyphH = 7;
constexpr uint32_t kAdvance = kGlyphW + 1;
constexpr uint32_t kPad = 2;
constexpr uint32_t kFbWidth = kMaxChars * kAdvance - 1 + 2 * kPad;  // 55
constexpr uint32_t kFbHeight = kGlyphH + 2 * kPad;                  // 11

// ---- cardigan_bd (the game's BMFont: text metrics + DXT5 atlas page) -------
// Glyphs the counter needs (digits, '.', F, P, S) live on page 1
// (cardigan_bd_1.dds), white with an alpha channel, and are drawn downscaled
// with bilinear atlas sampling.
struct CardiganGlyph {
  int x = 0, y = 0, w = 0, h = 0, xo = 0, yo = 0, xa = 0, page = 0;
};
std::map<int, CardiganGlyph> g_glyphs;
float g_fontLine = 59.0f;
uint32_t g_atlasW = 0, g_atlasH = 0;
std::vector<uint8_t> g_atlas;  // decoded page RGBA (alpha = coverage)
// BMFont page index holding the drawable glyphs: 1 for cardigan, 0 for CC1's
// single-page domyouji.
int g_atlasGlyphPage = 1;

// Target text height in final pixels.
constexpr float kTextPx = 28.0f;
constexpr uint32_t kCPad = 3;
constexpr uint32_t kCWidth = 128;
constexpr uint32_t kCHeight = 36;

void DecodeBc3Block(const uint8_t* b, uint8_t out[16][4]) {
  const uint8_t a0 = b[0], a1 = b[1];
  uint64_t aidx = 0;
  for (int i = 0; i < 6; i++) aidx |= uint64_t(b[2 + i]) << (8 * i);
  uint8_t atab[8];
  atab[0] = a0;
  atab[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; i++) {
      atab[1 + i] = uint8_t(((7 - i) * a0 + i * a1) / 7);
    }
  } else {
    for (int i = 1; i < 5; i++) {
      atab[1 + i] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    }
    atab[6] = 0;
    atab[7] = 255;
  }
  const uint16_t c0 = uint16_t(b[8] | (b[9] << 8));
  const uint16_t c1 = uint16_t(b[10] | (b[11] << 8));
  uint8_t ctab[4][3];
  auto expand565 = [](uint16_t c, uint8_t* o) {
    o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31);
  };
  expand565(c0, ctab[0]);
  expand565(c1, ctab[1]);
  for (int k = 0; k < 3; k++) {
    ctab[2][k] = uint8_t((2 * ctab[0][k] + ctab[1][k]) / 3);
    ctab[3][k] = uint8_t((ctab[0][k] + 2 * ctab[1][k]) / 3);
  }
  const uint32_t cidx = uint32_t(b[12]) | (uint32_t(b[13]) << 8) |
                        (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
  for (int p = 0; p < 16; p++) {
    const uint8_t* c = ctab[(cidx >> (2 * p)) & 3];
    out[p][0] = c[0];
    out[p][1] = c[1];
    out[p][2] = c[2];
    out[p][3] = atab[(aidx >> (3 * p)) & 7];
  }
}

bool LoadCardigan() {
  const std::string root = rex::cvar::Query<std::string>("game_data_root");
  if (root.empty()) return false;
  const std::string base = root + "/data/art/common/ui/fnt/";

  std::ifstream fnt(base + "cardigan_bd.fnt");
  if (!fnt) return false;
  std::string ln;
  while (std::getline(fnt, ln)) {
    if (ln.rfind("char id=", 0) == 0) {
      CardiganGlyph g;
      int id = 0;
      if (std::sscanf(ln.c_str(),
                      "char id=%d x=%d y=%d width=%d height=%d xoffset=%d "
                      "yoffset=%d xadvance=%d page=%d",
                      &id, &g.x, &g.y, &g.w, &g.h, &g.xo, &g.yo, &g.xa,
                      &g.page) >= 9) {
        g_glyphs[id] = g;
      }
    } else if (ln.rfind("common ", 0) == 0) {
      const auto p = ln.find("lineHeight=");
      if (p != std::string::npos) {
        g_fontLine = float(std::atoi(ln.c_str() + p + 11));
      }
    }
  }
  if (g_glyphs.find('0') == g_glyphs.end() || g_fontLine <= 0.0f) {
    return false;
  }

  std::ifstream dds(base + "cardigan_bd_1.dds", std::ios::binary);
  if (!dds) return false;
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(dds)),
                         std::istreambuf_iterator<char>());
  // DDS: magic + 124-byte header; DXT5 fourCC at offset 84, dims at 12/16.
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0 ||
      std::memcmp(d.data() + 84, "DXT5", 4) != 0) {
    return false;
  }
  auto rd32 = [&](size_t off) {
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
  };
  const uint32_t h = rd32(12), w = rd32(16);
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  if (w == 0 || h == 0 || d.size() < 128 + size_t(bw) * bh * 16) {
    return false;
  }
  g_atlasW = w;
  g_atlasH = h;
  g_atlas.assign(size_t(w) * h * 4, 0);
  const uint8_t* src = d.data() + 128;
  for (uint32_t by = 0; by < bh; by++) {
    for (uint32_t bx = 0; bx < bw; bx++, src += 16) {
      uint8_t texels[16][4];
      DecodeBc3Block(src, texels);
      for (uint32_t p = 0; p < 16; p++) {
        const uint32_t px = bx * 4 + (p & 3), py = by * 4 + (p >> 2);
        if (px >= w || py >= h) continue;
        std::memcpy(&g_atlas[(size_t(py) * w + px) * 4], texels[p], 4);
      }
    }
  }
  return true;
}

// CC1's domyouji font (assets/data/art/fonts): the same BMFont .fnt convention
// as cardigan with a single page-0 atlas shipped as JPEG, of which only the
// coverage mask domyouji_a.jpg is loaded.
bool LoadDomyouji() {
  const std::string root = rex::cvar::Query<std::string>("game_data_root");
  if (root.empty()) return false;
  const std::string base = root + "/data/art/fonts/";

  std::ifstream fnt(base + "domyouji.fnt");
  if (!fnt) return false;
  std::string ln;
  while (std::getline(fnt, ln)) {
    if (ln.rfind("char id=", 0) == 0) {
      CardiganGlyph g;
      int id = 0;
      if (std::sscanf(ln.c_str(),
                      "char id=%d x=%d y=%d width=%d height=%d xoffset=%d "
                      "yoffset=%d xadvance=%d page=%d",
                      &id, &g.x, &g.y, &g.w, &g.h, &g.xo, &g.yo, &g.xa,
                      &g.page) >= 9) {
        g_glyphs[id] = g;
      }
    } else if (ln.rfind("common ", 0) == 0) {
      const auto p = ln.find("lineHeight=");
      if (p != std::string::npos) {
        g_fontLine = float(std::atoi(ln.c_str() + p + 11));
      }
    }
  }
  if (g_glyphs.find('0') == g_glyphs.end() || g_fontLine <= 0.0f) {
    return false;
  }

  int w = 0, h = 0, comp = 0;
  stbi_uc* mask =
      stbi_load((base + "domyouji_a.jpg").c_str(), &w, &h, &comp, 1);
  if (!mask) return false;
  g_atlasW = uint32_t(w);
  g_atlasH = uint32_t(h);
  g_atlas.assign(size_t(w) * h * 4, 255);
  for (size_t i = 0; i < size_t(w) * h; i++) {
    g_atlas[i * 4 + 3] = mask[i];  // coverage from the mask's luminance
  }
  stbi_image_free(mask);
  g_atlasGlyphPage = 0;
  return true;
}

// Bilinear alpha sample of the decoded atlas.
float AtlasAlpha(float x, float y) {
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  auto at = [&](int sx, int sy) -> float {
    if (sx < 0 || sy < 0 || sx >= int(g_atlasW) || sy >= int(g_atlasH)) {
      return 0.0f;
    }
    return float(g_atlas[(size_t(sy) * g_atlasW + sx) * 4 + 3]) / 255.0f;
  };
  const float top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
  const float bot = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1 - fy) + bot * fy;
}

// ---- shared strip state ----------------------------------------------------
bool g_fontTried = false;
bool g_cardigan = false;
uint32_t g_width = 0, g_height = 0;
uint32_t g_usedWidth = 0;
std::vector<uint8_t> g_pixels;
std::vector<float> g_cov;  // glyph coverage, composited by FinalizeOutline
std::string g_lastText;

// Turns the coverage buffer into the final RGBA strip: transparent background,
// black outline (coverage dilated by `radius`) and white core.
void FinalizeOutline(int radius) {
  for (int y = 0; y < int(g_height); y++) {
    for (int x = 0; x < int(g_width); x++) {
      const float core = g_cov[size_t(y) * g_width + x];
      float dilated = core;
      for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
          const int sx = x + dx, sy = y + dy;
          if (sx < 0 || sy < 0 || sx >= int(g_width) || sy >= int(g_height)) {
            continue;
          }
          dilated = std::max(dilated, g_cov[size_t(sy) * g_width + sx]);
        }
      }
      uint8_t* px = &g_pixels[(size_t(y) * g_width + x) * 4];
      const float alpha = std::max(core, dilated * 0.85f);
      if (alpha <= 0.004f) {
        px[0] = px[1] = px[2] = px[3] = 0;
        continue;
      }
      const uint8_t white = uint8_t(std::lround(255.0f * (core / alpha)));
      px[0] = px[1] = px[2] = white;
      px[3] = uint8_t(std::lround(255.0f * alpha));
    }
  }
}

// Left-aligned so the leading edge stays put as the digit count changes.
void RasterizeCardigan(const std::string& text) {
  std::fill(g_cov.begin(), g_cov.end(), 0.0f);
  const float sc = kTextPx / g_fontLine;
  float x = float(kCPad);
  for (char ch : text) {
    const auto it = g_glyphs.find(int(uint8_t(ch)));
    if (it == g_glyphs.end()) {
      x += kTextPx * 0.3f;
      continue;
    }
    const CardiganGlyph& g = it->second;
    if (g.w > 0 && g.page == g_atlasGlyphPage && !g_atlas.empty()) {
      const float x0 = x + float(g.xo) * sc;
      const float y0 = float(kCPad) + float(g.yo) * sc;
      const int ow = std::max(1, int(std::lround(float(g.w) * sc)));
      const int oh = std::max(1, int(std::lround(float(g.h) * sc)));
      for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
          const int dx = int(std::lround(x0)) + ox;
          const int dy = int(std::lround(y0)) + oy;
          if (dx < 0 || dy < 0 || dx >= int(g_width) || dy >= int(g_height)) {
            continue;
          }
          const float a =
              AtlasAlpha(float(g.x) + (float(ox) + 0.5f) / sc - 0.5f,
                         float(g.y) + (float(oy) + 0.5f) / sc - 0.5f);
          if (a <= 0.0f) continue;
          float& cov = g_cov[size_t(dy) * g_width + dx];
          cov = std::max(cov, a);
        }
      }
    }
    x += float(g.xa) * sc;
  }
  g_usedWidth = std::min(g_width, uint32_t(std::lround(x)) + kCPad);
  FinalizeOutline(2);
}

void RasterizeBuiltin(const std::string& text) {
  std::fill(g_cov.begin(), g_cov.end(), 0.0f);
  const uint32_t count = std::min<uint32_t>(uint32_t(text.size()), kMaxChars);
  uint32_t x = kPad;
  for (uint32_t c = 0; c < count; c++, x += kAdvance) {
    const uint8_t* rows = BuiltinRows(text[c]);
    if (!rows) continue;
    for (uint32_t gy = 0; gy < kGlyphH; gy++) {
      for (uint32_t gx = 0; gx < kGlyphW; gx++) {
        if (!(rows[gy] & (0x10 >> gx))) continue;
        g_cov[(kPad + gy) * g_width + x + gx] = 1.0f;
      }
    }
  }
  g_usedWidth = std::min(g_width, count * kAdvance - 1 + 2 * kPad);
  FinalizeOutline(1);
}

}  // namespace

Overlay Tick() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  static double ema_ms = 0.0;

  const Clock::time_point now = Clock::now();
  const double dt_ms =
      std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  // EMA over swap-to-swap time, with startup and pause outliers dropped so the
  // readout recovers in a few frames.
  if (dt_ms > 0.0 && dt_ms < 1000.0) {
    ema_ms = ema_ms == 0.0 ? dt_ms : ema_ms * 0.95 + dt_ms * 0.05;
  }

  Overlay out;
  if (!REXCVAR_GET(gpu_fps) || ema_ms <= 0.0) return out;

  // The first visible tick latches the layout, since the overlay texture is
  // created once at these dimensions.
  if (!g_fontTried) {
    g_fontTried = true;
    g_cardigan = LoadCardigan();
    if (!g_cardigan) g_cardigan = LoadDomyouji();
    g_width = g_cardigan ? kCWidth : kFbWidth;
    g_height = g_cardigan ? kCHeight : kFbHeight;
    g_pixels.assign(size_t(g_width) * g_height * 4, 0);
    g_cov.assign(size_t(g_width) * g_height, 0.0f);
  }

  char text[16];
  const double fps_value = 1000.0 / ema_ms;
  if (fps_value >= 99.95) {
    std::snprintf(text, sizeof(text), "%.0f FPS", fps_value);
  } else {
    std::snprintf(text, sizeof(text), "%.1f FPS", fps_value);
  }
  out.visible = true;
  out.width = g_width;
  out.height = g_height;
  out.pixels = g_pixels.data();
  out.scale = g_cardigan ? 1.0f : 1.5f;
  out.dirty = g_lastText != text;
  if (out.dirty) {
    g_lastText = text;
    if (g_cardigan) {
      RasterizeCardigan(g_lastText);
    } else {
      RasterizeBuiltin(g_lastText);
    }
  }
  out.used_width = g_usedWidth ? g_usedWidth : g_width;
  return out;
}

}  // namespace rex::videonative::fps
