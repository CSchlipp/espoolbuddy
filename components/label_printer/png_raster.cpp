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

// The most a label is shrunk to fit the row: 85%. Beyond that it's the wrong
// template for the printer.
static const uint32_t MIN_SCALE_PERCENT = 85;

void PngRasterizer::on_init(uint32_t w, uint32_t h) {
  ESP_LOGD(TAG, "Label is %ux%u dots", (unsigned) w, (unsigned) h);
  this->num_ = this->den_ = 1;
  if (w > this->row_dots_ && w * MIN_SCALE_PERCENT <= (uint32_t) this->row_dots_ * 100) {
    this->num_ = this->row_dots_;
    this->den_ = w;
  }
  const uint32_t out_w = w == 0 ? 0 : this->scaled_(w - 1) + 1;
  const uint32_t out_h = h == 0 ? 0 : this->scaled_(h - 1) + 1;
  if (out_w == 0 || out_w > this->row_dots_ || out_h == 0 || out_h > this->max_rows_) {
    this->ok_ = false;
    this->rejected_ = true;
    return;
  }
  if (this->num_ != this->den_) {
    ESP_LOGI(TAG, "Shrinking the label to %ux%u dots (%u%%) to fit the printer", (unsigned) out_w, (unsigned) out_h,
             (unsigned) (out_w * 100 / w));
  }
  this->img_w_ = w;
  this->img_h_ = h;
  int x0 = ((int) this->row_dots_ - (int) out_w) / 2 + this->x_offset_;
  this->x0_ = std::max(0, std::min(x0, (int) (this->row_dots_ - out_w)));
  this->out_->row_bytes = this->row_dots_ / 8;
  this->out_->width = this->row_dots_;
  this->out_->height = out_h;
  this->out_->bits.assign((size_t) this->out_->row_bytes * out_h, 0);
  this->ok_ = true;
}

void PngRasterizer::on_pixel(uint32_t x, uint32_t y, const uint8_t rgba[4]) {
  if (!this->ok_ || x >= this->img_w_ || y >= this->img_h_) return;
  if (!this->sampled_(x) || !this->sampled_(y)) return;
  // Transparent is paper; otherwise dark enough is a dot.
  if (rgba[3] < 128) return;
  uint32_t lum = (rgba[0] * 299u + rgba[1] * 587u + rgba[2] * 114u) / 1000u;
  if (lum >= 128) return;
  const uint32_t col = this->x0_ + this->scaled_(x);
  const uint32_t row = this->scaled_(y);
  this->out_->bits[(size_t) row * this->out_->row_bytes + col / 8] |= 0x80 >> (col % 8);
}

}  // namespace esphome::label_printer
