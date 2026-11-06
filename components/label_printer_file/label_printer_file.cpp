#include "label_printer_file.h"

#ifdef USE_HOST

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace esphome::label_printer_file {

static const char *const TAG = "label_printer_file";

void LabelPrinterFile::dump_config() {
  ESP_LOGCONFIG(TAG, "Label printer to a file:\n  Path: %s\n  Head: %u dots", this->path_.c_str(),
                this->info_.head_dots);
}

void LabelPrinterFile::set_head_dots(uint16_t dots) { this->info_.head_dots = dots; }

bool LabelPrinterFile::begin(const label_printer::LabelRaster &raster) {
  if (raster.row_bytes == 0 || raster.bits.size() != (size_t) raster.row_bytes * raster.height) return false;
  this->raster_ = &raster;
  this->header_ = "P4\n" + std::to_string(raster.width) + " " + std::to_string(raster.height) + "\n";
  this->pos_ = 0;
  return true;
}

size_t LabelPrinterFile::next(uint8_t *buf, size_t max) {
  size_t n = 0;
  while (n < max && this->pos_ < this->size()) {
    const bool in_header = this->pos_ < this->header_.size();
    const uint8_t *src = in_header ? reinterpret_cast<const uint8_t *>(this->header_.data()) + this->pos_
                                   : this->raster_->bits.data() + (this->pos_ - this->header_.size());
    const size_t left = in_header ? this->header_.size() - this->pos_ : this->size() - this->pos_;
    const size_t take = std::min(max - n, left);
    memcpy(buf + n, src, take);
    n += take;
    this->pos_ += take;
  }
  return n;
}

void LabelPrinterFile::send() {
  FILE *f = fopen(this->path_.c_str(), "wb");
  if (f == nullptr) {
    ESP_LOGW(TAG, "Can't write %s", this->path_.c_str());
    this->send_state_ = SendState::FAILED;
    return;
  }
  std::vector<uint8_t> buf(this->info_.max_write);
  size_t total = 0, n;
  bool ok = true;
  while ((n = this->driver_->next(buf.data(), buf.size())) > 0) {
    ok = ok && fwrite(buf.data(), 1, n, f) == n;
    total += n;
  }
  ok = fclose(f) == 0 && ok;
  if (ok) ESP_LOGI(TAG, "Wrote the label to %s (%u bytes)", this->path_.c_str(), (unsigned) total);
  this->send_state_ = ok ? SendState::DONE : SendState::FAILED;
}

}  // namespace esphome::label_printer_file

#endif  // USE_HOST
