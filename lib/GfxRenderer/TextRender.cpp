#include "TextRender.h"

#include <Utf8.h>

#include <algorithm>
#include <string>

#include "GfxRenderer.h"

// The firmware builds with -Os; glyph blitting is the per-pixel hot loop of every text draw, so (like
// JpegRender.cpp) opt this translation unit into -O2. Placed after the includes so it doesn't apply to inlined
// header code pulled in above.
#pragma GCC optimize("O2")

namespace {

constexpr uint8_t kSmallCapsScalePct = 77;
constexpr int kScaleRoundBias = 50;

bool isAsciiLower(const uint32_t cp) { return cp >= 'a' && cp <= 'z'; }

uint32_t toAsciiUpper(const uint32_t cp) { return isAsciiLower(cp) ? (cp - ('a' - 'A')) : cp; }

void appendUtf8Codepoint(std::string& out, const uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string toUpperUtf8(const char* text) {
  std::string upper;
  if (!text) {
    return upper;
  }
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text);
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    appendUtf8Codepoint(upper, toAsciiUpper(cp));
  }
  return upper;
}

int scaleMetricRound(const int value, const uint8_t scalePct) {
  return std::max(1, (value * static_cast<int>(scalePct) + kScaleRoundBias) / 100);
}

uint8_t mediumTextCodeForLevel(const uint8_t level, const bool x3) {
  static constexpr uint8_t kX4Codes[4] = {
      0b00,  // white
      0b10,  // dark gray
      0b11,  // light gray
      0b01,  // black
  };
  static constexpr uint8_t kX3Codes[4] = {
      0b00,  // white
      0b11,  // dark gray
      0b10,  // light gray
      0b01,  // black
  };
  return x3 ? kX3Codes[level & 3u] : kX4Codes[level & 3u];
}

uint8_t imageLevelFromFontBmpVal(const uint8_t bmpVal) {
  switch (bmpVal & 3u) {
    case 0:
      return 3;  // solid black
    case 1:
      return 1;  // dark gray
    case 2:
      return 2;  // light gray
    default:
      return 0;  // white
  }
}

bool mediumTextPlaneShouldClear(const GfxRenderer& gfx, const uint8_t level) {
  const uint8_t code = mediumTextCodeForLevel(level, gfx.deviceIsX3());
  if (gfx.getRenderMode() == GfxRenderer::GRAYSCALE_LSB) {
    return (code & 0b01u) != 0;
  }
  if (gfx.getRenderMode() == GfxRenderer::GRAYSCALE_MSB) {
    return (code & 0b10u) != 0;
  }
  return false;
}

const EpdFontFamily* findFontFamily(const GfxRenderer& gfx, const int fontId) { return gfx.findFontFamily(fontId); }

ExternalFont* findStreamingFont(const GfxRenderer& gfx, const EpdFontData* data) { return gfx.findStreamingFont(data); }

}  // namespace

int TextRender::getWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  const auto& family = (*findFontFamily(gfx, fontId));
  if (findStreamingFont(gfx, family.getData(style)) != nullptr) {
    return getStreamingTextWidth(family, text, style);
  }
  int w = 0;
  int h = 0;
  family.getTextDimensions(text, &w, &h, style);
  return w;
}

int TextRender::getHeight(const int fontId) const { return getLineHeight(fontId); }

int TextRender::getFontAscenderSize(const int fontId) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  return (*findFontFamily(gfx, fontId)).getData(EpdFontFamily::REGULAR)->ascender;
}

int TextRender::getGlyphTopInset(const int fontId, const uint32_t cp, const EpdFontFamily::Style style) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    return 0;
  }
  const auto& family = (*findFontFamily(gfx, fontId));
  const EpdFontData* data = family.getData(style);
  if (!data) {
    return 0;
  }
  EpdGlyph storage;
  const EpdGlyph* glyph = nullptr;
  ExternalFont* streamIt = findStreamingFont(gfx, data);
  if (streamIt) {
    if (streamIt->getGlyphMetadata(cp, storage)) {
      glyph = &storage;
    }
  } else {
    glyph = family.getGlyph(cp, style);
  }
  if (!glyph) {
    return 0;
  }
  return data->ascender - glyph->top;
}

int TextRender::getGlyphBottomInset(const int fontId, const uint32_t cp, const EpdFontFamily::Style style) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    return 0;
  }
  const auto& family = (*findFontFamily(gfx, fontId));
  const EpdFontData* data = family.getData(style);
  if (!data) {
    return 0;
  }
  EpdGlyph storage;
  const EpdGlyph* glyph = nullptr;
  ExternalFont* streamIt = findStreamingFont(gfx, data);
  if (streamIt) {
    if (streamIt->getGlyphMetadata(cp, storage)) {
      glyph = &storage;
    }
  } else {
    glyph = family.getGlyph(cp, style);
  }
  if (!glyph) {
    return 0;
  }
  const int glyphBottom = data->ascender - glyph->top + glyph->height;
  return std::max(0, static_cast<int>(data->advanceY) - glyphBottom);
}

int TextRender::getLineHeight(const int fontId) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  return (*findFontFamily(gfx, fontId)).getData(EpdFontFamily::REGULAR)->advanceY;
}

int TextRender::getSpaceWidth(const int fontId) const {
  if (findFontFamily(gfx, fontId) == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  const EpdFontFamily& font = (*findFontFamily(gfx, fontId));
  const EpdFontData* fontData = font.getData(EpdFontFamily::REGULAR);
  if (!fontData) {
    return 0;
  }

  ExternalFont* streamIt = findStreamingFont(gfx, fontData);
  if (streamIt) {
    EpdGlyph g{};
    constexpr uint32_t kSpace = 0x20;
    if (!streamIt->getGlyphMetadata(kSpace, g)) {
      if (!streamIt->getGlyphMetadata(REPLACEMENT_GLYPH, g)) {
        return 0;
      }
    }
    return g.advanceX;
  }

  const EpdGlyph* glyph = font.getGlyph(' ', EpdFontFamily::REGULAR);
  if (!glyph) {
    glyph = font.getGlyph(REPLACEMENT_GLYPH, EpdFontFamily::REGULAR);
  }
  return glyph ? glyph->advanceX : 0;
}

bool TextRender::supportsAntiAliasing(const int fontId) const {
  const EpdFontFamily* family = findFontFamily(gfx, fontId);
  if (!family) {
    return false;
  }

  const EpdFontData* regular = family->getData(EpdFontFamily::REGULAR);
  if (!regular || !regular->is2Bit) {
    return false;
  }

  ExternalFont* stream = findStreamingFont(gfx, regular);
  if (stream) {
    return stream->hasAntiAliasData();
  }

  return true;
}

int TextRender::getSmallCapsWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (!text || *text == '\0' || findFontFamily(gfx, fontId) == nullptr) {
    return 0;
  }

  const auto& family = (*findFontFamily(gfx, fontId));
  const EpdFontData* fontData = family.getData(style);
  if (!fontData) {
    return 0;
  }

  ExternalFont* streamIt = findStreamingFont(gfx, fontData);
  const std::string upper = toUpperUtf8(text);
  const char* ptr = upper.c_str();
  int totalWidth = 0;
  while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr))) {
    EpdGlyph glyphStorage;
    const EpdGlyph* glyph = nullptr;
    if (streamIt) {
      if (!streamIt->getGlyphMetadata(cp, glyphStorage)) {
        streamIt->getGlyphMetadata(REPLACEMENT_GLYPH, glyphStorage);
      }
      glyph = &glyphStorage;
    } else {
      glyph = family.getGlyph(cp, style);
      if (!glyph) {
        glyph = family.getGlyph(REPLACEMENT_GLYPH, style);
      }
    }
    if (!glyph) {
      continue;
    }
    totalWidth += scaleMetricRound(glyph->advanceX, kSmallCapsScalePct);
  }
  return totalWidth;
}

int TextRender::getScaledWidth(const int fontId, const char* text, const uint8_t scalePct,
                               const EpdFontFamily::Style style) const {
  if (!text || *text == '\0' || findFontFamily(gfx, fontId) == nullptr) {
    return 0;
  }

  const auto& family = (*findFontFamily(gfx, fontId));
  const EpdFontData* fontData = family.getData(style);
  if (!fontData) {
    return 0;
  }

  ExternalFont* streamIt = findStreamingFont(gfx, fontData);
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text);
  int totalWidth = 0;
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    EpdGlyph glyphStorage;
    const EpdGlyph* glyph = nullptr;
    if (streamIt) {
      if (!streamIt->getGlyphMetadata(cp, glyphStorage)) {
        streamIt->getGlyphMetadata(REPLACEMENT_GLYPH, glyphStorage);
      }
      glyph = &glyphStorage;
    } else {
      glyph = family.getGlyph(cp, style);
      if (!glyph) {
        glyph = family.getGlyph(REPLACEMENT_GLYPH, style);
      }
    }
    if (!glyph) {
      continue;
    }
    totalWidth += scaleMetricRound(glyph->advanceX, scalePct);
  }
  return totalWidth;
}

std::string TextRender::truncate(const int fontId, const char* text, const int maxWidth,
                                 const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  const char* ellipsis = "...";
  int textWidth = getWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    return item;
  }

  while (!item.empty() && getWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

// ---- Glyph blitting -------------------------------------------------------------------------------------------
//
// Every glyph pixel used to go through GfxRenderer::drawPixel() (re-fetching the framebuffer, switching on the
// orientation, bounds-checking) plus a couple of render-mode helper calls. Now, per render call, the per-sample
// decision (ink / paper / nothing) is precomputed into a tiny LUT and the logical->panel mapping is resolved
// once; per glyph the rect is clipped once and fully-on-screen glyphs are written straight into the framebuffer
// with incremental panel coordinates. Partially clipped glyphs (rare) keep using drawPixel() so their behaviour,
// including its out-of-range logging, is unchanged.

struct TextGlyphTarget {
  const GfxRenderer* gfx;
  uint8_t* frameBuffer;  // null -> every pixel goes through the checked drawPixel() path
  GfxRenderer::PanelMapping map;
  uint8_t lut2[4];  // raw 2-bit sample -> PixelAction
  uint8_t lut1[2];  // 1-bit sample -> PixelAction
};

struct TextRender::WidthBounds {
  int startX;      // pen x at the start of the run
  int minX;        // flash fonts: bounds relative to startX, exactly as EpdFont::getTextBounds() tracks them
  int maxX;        //
  int advanceSum;  // streaming fonts: sum of advances, exactly as getStreamingTextWidth() computes it
};

namespace {

enum PixelAction : uint8_t {
  kSkip = 0,
  kInk = 1,    // drawPixel(x, y, true): clear the bit
  kPaper = 2,  // drawPixel(x, y, false): set the bit
};

uint8_t mediumPlaneAction(const GfxRenderer& gfx, const uint8_t level) {
  return mediumTextPlaneShouldClear(gfx, level) ? kPaper : kSkip;
}

/** Action equivalent of renderSolidTextPixel(). */
uint8_t solidAction(const GfxRenderer& gfx, const bool state) {
  if (gfx.getRenderMode() == GfxRenderer::BW) {
    return state ? kInk : kPaper;
  }
  return mediumPlaneAction(gfx, state ? 3 : 0);
}

/**
 * Precomputes, for the current render mode, what the old per-pixel code did for each sample value.
 * rotatedRules selects rotated90CW()'s 2-bit rules (it has no "!pixelState" branch).
 */
void initTarget(TextGlyphTarget& t, const GfxRenderer& gfx, const bool pixelState, const bool rotatedRules) {
  t.gfx = &gfx;
  t.frameBuffer = gfx.getFrameBuffer();
  t.map = gfx.getPanelMapping();
  const bool bw = gfx.getRenderMode() == GfxRenderer::BW;
  for (uint8_t raw = 0; raw < 4; ++raw) {
    const uint8_t bmpVal = 3 - raw;
    uint8_t action;
    if (!rotatedRules && !pixelState && bmpVal < 3) {
      action = solidAction(gfx, false);
    } else if (bw && bmpVal < 3) {
      action = pixelState ? kInk : kPaper;
    } else {
      action = mediumPlaneAction(gfx, imageLevelFromFontBmpVal(bmpVal));
    }
    t.lut2[raw] = action;
  }
  t.lut1[0] = kSkip;
  t.lut1[1] = solidAction(gfx, pixelState);
}

inline void applyAction(uint8_t* fb, const GfxRenderer::PanelMapping& m, const int px, const int py,
                        const uint8_t action) {
  uint8_t* p = fb + py * m.widthBytes + (px >> 3);
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (px & 7));
  if (action == kInk) {
    *p &= static_cast<uint8_t>(~mask);
  } else {
    *p |= mask;
  }
}

/** True when the logical rect [x0..x1] x [y0..y1] (inclusive) is fully on screen and the framebuffer exists. */
inline bool rectOnScreen(const TextGlyphTarget& t, const int x0, const int y0, const int x1, const int y1) {
  return t.frameBuffer != nullptr && x0 >= 0 && y0 >= 0 && x1 < t.map.screenWidth && y1 < t.map.screenHeight;
}

/** Single logical pixel; `fast` must only be true if the pixel is known to be on screen. */
inline void plotLogical(const TextGlyphTarget& t, const int lx, const int ly, const uint8_t action, const bool fast) {
  if (action == kSkip) {
    return;
  }
  if (!fast) {
    t.gfx->drawPixel(lx, ly, action == kInk);
    return;
  }
  const GfxRenderer::PanelMapping& m = t.map;
  applyAction(t.frameBuffer, m, m.originX + lx * m.xStepX + ly * m.yStepX, m.originY + lx * m.xStepY + ly * m.yStepY,
              action);
}

template <bool kTwoBit>
inline uint8_t readSample(const uint8_t* bits, const int pos) {
  if (kTwoBit) {
    return (bits[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3u;
  }
  return (bits[pos >> 3] >> (7 - (pos & 7))) & 0x1u;
}

/**
 * Blits a packed glyph bitmap whose samples are contiguous (sample (gx, gy) at index gy * width + gx). Glyph
 * pixel (gx, gy) lands on logical (ox + gx * lxx + gy * lxy, oy + gx * lyx + gy * lyy).
 */
template <bool kTwoBit>
void blitGlyph(const TextGlyphTarget& t, const uint8_t* bits, const int width, const int height, const int ox,
               const int oy, const int lxx, const int lyx, const int lxy, const int lyy) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const uint8_t* lut = kTwoBit ? t.lut2 : t.lut1;

  // The mapping is axis-aligned, so two opposite corners give the logical bounding box.
  const int cx = ox + (width - 1) * lxx + (height - 1) * lxy;
  const int cy = oy + (width - 1) * lyx + (height - 1) * lyy;
  if (!rectOnScreen(t, std::min(ox, cx), std::min(oy, cy), std::max(ox, cx), std::max(oy, cy))) {
    int pos = 0;
    for (int gy = 0; gy < height; gy++) {
      for (int gx = 0; gx < width; gx++, pos++) {
        const uint8_t action = lut[readSample<kTwoBit>(bits, pos)];
        if (action != kSkip) {
          t.gfx->drawPixel(ox + gx * lxx + gy * lxy, oy + gx * lyx + gy * lyy, action == kInk);
        }
      }
    }
    return;
  }

  const GfxRenderer::PanelMapping& m = t.map;
  uint8_t* fb = t.frameBuffer;
  // Panel-space deltas for one step along the glyph's x and y.
  const int pxPerGx = lxx * m.xStepX + lyx * m.yStepX;
  const int pyPerGx = lxx * m.xStepY + lyx * m.yStepY;
  const int pxPerGy = lxy * m.xStepX + lyy * m.yStepX;
  const int pyPerGy = lxy * m.xStepY + lyy * m.yStepY;
  int rowPx = m.originX + ox * m.xStepX + oy * m.yStepX;
  int rowPy = m.originY + ox * m.xStepY + oy * m.yStepY;
  int pos = 0;
  for (int gy = 0; gy < height; gy++) {
    int px = rowPx;
    int py = rowPy;
    for (int gx = 0; gx < width; gx++, pos++) {
      const uint8_t action = lut[readSample<kTwoBit>(bits, pos)];
      if (action != kSkip) {
        applyAction(fb, m, px, py, action);
      }
      px += pxPerGx;
      py += pyPerGx;
    }
    rowPx += pxPerGy;
    rowPy += pyPerGy;
  }
}

/** Upright glyph (or one row of it) at logical (ox, oy). */
inline void blitUpright(const TextGlyphTarget& t, const bool is2Bit, const uint8_t* bits, const int width,
                        const int height, const int ox, const int oy) {
  if (is2Bit) {
    blitGlyph<true>(t, bits, width, height, ox, oy, 1, 0, 0, 1);
  } else {
    blitGlyph<false>(t, bits, width, height, ox, oy, 1, 0, 0, 1);
  }
}

}  // namespace

void TextRender::rotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                             const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') {
    return;
  }
  const EpdFontFamily* family = findFontFamily(gfx, fontId);
  if (family == nullptr) {
    return;
  }
  const EpdFontFamily& font = *family;
  // (No hasPrintableChars() pre-pass: a run it rejects consists only of zero-size glyphs, which draw nothing.)

  const EpdFontData* fontData = font.getData(style);
  if (!fontData) {
    return;
  }
  ExternalFont* it = findStreamingFont(gfx, fontData);
  TextGlyphTarget target;
  initTarget(target, gfx, black, /*rotatedRules=*/true);
  int yPos = y;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    EpdGlyph glyphStorage;
    const EpdGlyph* glyph = nullptr;

    if (it) {
      // Neither the codepoint nor U+FFFD in the font: skip it (as getWidth() does) instead of drawing whatever
      // was left in the uninitialized glyphStorage.
      if (!it->getGlyphMetadata(cp, glyphStorage) && !it->getGlyphMetadata(REPLACEMENT_GLYPH, glyphStorage)) {
        continue;
      }
      glyph = &glyphStorage;
    } else {
      glyph = font.getGlyph(cp, style);
      if (!glyph) {
        glyph = font.getGlyph(REPLACEMENT_GLYPH, style);
      }
    }

    if (!glyph) {
      continue;
    }

    uint8_t localStackBuffer[1024];
    const uint8_t* bitmap = nullptr;

    if (fontData->bitmap != nullptr) {
      bitmap = &fontData->bitmap[glyph->dataOffset];
    } else if (it) {
      if (it->getGlyphBitmap(glyph->dataOffset, glyph->dataLength, localStackBuffer)) {
        bitmap = localStackBuffer;
      }
    }

    if (bitmap != nullptr) {
      // Glyph pixel (gx, gy) -> screen (x + ascender - top + gy, yPos - left - gx).
      const int ox = x + (fontData->ascender - glyph->top);
      const int oy = yPos - glyph->left;
      if (fontData->is2Bit) {
        blitGlyph<true>(target, bitmap, glyph->width, glyph->height, ox, oy, 0, -1, 1, 0);
      } else {
        blitGlyph<false>(target, bitmap, glyph->width, glyph->height, ox, oy, 0, -1, 1, 0);
      }
    }
    yPos -= glyph->advanceX;
  }
}

int TextRender::render(const int fontId, const int x, const int y, const char* text, const bool black,
                       const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  const EpdFontFamily* family = findFontFamily(gfx, fontId);
  if (family == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  // (No hasPrintableChars() pre-pass: a run it rejects consists only of zero-size glyphs, which draw nothing.)
  const EpdFontData* fontData = family->getData(style);
  if (!fontData) {
    return 0;
  }
  ExternalFont* stream = findStreamingFont(gfx, fontData);
  TextGlyphTarget target;
  initTarget(target, gfx, black, /*rotatedRules=*/false);

  const int yPos = y + family->getData(EpdFontFamily::REGULAR)->ascender;
  int xpos = x;
  WidthBounds bounds{x, 0, 0, 0};

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderChar(*family, fontData, stream, cp, &xpos, yPos, style, target, &bounds);
  }
  return stream ? bounds.advanceSum : bounds.maxX - bounds.minX;
}

int TextRender::renderScaled(const int fontId, const int x, const int y, const char* text, const uint8_t scalePct,
                             const bool black, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') {
    return x;
  }
  const EpdFontFamily* family = findFontFamily(gfx, fontId);
  if (family == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return x;
  }

  const EpdFontData* fontData = family->getData(style);
  if (!fontData) {
    return x;
  }
  ExternalFont* stream = findStreamingFont(gfx, fontData);
  TextGlyphTarget target;
  initTarget(target, gfx, black, /*rotatedRules=*/false);

  const int yPos = y + family->getData(EpdFontFamily::REGULAR)->ascender;
  int xpos = x;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderScaledChar(*family, fontData, stream, cp, &xpos, yPos, style, scalePct, target);
  }
  return xpos;
}

int TextRender::renderSmallCaps(const int fontId, const int x, const int y, const char* text, const bool black,
                                const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') {
    return x;
  }
  const EpdFontFamily* family = findFontFamily(gfx, fontId);
  if (family == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return x;
  }

  const EpdFontData* fontData = family->getData(style);
  if (!fontData) {
    return x;
  }
  ExternalFont* stream = findStreamingFont(gfx, fontData);
  TextGlyphTarget target;
  initTarget(target, gfx, black, /*rotatedRules=*/false);

  const std::string upper = toUpperUtf8(text);
  const char* ptr = upper.c_str();
  // Sit the small caps on the same baseline as the surrounding full-size text.
  const int yPos = y + family->getData(EpdFontFamily::REGULAR)->ascender;
  int xpos = x;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
    renderScaledChar(*family, fontData, stream, cp, &xpos, yPos, style, kSmallCapsScalePct, target);
  }
  return xpos;
}

void TextRender::centered(const int fontId, const int y, const char* text, const bool black,
                          const EpdFontFamily::Style style) const {
  const int x = (gfx.getScreenWidth() - getWidth(fontId, text, style)) / 2;
  render(fontId, x, y, text, black, style);
}

void TextRender::renderChar(const EpdFontFamily& fontFamily, const EpdFontData* fontData, ExternalFont* it,
                            const uint32_t cp, int* x, const int y, const EpdFontFamily::Style style,
                            const TextGlyphTarget& target, WidthBounds* bounds) const {
  EpdGlyph glyphStorage;
  const EpdGlyph* glyph = nullptr;
  if (it) {
    // Neither the codepoint nor U+FFFD in the font: skip it, exactly as getWidth() does, instead of drawing and
    // advancing by whatever was left in the uninitialized glyphStorage.
    if (!it->getGlyphMetadata(cp, glyphStorage) && !it->getGlyphMetadata(REPLACEMENT_GLYPH, glyphStorage)) {
      return;
    }
    if (bounds) {
      bounds->advanceSum += glyphStorage.advanceX;
    }
    glyph = &glyphStorage;
  } else {
    glyph = fontFamily.getGlyph(cp, style);
  }

  if (!glyph) {
    glyph = fontFamily.getGlyph(REPLACEMENT_GLYPH, style);
  }

  if (!glyph) {
    return;
  }

  if (!it && bounds) {
    const int cursor = *x - bounds->startX;
    bounds->minX = std::min(bounds->minX, cursor + glyph->left);
    bounds->maxX = std::max(bounds->maxX, cursor + glyph->left + glyph->width);
  }

  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int originX = *x + glyph->left;
  const int originY = y - glyph->top;

  if (fontData->bitmap != nullptr) {
    blitUpright(target, is2Bit, &fontData->bitmap[glyph->dataOffset], width, height, originX, originY);
  } else if (it) {
    uint8_t localStackBuffer[2048];
    const size_t dataLen = glyph->dataLength;
    if (dataLen <= sizeof(localStackBuffer)) {
      if (it->getGlyphBitmap(glyph->dataOffset, dataLen, localStackBuffer)) {
        blitUpright(target, is2Bit, localStackBuffer, width, height, originX, originY);
      }
    } else {
      // Too big for the stack buffer: stream it row by row (rows padded to whole bytes in this layout).
      constexpr size_t kMaxRowBytes = 512;
      const size_t rowBytes = is2Bit ? (static_cast<size_t>(width) + 3u) / 4u : (static_cast<size_t>(width) + 7u) / 8u;
      if (rowBytes != 0 && rowBytes <= kMaxRowBytes) {
        uint8_t rowBuf[kMaxRowBytes];
        for (int glyphY = 0; glyphY < height; glyphY++) {
          const uint32_t rowOff = glyph->dataOffset + static_cast<uint32_t>(glyphY) * static_cast<uint32_t>(rowBytes);
          if (!it->getGlyphBitmap(rowOff, rowBytes, rowBuf)) {
            break;
          }
          blitUpright(target, is2Bit, rowBuf, width, 1, originX, originY + glyphY);
        }
      }
    }
  }

  *x += glyph->advanceX;
}

void TextRender::renderScaledChar(const EpdFontFamily& fontFamily, const EpdFontData* fontData, ExternalFont* it,
                                  const uint32_t cp, int* x, const int y, const EpdFontFamily::Style style,
                                  const uint8_t scalePct, const TextGlyphTarget& target) const {
  EpdGlyph glyphStorage;
  const EpdGlyph* glyph = nullptr;
  if (it) {
    // Unknown codepoint and no U+FFFD: skip rather than use the uninitialized glyphStorage.
    if (!it->getGlyphMetadata(cp, glyphStorage) && !it->getGlyphMetadata(REPLACEMENT_GLYPH, glyphStorage)) {
      return;
    }
    glyph = &glyphStorage;
  } else {
    glyph = fontFamily.getGlyph(cp, style);
  }

  if (!glyph) {
    glyph = fontFamily.getGlyph(REPLACEMENT_GLYPH, style);
  }
  if (!glyph) {
    return;
  }

  const int is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int scaledAdvanceX = scaleMetricRound(glyph->advanceX, scalePct);
  const int scaledLeft = (glyph->left * static_cast<int>(scalePct) + kScaleRoundBias) / 100;
  const int scaledTop = std::max(1, (glyph->top * static_cast<int>(scalePct) + kScaleRoundBias) / 100);
  const int scaledW = std::max(1, (static_cast<int>(width) * static_cast<int>(scalePct) + kScaleRoundBias) / 100);
  const int scaledH = std::max(1, (static_cast<int>(height) * static_cast<int>(scalePct) + kScaleRoundBias) / 100);
  const int originX = *x + scaledLeft;
  const int originY = y - scaledTop;
  const bool fast = rectOnScreen(target, originX, originY, originX + scaledW - 1, originY + scaledH - 1);
  const uint8_t inkAction = target.lut1[1];

  const uint8_t* bitmap = nullptr;
  uint8_t localStackBuffer[2048];
  if (fontData->bitmap != nullptr) {
    bitmap = &fontData->bitmap[glyph->dataOffset];
  } else if (it) {
    const size_t dataLen = glyph->dataLength;
    if (dataLen <= sizeof(localStackBuffer)) {
      if (it->getGlyphBitmap(glyph->dataOffset, dataLen, localStackBuffer)) {
        bitmap = localStackBuffer;
      } else {
        *x += scaledAdvanceX;
        return;
      }
    } else {
      constexpr size_t kMaxRowBytes = 512;
      const size_t rowBytes = is2Bit ? (static_cast<size_t>(width) + 3u) / 4u : (static_cast<size_t>(width) + 7u) / 8u;
      if (rowBytes == 0 || rowBytes > kMaxRowBytes) {
        *x += scaledAdvanceX;
        return;
      }
      uint8_t rowBuf[kMaxRowBytes];
      for (int outY = 0; outY < scaledH; ++outY) {
        const int srcY = std::min<int>(height - 1, (outY * static_cast<int>(height)) / scaledH);
        const uint32_t rowOff = glyph->dataOffset + static_cast<uint32_t>(srcY) * static_cast<uint32_t>(rowBytes);
        if (!it->getGlyphBitmap(rowOff, rowBytes, rowBuf)) {
          *x += scaledAdvanceX;
          return;
        }
        const int screenY = originY + outY;
        for (int outX = 0; outX < scaledW; ++outX) {
          int sx0 = (outX * static_cast<int>(width)) / scaledW;
          int sx1 = ((outX + 1) * static_cast<int>(width)) / scaledW;
          if (sx1 <= sx0) sx1 = sx0 + 1;
          sx1 = std::min<int>(sx1, width);
          const int screenX = originX + outX;
          // Keep the darkest source pixel in the horizontal footprint so vertical strokes survive.
          if (is2Bit) {
            uint8_t rawMax = 0;
            for (int sx = sx0; sx < sx1; ++sx) {
              const uint8_t raw = (rowBuf[sx / 4] >> ((3 - (sx % 4)) * 2)) & 0x3;
              if (raw > rawMax) rawMax = raw;
            }
            plotLogical(target, screenX, screenY, target.lut2[rawMax], fast);
          } else {
            bool ink = false;
            for (int sx = sx0; sx < sx1; ++sx) {
              if ((rowBuf[sx / 8] >> (7 - (sx % 8))) & 1) {
                ink = true;
                break;
              }
            }
            if (ink) {
              plotLogical(target, screenX, screenY, inkAction, fast);
            }
          }
        }
      }
      *x += scaledAdvanceX;
      return;
    }
  }

  if (bitmap != nullptr) {
    for (int outY = 0; outY < scaledH; ++outY) {
      int sy0 = (outY * static_cast<int>(height)) / scaledH;
      int sy1 = ((outY + 1) * static_cast<int>(height)) / scaledH;
      if (sy1 <= sy0) sy1 = sy0 + 1;
      sy1 = std::min<int>(sy1, height);
      const int screenY = originY + outY;
      for (int outX = 0; outX < scaledW; ++outX) {
        int sx0 = (outX * static_cast<int>(width)) / scaledW;
        int sx1 = ((outX + 1) * static_cast<int>(width)) / scaledW;
        if (sx1 <= sx0) sx1 = sx0 + 1;
        sx1 = std::min<int>(sx1, width);
        const int screenX = originX + outX;
        // Keep the darkest source pixel in the footprint so thin strokes survive downscaling.
        if (is2Bit) {
          uint8_t rawMax = 0;
          for (int sy = sy0; sy < sy1; ++sy) {
            for (int sx = sx0; sx < sx1; ++sx) {
              const int pp = sy * width + sx;
              const uint8_t raw = (bitmap[pp / 4] >> ((3 - (pp % 4)) * 2)) & 0x3;
              if (raw > rawMax) rawMax = raw;
            }
          }
          plotLogical(target, screenX, screenY, target.lut2[rawMax], fast);
        } else {
          bool ink = false;
          for (int sy = sy0; sy < sy1 && !ink; ++sy) {
            for (int sx = sx0; sx < sx1; ++sx) {
              const int pp = sy * width + sx;
              if ((bitmap[pp / 8] >> (7 - (pp % 8))) & 1) {
                ink = true;
                break;
              }
            }
          }
          if (ink) {
            plotLogical(target, screenX, screenY, inkAction, fast);
          }
        }
      }
    }
  }
  *x += scaledAdvanceX;
}

int TextRender::getStreamingTextWidth(const EpdFontFamily& family, const char* text,
                                      const EpdFontFamily::Style style) const {
  const EpdFontData* data = family.getData(style);
  ExternalFont* it = findStreamingFont(gfx, data);
  if (!it) {
    return 0;
  }

  int totalWidth = 0;
  uint32_t cp;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text);

  while ((cp = utf8NextCodepoint(&ptr))) {
    EpdGlyph glyph;
    if (!it->getGlyphMetadata(cp, glyph)) {
      if (!it->getGlyphMetadata(REPLACEMENT_GLYPH, glyph)) {
        continue;
      }
    }
    totalWidth += glyph.advanceX;
  }
  return totalWidth;
}
