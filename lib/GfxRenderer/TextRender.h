#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>

class GfxRenderer;
class ExternalFont;
struct TextGlyphTarget;

class TextRender {
 public:
  explicit TextRender(GfxRenderer& g) : gfx(g) {}

  int getWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getHeight(int fontId) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  int getSpaceWidth(int fontId) const;
  bool supportsAntiAliasing(int fontId) const;
  /** Pixels between a glyph's top edge and the font's ascender line (ascender - glyph.top). Used to align
   *  a drop cap's cap-top with the surrounding body text, whose font has a different inset. */
  int getGlyphTopInset(int fontId, uint32_t codepoint, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /** Pixels from a glyph's bottom edge to the font line box bottom. */
  int getGlyphBottomInset(int fontId, uint32_t codepoint, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  std::string truncate(int fontId, const char* text, int maxWidth,
                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void rotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                   EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /** Draws text with its top at y. Returns the text width exactly as getWidth() would measure it, from the same
   *  glyph walk, so callers that need the width (e.g. underline/bionic layout) don't re-measure. */
  int render(int fontId, int x, int y, const char* text, bool black = true,
             EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getScaledWidth(int fontId, const char* text, uint8_t scalePct,
                     EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int renderScaled(int fontId, int x, int y, const char* text, uint8_t scalePct, bool black = true,
                   EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSmallCapsWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /** Renders small caps and returns the x position after the text (its advance), so callers don't re-measure. */
  int renderSmallCaps(int fontId, int x, int y, const char* text, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void centered(int fontId, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

 private:
  struct WidthBounds;
  void renderChar(const EpdFontFamily& fontFamily, const EpdFontData* fontData, ExternalFont* stream, uint32_t cp,
                  int* x, int y, EpdFontFamily::Style style, const TextGlyphTarget& target,
                  WidthBounds* bounds) const;
  void renderScaledChar(const EpdFontFamily& fontFamily, const EpdFontData* fontData, ExternalFont* stream,
                        uint32_t cp, int* x, int y, EpdFontFamily::Style style, uint8_t scalePct,
                        const TextGlyphTarget& target) const;
  int getStreamingTextWidth(const EpdFontFamily& family, const char* text, EpdFontFamily::Style style) const;
  GfxRenderer& gfx;
};
