#pragma once
#include <string>

#include "EpdFontData.h"
#include "SDCardManager.h"

class ExternalFont {
 public:
  /** Construct an ExternalFont with no font loaded. */
  ExternalFont();
  /** Unload the font and release its resources. */
  ~ExternalFont();
  ExternalFont(const ExternalFont&) = delete;
  ExternalFont& operator=(const ExternalFont&) = delete;
  /** Load font metadata from an on-disk font file at path, keeping glyph data on SD for on-demand reads. */
  bool load(const char* path, bool enableGlyphBitmapCache = true);
  /** Close the font file and release cached glyph data. */
  void unload();
  /** Enable or disable the shared glyph bitmap cache for this font instance. */
  void setGlyphBitmapCacheEnabled(bool enabled);
  /** Look up glyph metadata for a code point, using the metadata cache when possible. */
  bool getGlyphMetadata(uint32_t codePoint, EpdGlyph& outGlyph);
  /** Read length bytes of glyph bitmap data at offset into outputBuffer, using the bitmap cache when possible. */
  bool getGlyphBitmap(uint32_t offset, uint32_t length, uint8_t* outputBuffer);
  /** Return whether the loaded font contains anti-aliased (grayscale) glyph data. */
  bool hasAntiAliasData() const { return m_hasAntiAliasData; }
  /** Return the underlying EpdFontData for the loaded font. */
  EpdFontData* getData() { return m_fontData; }

 private:
  /** Determine whether the loaded 2-bit font actually contains anti-aliased pixel values. */
  bool detectAntiAliasData();
  /** Read the raw 24-byte glyph table entry at index into out24. */
  bool readGlyphEntryAtIndex(uint32_t index, uint8_t out24[24]) const;
  /** Read the code point stored in the glyph table entry at index. */
  bool readCodepointAtIndex(uint32_t index, uint32_t& outCp) const;
  /** Decode a raw 24-byte glyph table row into an EpdGlyph. */
  void decodeGlyphRow(const uint8_t entry[24], EpdGlyph& out) const;
  /** Look up a glyph's metadata in the per-instance metadata cache. Sets found=false for a cached miss. */
  bool metaCacheLookup(uint32_t cp, EpdGlyph& out, bool& found) const;
  /** Store a glyph's metadata (or a negative entry when g is null) in the direct-mapped metadata cache. */
  void metaCacheStore(uint32_t cp, const EpdGlyph* g);
  /** Clear the per-instance glyph metadata cache. */
  void metaCacheClear();
  /** Scan the on-SD glyph table once and build the in-RAM table of consecutive-code-point runs. */
  void buildGlyphRuns();
  /** Release the in-RAM run table. */
  void freeGlyphRuns();
  /** Find the glyph table index of cp using the in-RAM run table. Returns false if cp is not in the font. */
  bool findIndexInRuns(uint32_t cp, uint32_t& outIndex) const;
  /** Find the glyph table index of cp by binary-searching the glyph table on the SD card. */
  bool findIndexOnDisk(uint32_t cp, uint32_t& outIndex, bool& ioError) const;
  /** Look up cached bitmap data for this font at offset/length, copying it into outputBuffer if found. */
  bool bitmapCacheLookup(uint32_t offset, uint32_t length, uint8_t* outputBuffer);
  /** Store bitmap data for this font in the shared bitmap cache, overwriting the oldest arena bytes if needed. */
  void bitmapCacheStore(uint32_t offset, uint32_t length, const uint8_t* data);
  /** Clear this font's entries from the shared bitmap cache. */
  void bitmapCacheClear();
  /** Return whether the given offset/length is eligible for bitmap caching. */
  bool bitmapCacheCanStore(uint32_t offset, uint32_t length) const;

  // Glyph metadata cache: direct-mapped, keyed by code point. A slot whose glyph.dataOffset is 0 records a
  // code point known to be absent from the font (real glyphs always have dataOffset >= m_bitmapDataStart > 0).
  static constexpr size_t kGlyphMetaCacheSlots = 128;
  static_assert((kGlyphMetaCacheSlots & (kGlyphMetaCacheSlots - 1)) == 0, "meta cache slots must be a power of 2");
  struct GlyphMetaCacheSlot {
    uint32_t cp = 0xFFFFFFFFu;
    EpdGlyph glyph{};
  };
  GlyphMetaCacheSlot m_metaCache[kGlyphMetaCacheSlots];

  // In-RAM index of the glyph table: maximal runs of consecutive code points. Run i covers code points
  // [firstCp, firstCp + (nextRun.firstIndex - firstIndex)) at glyph table indices starting at firstIndex.
  // Built once at load; if the table would be too large we fall back to binary-searching the SD copy.
  struct GlyphRun {
    uint32_t firstCp;
    uint32_t firstIndex;
  };
  static constexpr uint32_t kMaxGlyphRuns = 256;           // <= 2 KB per font
  static constexpr uint32_t kMaxGlyphsToScanForRuns = 8192;  // bounds load-time SD reads (~192 KB)
  GlyphRun* m_runs = nullptr;
  uint32_t m_runCount = 0;

  // Shared glyph bitmap cache: a byte ring ("arena") holding variable-sized glyph bitmaps, indexed by a
  // 2-way set-associative table keyed by (font tag, offset). Ring positions are tracked as a monotonically
  // increasing byte counter, so an entry is valid while the write head has advanced less than the arena size
  // past it (FIFO eviction, no per-hit bookkeeping).
  static constexpr size_t kGlyphBitmapCacheMaxBytes = 512;
  static constexpr size_t kGlyphBitmapCacheSets = 128;  // bitmapSetFor() yields 7 bits
  static constexpr size_t kGlyphBitmapCacheWays = 2;
  static constexpr size_t kGlyphBitmapArenaBytes = 13312;
  struct GlyphBitmapCacheEntry {
    uint32_t offset;
    uint32_t absPos;
    uint16_t length;
    uint8_t tag;  // 0 = empty
    uint8_t reserved;
  };
  struct GlyphBitmapCacheStorage {
    GlyphBitmapCacheEntry index[kGlyphBitmapCacheSets * kGlyphBitmapCacheWays];
    uint8_t arena[kGlyphBitmapArenaBytes];
  };
  static GlyphBitmapCacheStorage* s_bitmapCache;
  static uint32_t s_bitmapWriteHead;
  static uint8_t s_bitmapCacheUsers;
  static uint8_t s_nextBitmapCacheTag;
  bool m_bitmapCacheEnabled = false;
  uint8_t m_bitmapCacheTag = 0;

  EpdFontData* m_fontData;
  std::string m_filePath;
  FsFile m_file;
  uint32_t m_glyphTableStart = 0;
  uint32_t m_glyphCount = 0;
  uint32_t m_bitmapDataStart = 0;
  bool m_hasAntiAliasData = false;
};
