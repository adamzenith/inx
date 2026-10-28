#include "ExternalFont.h"

#include <Arduino.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>

/** Construct an ExternalFont with no font loaded. */
ExternalFont::ExternalFont() : m_fontData(nullptr) {}

/** Unload the font and release its resources. */
ExternalFont::~ExternalFont() { unload(); }

ExternalFont::GlyphBitmapCacheStorage* ExternalFont::s_bitmapCache = nullptr;
uint32_t ExternalFont::s_bitmapWriteHead = 0;
uint8_t ExternalFont::s_bitmapCacheUsers = 0;
uint8_t ExternalFont::s_nextBitmapCacheTag = 0;

namespace {
inline size_t metaSlotFor(const uint32_t cp, const size_t slots) { return (cp ^ (cp >> 7) ^ (cp >> 14)) & (slots - 1); }
inline size_t bitmapSetFor(const uint32_t offset, const uint8_t tag) {
  return ((offset ^ (static_cast<uint32_t>(tag) << 24)) * 2654435761u) >> 25;  // top 7 bits -> 128 sets
}
}  // namespace

/** Clear the per-instance glyph metadata cache. */
void ExternalFont::metaCacheClear() {
  for (size_t i = 0; i < kGlyphMetaCacheSlots; ++i) {
    m_metaCache[i].cp = 0xFFFFFFFFu;
    m_metaCache[i].glyph = EpdGlyph{};
  }
}

/** Clear this font's entries from the shared bitmap cache. */
void ExternalFont::bitmapCacheClear() {
  if (s_bitmapCache && m_bitmapCacheTag != 0) {
    for (auto& e : s_bitmapCache->index) {
      if (e.tag == m_bitmapCacheTag) {
        e.tag = 0;
      }
    }
  }
}

/** Enable or disable the shared glyph bitmap cache for this font instance. */
void ExternalFont::setGlyphBitmapCacheEnabled(const bool enabled) {
  if (enabled == m_bitmapCacheEnabled) {
    return;
  }

  m_bitmapCacheEnabled = enabled;
  if (!enabled) {
    bitmapCacheClear();
    m_bitmapCacheTag = 0;
    if (s_bitmapCacheUsers > 0) {
      --s_bitmapCacheUsers;
    }
    if (s_bitmapCacheUsers == 0) {
      delete s_bitmapCache;
      s_bitmapCache = nullptr;
      s_bitmapWriteHead = 0;
    }
    return;
  }

  if (!s_bitmapCache) {
    s_bitmapCache = new (std::nothrow) GlyphBitmapCacheStorage;
    if (s_bitmapCache) {
      for (auto& e : s_bitmapCache->index) {
        e = GlyphBitmapCacheEntry{0, 0, 0, 0, 0};
      }
    }
    s_bitmapWriteHead = 0;
  }
  if (s_bitmapCache) {
    ++s_bitmapCacheUsers;
    if (++s_nextBitmapCacheTag == 0) {
      s_nextBitmapCacheTag = 1;
    }
    m_bitmapCacheTag = s_nextBitmapCacheTag;
    bitmapCacheClear();  // drop stale entries left by a previous owner of this tag
  } else {
    m_bitmapCacheEnabled = false;
  }
}

/** Close the font file and release cached glyph data. */
void ExternalFont::unload() {
  setGlyphBitmapCacheEnabled(false);
  if (m_file) m_file.close();
  if (m_fontData) {
    delete m_fontData;
    m_fontData = nullptr;
  }
  m_glyphTableStart = 0;
  m_glyphCount = 0;
  m_bitmapDataStart = 0;
  m_hasAntiAliasData = false;
  freeGlyphRuns();
  metaCacheClear();
}

/** Load font metadata from an on-disk font file at path, keeping glyph data on SD for on-demand reads. */
bool ExternalFont::load(const char* path, const bool enableGlyphBitmapCache) {
  unload();
  m_filePath = path;
  m_file = SdMan.open(path, FILE_READ);
  if (!m_file) return false;

  uint32_t magic, version;
  m_file.read(reinterpret_cast<uint8_t*>(&magic), 4);
  m_file.read(reinterpret_cast<uint8_t*>(&version), 4);

  uint16_t nameLen;
  m_file.read(reinterpret_cast<uint8_t*>(&nameLen), 2);
  m_file.seek(m_file.position() + nameLen);

  m_fontData = new EpdFontData();
  int16_t lineHeight, ascender, descender;
  m_file.read(reinterpret_cast<uint8_t*>(&lineHeight), 2);
  m_file.read(reinterpret_cast<uint8_t*>(&ascender), 2);
  m_file.read(reinterpret_cast<uint8_t*>(&descender), 2);
  uint8_t is2Bit;
  m_file.read(&is2Bit, 1);

  int lh = static_cast<int>(lineHeight);
  if (lh < 1) lh = 12;
  if (lh > 255) lh = 255;
  m_fontData->advanceY = static_cast<uint8_t>(lh);
  m_fontData->ascender = ascender;
  m_fontData->descender = descender;
  m_fontData->is2Bit = (is2Bit != 0);
  m_fontData->bitmap = nullptr;
  m_fontData->glyph = nullptr;
  m_fontData->intervals = nullptr;
  m_fontData->intervalCount = 0;

  uint16_t intervalCount;
  m_file.read(reinterpret_cast<uint8_t*>(&intervalCount), 2);
  m_file.seek(m_file.position() + (intervalCount * 12));

  m_file.read(reinterpret_cast<uint8_t*>(&m_glyphCount), 4);
  m_glyphTableStart = m_file.position();

  const uint32_t tableBytes = m_glyphCount * 24u;
  if (!m_file.seek(m_glyphTableStart + tableBytes)) {
    Serial.println("[ExternalFont] Seek past glyph table failed");
    return false;
  }
  m_bitmapDataStart = m_file.position();
  buildGlyphRuns();
  setGlyphBitmapCacheEnabled(enableGlyphBitmapCache);
  m_hasAntiAliasData = enableGlyphBitmapCache && m_fontData->is2Bit && detectAntiAliasData();

  metaCacheClear();
  bitmapCacheClear();
  if (m_bitmapCacheEnabled) {
    Serial.printf(
        "[ExternalFont] On-demand glyph table: %u glyphs, mode=%s, aa=%d, runs %u (~%u B), meta %u-slot (~%u B), "
        "shared bitmap arena %u B + %u-entry index (~%u B)\n",
        m_glyphCount, m_fontData->is2Bit ? "2bit" : "1bit", m_hasAntiAliasData ? 1 : 0,
        static_cast<unsigned>(m_runCount), static_cast<unsigned>(m_runCount * sizeof(GlyphRun)),
        static_cast<unsigned>(kGlyphMetaCacheSlots), static_cast<unsigned>(sizeof(m_metaCache)),
        static_cast<unsigned>(kGlyphBitmapArenaBytes),
        static_cast<unsigned>(kGlyphBitmapCacheSets * kGlyphBitmapCacheWays),
        static_cast<unsigned>(sizeof(GlyphBitmapCacheStorage)));
  } else {
    Serial.printf(
        "[ExternalFont] On-demand glyph table: %u glyphs, mode=%s, aa=0, runs %u, meta %u-slot (~%u B), "
        "bitmap cache off\n",
        m_glyphCount, m_fontData->is2Bit ? "2bit" : "1bit", static_cast<unsigned>(m_runCount),
        static_cast<unsigned>(kGlyphMetaCacheSlots), static_cast<unsigned>(sizeof(m_metaCache)));
  }
  return true;
}

/** Determine whether the loaded 2-bit font actually contains anti-aliased pixel values. */
bool ExternalFont::detectAntiAliasData() {
  if (!m_fontData || !m_fontData->is2Bit || m_glyphCount == 0) {
    return false;
  }

  static constexpr uint32_t kSampleCodepoints[] = {
      'a', 'e', 'g', 'm', 'n', 'o', 's', 't', 'A', 'E', 'H', 'M', 'O', 'S', '0', '2', '8',
  };
  uint8_t buffer[256];

  auto glyphHasGray = [&](const EpdGlyph& glyph) -> bool {
    if (glyph.dataLength == 0) {
      return false;
    }

    uint32_t remaining = glyph.dataLength;
    uint32_t offset = glyph.dataOffset;
    while (remaining > 0) {
      const uint32_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
      if (!getGlyphBitmap(offset, chunk, buffer)) {
        return false;
      }
      for (uint32_t i = 0; i < chunk; ++i) {
        const uint8_t byte = buffer[i];
        for (uint8_t shift = 0; shift < 8; shift += 2) {
          const uint8_t value = (byte >> shift) & 0x03u;
          if (value == 1u || value == 2u) {
            return true;
          }
        }
      }
      offset += chunk;
      remaining -= chunk;
    }
    return false;
  };

  const bool sampleHasGray = std::any_of(std::begin(kSampleCodepoints), std::end(kSampleCodepoints), [&](uint32_t cp) {
    EpdGlyph glyph{};
    return getGlyphMetadata(cp, glyph) && glyphHasGray(glyph);
  });
  if (sampleHasGray) {
    return true;
  }

  static constexpr uint32_t kMaxFallbackGlyphs = 96;
  const uint32_t limit = m_glyphCount < kMaxFallbackGlyphs ? m_glyphCount : kMaxFallbackGlyphs;
  // cppcheck-suppress knownConditionTrueFalse ; m_glyphCount is populated by ExternalFont::load() via
  // m_file.read(&m_glyphCount, 4), an opaque library call cppcheck's ValueFlow can't see through, so it
  // wrongly assumes m_glyphCount is still its default 0 here.
  for (uint32_t i = 0; i < limit; ++i) {
    uint8_t entry[24];
    EpdGlyph glyph{};
    if (!readGlyphEntryAtIndex(i, entry)) {
      return false;
    }
    decodeGlyphRow(entry, glyph);
    if (glyphHasGray(glyph)) {
      return true;
    }
  }
  return false;
}

/** Read the raw 24-byte glyph table entry at index into out24. */
bool ExternalFont::readGlyphEntryAtIndex(uint32_t index, uint8_t out24[24]) const {
  if (index >= m_glyphCount || out24 == nullptr) {
    return false;
  }
  FsFile* useFile = const_cast<FsFile*>(&m_file);
  if (!*useFile || !useFile->isOpen()) {
    *useFile = SdMan.open(m_filePath.c_str(), FILE_READ);
    if (!*useFile) {
      return false;
    }
  }
  const uint32_t pos = m_glyphTableStart + index * 24u;
  if (!useFile->seek(pos)) {
    return false;
  }
  return useFile->read(out24, 24) == 24;
}

/** Read the code point stored in the glyph table entry at index. */
bool ExternalFont::readCodepointAtIndex(uint32_t index, uint32_t& outCp) const {
  if (index >= m_glyphCount) {
    return false;
  }
  FsFile* useFile = const_cast<FsFile*>(&m_file);
  if (!*useFile || !useFile->isOpen()) {
    *useFile = SdMan.open(m_filePath.c_str(), FILE_READ);
    if (!*useFile) {
      return false;
    }
  }
  const uint32_t pos = m_glyphTableStart + index * 24u + 18u;
  if (!useFile->seek(pos)) {
    return false;
  }
  return useFile->read(reinterpret_cast<uint8_t*>(&outCp), 4) == 4;
}

/** Decode a raw 24-byte glyph table row into an EpdGlyph. */
void ExternalFont::decodeGlyphRow(const uint8_t entry[24], EpdGlyph& out) const {
  uint16_t w = 0, h = 0, ax = 0;
  int16_t lef = 0, tp = 0;
  uint32_t dlen = 0, rel = 0;
  memcpy(&w, entry + 0, 2);
  memcpy(&h, entry + 2, 2);
  memcpy(&ax, entry + 4, 2);
  memcpy(&lef, entry + 6, 2);
  memcpy(&tp, entry + 8, 2);
  memcpy(&dlen, entry + 10, 4);
  memcpy(&rel, entry + 14, 4);
  out.width = static_cast<uint8_t>(w > 255 ? 255 : w);
  out.height = static_cast<uint8_t>(h > 255 ? 255 : h);
  out.advanceX = static_cast<uint8_t>(ax > 255 ? 255 : ax);
  out.left = lef;
  out.top = tp;
  out.dataLength = static_cast<uint16_t>(dlen > 0xFFFFu ? 0xFFFFu : dlen);
  out.dataOffset = m_bitmapDataStart + rel;
}

/** Scan the on-SD glyph table once and build the in-RAM table of consecutive-code-point runs. */
void ExternalFont::buildGlyphRuns() {
  freeGlyphRuns();
  if (m_glyphCount == 0 || m_glyphCount > kMaxGlyphsToScanForRuns) {
    return;
  }
  if (!m_file.seek(m_glyphTableStart)) {
    return;
  }

  // Worst case is kMaxGlyphRuns entries; collect into a temporary buffer, then keep an exact-size copy.
  GlyphRun* tmp = new (std::nothrow) GlyphRun[kMaxGlyphRuns];
  if (!tmp) {
    return;
  }

  static constexpr uint32_t kRowsPerChunk = 16;
  uint8_t chunk[kRowsPerChunk * 24];
  uint32_t runCount = 0;
  uint32_t prevCp = 0;
  bool ok = true;
  for (uint32_t index = 0; ok && index < m_glyphCount;) {
    const uint32_t rows = std::min(kRowsPerChunk, m_glyphCount - index);
    if (static_cast<size_t>(m_file.read(chunk, rows * 24u)) != rows * 24u) {
      ok = false;
      break;
    }
    for (uint32_t r = 0; r < rows; ++r, ++index) {
      uint32_t cp = 0;
      memcpy(&cp, chunk + r * 24u + 18u, 4);
      if (index > 0 && cp <= prevCp) {
        ok = false;  // not strictly ascending: keep the original on-disk search semantics
        break;
      }
      if (index == 0 || cp != prevCp + 1) {
        if (runCount == kMaxGlyphRuns) {
          ok = false;  // too fragmented to index cheaply; fall back to the on-disk search
          break;
        }
        tmp[runCount++] = GlyphRun{cp, index};
      }
      prevCp = cp;
    }
  }

  if (ok && runCount > 0) {
    m_runs = new (std::nothrow) GlyphRun[runCount];
    if (m_runs) {
      memcpy(m_runs, tmp, runCount * sizeof(GlyphRun));
      m_runCount = runCount;
    }
  }
  delete[] tmp;
}

/** Release the in-RAM run table. */
void ExternalFont::freeGlyphRuns() {
  delete[] m_runs;
  m_runs = nullptr;
  m_runCount = 0;
}

/** Find the glyph table index of cp using the in-RAM run table. Returns false if cp is not in the font. */
bool ExternalFont::findIndexInRuns(const uint32_t cp, uint32_t& outIndex) const {
  // Find the last run whose firstCp <= cp.
  uint32_t lo = 0;
  uint32_t hi = m_runCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (m_runs[mid].firstCp <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return false;
  }
  const GlyphRun& run = m_runs[lo - 1];
  const uint32_t runEnd = (lo < m_runCount) ? m_runs[lo].firstIndex : m_glyphCount;
  const uint32_t delta = cp - run.firstCp;
  if (delta >= runEnd - run.firstIndex) {
    return false;
  }
  outIndex = run.firstIndex + delta;
  return true;
}

/** Find the glyph table index of cp by binary-searching the glyph table on the SD card. */
bool ExternalFont::findIndexOnDisk(const uint32_t cp, uint32_t& outIndex, bool& ioError) const {
  ioError = false;
  int32_t low = 0;
  int32_t high = static_cast<int32_t>(m_glyphCount) - 1;

  while (low <= high) {
    const int32_t mid = low + (high - low) / 2;
    uint32_t midCp = 0;
    if (!readCodepointAtIndex(static_cast<uint32_t>(mid), midCp)) {
      ioError = true;
      return false;
    }

    if (midCp == cp) {
      outIndex = static_cast<uint32_t>(mid);
      return true;
    }

    if (midCp < cp) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return false;
}

/** Look up a glyph's metadata in the per-instance metadata cache. Sets found=false for a cached miss. */
bool ExternalFont::metaCacheLookup(const uint32_t cp, EpdGlyph& out, bool& found) const {
  const GlyphMetaCacheSlot& slot = m_metaCache[metaSlotFor(cp, kGlyphMetaCacheSlots)];
  if (slot.cp != cp) {
    return false;
  }
  found = slot.glyph.dataOffset != 0;
  if (found) {
    out = slot.glyph;
  }
  return true;
}

void ExternalFont::metaCacheStore(const uint32_t cp, const EpdGlyph* g) {
  GlyphMetaCacheSlot& slot = m_metaCache[metaSlotFor(cp, kGlyphMetaCacheSlots)];
  slot.cp = cp;
  slot.glyph = g ? *g : EpdGlyph{};
}

bool ExternalFont::bitmapCacheLookup(uint32_t offset, uint32_t length, uint8_t* outputBuffer) {
  if (!outputBuffer || !bitmapCacheCanStore(offset, length)) {
    return false;
  }
  const GlyphBitmapCacheEntry* set =
      &s_bitmapCache->index[bitmapSetFor(offset, m_bitmapCacheTag) * kGlyphBitmapCacheWays];
  for (size_t w = 0; w < kGlyphBitmapCacheWays; ++w) {
    const GlyphBitmapCacheEntry& e = set[w];
    if (e.tag == m_bitmapCacheTag && e.offset == offset && e.length == length &&
        s_bitmapWriteHead - e.absPos <= kGlyphBitmapArenaBytes) {
      memcpy(outputBuffer, s_bitmapCache->arena + (e.absPos % kGlyphBitmapArenaBytes), length);
      return true;
    }
  }
  return false;
}

void ExternalFont::bitmapCacheStore(uint32_t offset, uint32_t length, const uint8_t* data) {
  if (!data || !bitmapCacheCanStore(offset, length)) {
    return;
  }

  // Keep the byte counter far from wrap-around (practically unreachable).
  if (s_bitmapWriteHead > 0xF0000000u) {
    for (auto& e : s_bitmapCache->index) {
      e.tag = 0;
    }
    s_bitmapWriteHead = 0;
  }

  // Allocate from the ring; an entry never straddles the wrap point.
  uint32_t pos = s_bitmapWriteHead % kGlyphBitmapArenaBytes;
  if (pos + length > kGlyphBitmapArenaBytes) {
    s_bitmapWriteHead += kGlyphBitmapArenaBytes - pos;
    pos = 0;
  }
  const uint32_t absPos = s_bitmapWriteHead;
  s_bitmapWriteHead += length;
  memcpy(s_bitmapCache->arena + pos, data, length);

  // Pick a way: an empty or overwritten one if available, otherwise the older entry.
  GlyphBitmapCacheEntry* set = &s_bitmapCache->index[bitmapSetFor(offset, m_bitmapCacheTag) * kGlyphBitmapCacheWays];
  GlyphBitmapCacheEntry* victim = &set[0];
  for (size_t w = 0; w < kGlyphBitmapCacheWays; ++w) {
    GlyphBitmapCacheEntry& e = set[w];
    if (e.tag == 0 || s_bitmapWriteHead - e.absPos > kGlyphBitmapArenaBytes) {
      victim = &e;
      break;
    }
    if (e.absPos < victim->absPos) {
      victim = &e;
    }
  }
  victim->offset = offset;
  victim->absPos = absPos;
  victim->length = static_cast<uint16_t>(length);
  victim->tag = m_bitmapCacheTag;
}

bool ExternalFont::bitmapCacheCanStore(const uint32_t offset, const uint32_t length) const {
  if (!m_bitmapCacheEnabled || !s_bitmapCache || m_bitmapCacheTag == 0 || offset == 0 || length == 0 ||
      length > kGlyphBitmapCacheMaxBytes) {
    return false;
  }
  return true;
}

bool ExternalFont::getGlyphMetadata(uint32_t cp, EpdGlyph& out) {
  if (m_glyphCount == 0) {
    return false;
  }

  bool cachedFound = false;
  if (metaCacheLookup(cp, out, cachedFound)) {
    return cachedFound;
  }

  uint32_t index = 0;
  bool found = false;
  if (m_runs) {
    found = findIndexInRuns(cp, index);
  } else {
    bool ioError = false;
    found = findIndexOnDisk(cp, index, ioError);
    if (ioError) {
      return false;  // don't cache transient SD errors
    }
  }

  if (!found) {
    metaCacheStore(cp, nullptr);  // negative entry: cp is not in this font
    return false;
  }

  uint8_t entry[24];
  if (!readGlyphEntryAtIndex(index, entry)) {
    return false;
  }
  uint32_t rowCp = 0;
  memcpy(&rowCp, entry + 18, 4);
  if (rowCp != cp) {
    return false;  // index inconsistent with the file; don't cache
  }
  decodeGlyphRow(entry, out);
  metaCacheStore(cp, &out);
  return true;
}

bool ExternalFont::getGlyphBitmap(uint32_t absoluteOffset, uint32_t length, uint8_t* buffer) {
  if (length == 0) return true;
  if (!buffer) return false;

  if (bitmapCacheLookup(absoluteOffset, length, buffer)) {
    return true;
  }

  if (!m_file || !m_file.isOpen()) {
    m_file = SdMan.open(m_filePath.c_str(), FILE_READ);
    if (!m_file) {
      Serial.printf("[ExternalFont] ERR: Could not reopen %s for bitmap read\n", m_filePath.c_str());
      return false;
    }
  }

  if (!m_file.seek(absoluteOffset)) {
    Serial.printf("[ExternalFont] ERR: Seek failed to %u\n", absoluteOffset);
    return false;
  }

  const size_t bytesRead = m_file.read(buffer, length);
  if (bytesRead != length) {
    Serial.printf("[ExternalFont] ERR: Read mismatch. Expected %u, got %u\n", length, static_cast<unsigned>(bytesRead));
    return false;
  }

  bitmapCacheStore(absoluteOffset, length, buffer);
  return true;
}
