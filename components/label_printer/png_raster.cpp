#include "png_raster.h"

#include "esphome/core/log.h"

#include <pngle.h>

#include <algorithm>

namespace esphome::label_printer {

static const char *const TAG = "label_printer";

static void png_init_cb(pngle_t *p, uint32_t w, uint32_t h) {
  static_cast<PngRasterizer *>(pngle_get_user_data(p))->on_init(w, h);
}
static void png_done_cb(pngle_t *p) { static_cast<PngRasterizer *>(pngle_get_user_data(p))->on_done(); }
static void png_draw_cb(pngle_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  auto *self = static_cast<PngRasterizer *>(pngle_get_user_data(p));
  // w/h > 1 only for interlaced passes: fill the block.
  for (uint32_t dy = 0; dy < h; dy++)
    for (uint32_t dx = 0; dx < w; dx++) self->on_pixel(x + dx, y + dy, rgba);
}

bool PngRasterizer::begin(LabelRaster *out, uint16_t row_dots, uint16_t max_rows, int x_offset) {
  this->end();
  this->pngle_ = pngle_new();
  if (this->pngle_ == nullptr) return false;
  pngle_set_user_data(this->pngle_, this);
  pngle_set_init_callback(this->pngle_, png_init_cb);
  pngle_set_draw_callback(this->pngle_, png_draw_cb);
  pngle_set_done_callback(this->pngle_, png_done_cb);
  this->out_ = out;
  this->row_dots_ = row_dots;
  this->max_rows_ = max_rows;
  this->x_offset_ = x_offset;
  this->ok_ = false;
  this->rejected_ = false;
  this->complete_ = false;
  return true;
}

int PngRasterizer::feed(const uint8_t *data, size_t len) {
  if (this->pngle_ == nullptr || this->rejected_) return -1;
  int eaten = pngle_feed(this->pngle_, data, len);
  // A label that doesn't fit is refused at its header, not after decoding it.
  return this->rejected_ ? -1 : eaten;
}

const char *PngRasterizer::error() const {
  if (this->rejected_) return "label doesn't fit the printer";
  return this->pngle_ != nullptr ? pngle_error(this->pngle_) : "not started";
}

void PngRasterizer::end() {
  if (this->pngle_ != nullptr) {
    pngle_destroy(this->pngle_);
    this->pngle_ = nullptr;
  }
}

void PngRasterizer::on_init(uint32_t w, uint32_t h) {
  ESP_LOGD(TAG, "Label is %ux%u dots", (unsigned) w, (unsigned) h);
  if (w == 0 || w > this->row_dots_ || h == 0 || h > this->max_rows_) {
    this->ok_ = false;
    this->rejected_ = true;
    return;
  }
  this->img_w_ = w;
  this->img_h_ = h;
  int x0 = ((int) this->row_dots_ - (int) w) / 2 + this->x_offset_;
  this->x0_ = std::max(0, std::min(x0, (int) (this->row_dots_ - w)));
  this->out_->row_bytes = this->row_dots_ / 8;
  this->out_->width = this->row_dots_;
  this->out_->height = h;
  this->out_->bits.assign((size_t) this->out_->row_bytes * h, 0);
  this->ok_ = true;
}

void PngRasterizer::on_pixel(uint32_t x, uint32_t y, const uint8_t rgba[4]) {
  if (!this->ok_ || x >= this->img_w_ || y >= this->img_h_) return;
  // Transparent is paper; otherwise dark enough is a dot.
  if (rgba[3] < 128) return;
  uint32_t lum = (rgba[0] * 299u + rgba[1] * 587u + rgba[2] * 114u) / 1000u;
  if (lum >= 128) return;
  uint32_t col = this->x0_ + x;
  this->out_->bits[(size_t) y * this->out_->row_bytes + col / 8] |= 0x80 >> (col % 8);
}

}  // namespace esphome::label_printer
