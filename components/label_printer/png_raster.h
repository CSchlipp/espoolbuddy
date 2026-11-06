#pragma once

#include "types.h"

struct _pngle_t;

namespace esphome::label_printer {

// Decodes a PNG into a LabelRaster, a slice at a time so the UI stays
// responsive. The image is placed in the print row by an offset from the
// centre, clamped so it stays on the head.
class PngRasterizer {
 public:
  ~PngRasterizer() { this->end(); }
  // row_dots: the print row (a multiple of 8). x_offset: dots right of centre.
  bool begin(LabelRaster *out, uint16_t row_dots, uint16_t max_rows, int x_offset);
  // Feeds up to len bytes: how many were taken, or -1 on a decode error.
  int feed(const uint8_t *data, size_t len);
  // The header was read, the image fits the row, and the PNG ended (IEND).
  bool ok() const { return this->ok_ && this->complete_; }
  const char *error() const;
  void end();

  // Called by pngle's callbacks.
  void on_init(uint32_t w, uint32_t h);
  void on_pixel(uint32_t x, uint32_t y, const uint8_t rgba[4]);
  void on_done() { this->complete_ = true; }

 protected:
  _pngle_t *pngle_{nullptr};
  LabelRaster *out_{nullptr};
  uint16_t row_dots_{0};
  uint16_t max_rows_{0};
  int x_offset_{0};
  bool ok_{false};
  bool rejected_{false};  // the header said it doesn't fit: stop decoding
  bool complete_{false};
  uint16_t img_w_{0}, img_h_{0};
  int x0_{0};
};

}  // namespace esphome::label_printer
