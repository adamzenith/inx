#pragma once

/**
 * @file ZipFile.h
 * @brief Public interface and types for ZipFile.
 */

#include <SdFat.h>

#include <string>
#include <unordered_map>
#include <vector>

struct ZipCentralDirIndex;

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t localHeaderOffset;
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  using CentralDirIndex = ZipCentralDirIndex;

  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };

  static uint32_t fnvHash32(const char* s, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 16777619u;
    }
    return hash;
  }

  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  CentralDirIndex* index = nullptr;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();
  bool ensureIndex();
  bool lookupInIndex(const char* filename, FileStatSlim* fileStat);

 public:
  explicit ZipFile(const std::string& filePath, CentralDirIndex* index = nullptr) : filePath(filePath), index(index) {}
  ~ZipFile() = default;

  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);

  int fillUncompressedSizes(std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes);

  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize);
};

/**
 * Compact, caller-owned lookup index over a zip's central directory, shared by the short-lived ZipFile
 * objects an owner (e.g. Epub) creates per item read. Built lazily on the first lookup with one buffered
 * pass over the central directory; afterwards a lookup is a binary search plus one small read of the
 * matching central-directory record (names are verified, so hash collisions are harmless).
 * RAM: 8 bytes per entry, only for archives with <= kMaxEntries entries (larger ones fall back to the
 * linear scan, still skipping the EOCD re-read). Not thread-safe: one owner thread at a time.
 */
struct ZipCentralDirIndex {
  static constexpr uint16_t kMaxEntries = 2048;
  struct Entry {
    uint32_t nameHash;
    uint32_t cdEntryOffset;
  };
  ZipFile::ZipDetails details = {0, 0, false};
  Entry* entries = nullptr;
  uint16_t count = 0;
  bool built = false;
  bool unavailable = false;

  ZipCentralDirIndex() = default;
  ZipCentralDirIndex(const ZipCentralDirIndex&) = delete;
  ZipCentralDirIndex& operator=(const ZipCentralDirIndex&) = delete;
  ~ZipCentralDirIndex() { free(entries); }
};
