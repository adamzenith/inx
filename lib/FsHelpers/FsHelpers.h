#pragma once

/**
 * @file FsHelpers.h
 * @brief Public interface and types for FsHelpers.
 */

#include <cstddef>
#include <string>

class FsHelpers {
 public:
  static std::string normalisePath(const std::string& path);
  static std::string resolveRelativePath(const std::string& currentFile, const std::string& relativePath);

  /** FsFile::read() returns -1 on an SD error, which stored in a size_t reads as a huge byte count; this reports
   * it as nothing read instead. */
  template <typename File>
  static size_t readChecked(File& file, void* buf, const size_t len) {
    const int got = file.read(buf, len);
    return got > 0 ? static_cast<size_t>(got) : 0;
  }
};