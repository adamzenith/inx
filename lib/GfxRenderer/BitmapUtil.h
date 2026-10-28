#pragma once

/**
 * @file BitmapUtil.h
 * @brief Public interface and types for BitmapUtil.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "ImageToneDither.h"

class Print;

struct BmpHeader;

uint8_t quantize(int gray, int x, int y);
uint8_t quantizeSimple(int gray);
ImageToneSample quantizeTwoBitImage(int gray);
uint8_t adjustTwoBitImageLevelForDisplay(uint8_t level);
uint8_t mapQualityGray2Level(uint8_t level);
uint8_t quantize1bit(int gray, int x, int y);
int adjustOneBitPixel(int gray);
int adjustPixel(int gray);

uint8_t rgbToGray(uint8_t r, uint8_t g, uint8_t b);

bool bmpTo1BitBmpScaled(const char* srcPath, const char* dstPath, int targetMaxWidth, int targetMaxHeight);

enum class BmpRowOrder { BottomUp, TopDown };

void createBmpHeader(BmpHeader* bmpHeader, int width, int height, BmpRowOrder rowOrder);

class Atkinson1BitDitherer {
 public:
  // The three error rows share one allocation. If it fails, pixels fall back to a plain threshold instead
  // of the throwing new[] aborting the device.
  explicit Atkinson1BitDitherer(int width) : width(width > 0 ? width : 0) {
    if (this->width == 0) return;
    const size_t rowSize = static_cast<size_t>(this->width) + 4;
    errorRows = static_cast<int16_t*>(calloc(rowSize * 3, sizeof(int16_t)));
    if (!errorRows) return;
    errorRow0 = errorRows;
    errorRow1 = errorRow0 + rowSize;
    errorRow2 = errorRow1 + rowSize;
  }

  ~Atkinson1BitDitherer() { free(errorRows); }

  bool isValid() const { return errorRows != nullptr; }

  Atkinson1BitDitherer(const Atkinson1BitDitherer& other) = delete;

  Atkinson1BitDitherer& operator=(const Atkinson1BitDitherer& other) = delete;

  uint8_t processPixel(int gray, int x) {
    gray = adjustOneBitPixel(gray);
    if (!isValid()) return gray < 128 ? 0 : 1;

    int adjusted = gray + errorRow0[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    uint8_t quantized;
    int quantizedValue;
    if (adjusted < 128) {
      quantized = 0;
      quantizedValue = 0;
    } else {
      quantized = 1;
      quantizedValue = 255;
    }

    int error = (adjusted - quantizedValue) >> 3;

    errorRow0[x + 3] += error;
    errorRow0[x + 4] += error;
    errorRow1[x + 1] += error;
    errorRow1[x + 2] += error;
    errorRow1[x + 3] += error;
    errorRow2[x + 2] += error;

    return quantized;
  }

  void nextRow() {
    if (!isValid()) return;
    int16_t* temp = errorRow0;
    errorRow0 = errorRow1;
    errorRow1 = errorRow2;
    errorRow2 = temp;
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    if (!isValid()) return;
    memset(errorRows, 0, (width + 4) * 3 * sizeof(int16_t));
  }

 private:
  int width;
  int16_t* errorRows = nullptr;
  int16_t* errorRow0 = nullptr;
  int16_t* errorRow1 = nullptr;
  int16_t* errorRow2 = nullptr;
};

uint8_t epubWebRgb565ToGray8Rounded(uint16_t rgb565LittleEndian);
void epubWebContainDimensionsFloor(int srcW, int srcH, int maxW, int maxH, int* outW, int* outH);
void epubWebWrite2BitBmpHeader(Print& bmpOut, int width, int height);

struct EpubWeb2BitRowPacker {
  int dw = 0;
  int bytesPerRow = 0;
  uint8_t* rowBuffer = nullptr;
  FourToneImageDitherer* ditherer = nullptr;
  int rowIndex = 0;

  bool init(int width);
  void freeBuffers();
  bool writeGrayRow(Print& bmpOut, const uint8_t* grayRow);
};
