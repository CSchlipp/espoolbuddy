#include "brother_ql.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::label_printer_brother_ql {

static const char *const TAG = "brother_ql";

using label_printer::Result;

// Continuous rolls on the 720-dot head: width in mm, the dots it prints, and
// the dots between the head's first dot and the printed area. From Brother's
// QL raster command reference, as tabled in the brother_ql project.
struct MediaSpec {
  uint8_t mm;
  uint16_t printable;
  uint16_t right_margin;
};
static const MediaSpec MEDIA_SPECS[] = {
    {12, 106, 29}, {29, 306, 6}, {38, 413, 12}, {50, 554, 12}, {54, 590, 0}, {62, 696, 12},
};

// Feed before and after a continuous label, in dots (3 mm).
static const uint16_t CONTINUOUS_MARGIN = 35;
// 1 m of 300 dpi rows; far longer than any Bambuddy label.
static const uint16_t MAX_ROWS = 11811;

// The job starts as brother_ql's does: raster mode first (a QL-8xx left in
// P-touch Template mode reads nothing else), zeros that clear whatever a
// half-sent job left in the printer's buffer, init, raster mode again, and a
// status request. 400 zeros, as the QL-8xx wants (200 for older models; the
// extra ones are ignored).
static const uint8_t CMD_INVALIDATE[400] = {};
static const uint8_t CMD_INIT[] = {0x1b, 0x40};                     // ESC @
static const uint8_t CMD_RASTER_MODE[] = {0x1b, 0x69, 0x61, 0x01};  // ESC i a 1
static const uint8_t CMD_STATUS_REQUEST[] = {0x1b, 0x69, 0x53};     // ESC i S
static const uint8_t CMD_CUT_EVERY[] = {0x1b, 0x69, 0x41, 0x01};    // ESC i A 1: every label
static const uint8_t CMD_MARGIN[] = {0x1b, 0x69, 0x64, CONTINUOUS_MARGIN & 0xff, CONTINUOUS_MARGIN >> 8};
static const uint8_t CMD_PRINT[] = {0x1a};  // print, and feed: the last page

// Status frame (32 bytes, 80 20 42 ...): error bits in bytes 8 and 9, what
// it reports in byte 18.
static const uint8_t ERR1_NO_MEDIA = 0x01;
static const uint8_t ERR1_END_OF_MEDIA = 0x02;
static const uint8_t ERR1_CUTTER_JAM = 0x04;
static const uint8_t ERR2_REPLACE_MEDIA = 0x01;
static const uint8_t ERR2_COVER_OPEN = 0x10;
static const uint8_t ERR2_FEED_ERROR = 0x40;
static const uint8_t STATUS_PRINTED = 0x01;
static const uint8_t STATUS_ERROR = 0x02;
static const uint8_t STATUS_TURNED_OFF = 0x04;

void BrotherQLDriver::set_media(Media media) {
  this->media_ = media;
  const MediaSpec &m = MEDIA_SPECS[media];
  // The core's raster is whole bytes wide; centre it in the printed area.
  const uint16_t row = m.printable & ~7u;
  this->info_.head_dots = row;
  this->info_.max_rows = MAX_ROWS;
  this->right_dots_ = m.right_margin + (m.printable - row) / 2;
}

uint8_t BrotherQLDriver::media_mm() const { return MEDIA_SPECS[this->media_].mm; }
uint16_t BrotherQLDriver::printable_dots() const { return MEDIA_SPECS[this->media_].printable; }

bool BrotherQLDriver::begin(const label_printer::LabelRaster &raster) {
  const uint16_t rows = raster.height;
  if (raster.row_bytes * 8 != this->info_.head_dots || rows == 0 || rows > MAX_ROWS ||
      raster.bits.size() != (size_t) raster.row_bytes * rows)
    return false;

  // ESC i z: which fields are valid (media type, width, length, quality,
  // recover), continuous, width in mm, length 0 (continuous), the raster
  // count, first page.
  const uint8_t media_info[] = {0x1b, 0x69, 0x7a, 0xce, 0x0a, this->media_mm(), 0x00,
                                (uint8_t) rows, (uint8_t) (rows >> 8), 0x00, 0x00, 0x00, 0x00};
  static_assert(sizeof(media_info) == sizeof(BrotherQLDriver::media_info_), "media info size");
  memcpy(this->media_info_, media_info, sizeof(media_info));
  // ESC i M: auto cut. ESC i K: cut at the end; not two-colour, not 600 dpi.
  const uint8_t autocut[] = {0x1b, 0x69, 0x4d, (uint8_t) (this->cut_ ? 0x40 : 0x00)};
  memcpy(this->autocut_, autocut, sizeof(autocut));
  const uint8_t expanded[] = {0x1b, 0x69, 0x4b, (uint8_t) (this->cut_ ? 0x08 : 0x00)};
  memcpy(this->expanded_, expanded, sizeof(expanded));

  this->raster_ = &raster;
  this->line_row_ = -1;
  this->size_ = 0;
  for (uint8_t p = 0; p < PARTS; p++) this->size_ += this->part_size_(p);
  this->part_ = FIRST_MODE;
  this->part_pos_ = 0;
  this->status_len_ = 0;
  return true;
}

void BrotherQLDriver::build_line_(uint16_t row) {
  // 'g' 0x00 n: n bytes of raster, uncompressed.
  this->line_[0] = 0x67;
  this->line_[1] = 0x00;
  this->line_[2] = LINE_BYTES;
  uint8_t *dots = this->line_ + 3;
  memset(dots, 0, LINE_BYTES);
  const uint8_t *src = this->raster_->row(row);
  const uint16_t width = this->info_.head_dots;
  // Raster dot x lands on head dot right_dots + width - 1 - x: mirrored.
  const uint16_t last = this->right_dots_ + width - 1;
  for (uint16_t x = 0; x < width; x++) {
    if ((src[x >> 3] & (0x80 >> (x & 7))) == 0) continue;
    const uint16_t d = last - x;
    dots[d >> 3] |= 0x80 >> (d & 7);
  }
  this->line_row_ = row;
}

const uint8_t *BrotherQLDriver::part_data_(uint8_t part) const {
  switch (part) {
    case INVALIDATE:
      return CMD_INVALIDATE;
    case INIT:
      return CMD_INIT;
    case FIRST_MODE:
    case MODE:
      return CMD_RASTER_MODE;
    case STATUS_REQUEST:
      return CMD_STATUS_REQUEST;
    case MEDIA_INFO:
      return this->media_info_;
    case AUTOCUT:
      return this->autocut_;
    case CUT_EVERY:
      return CMD_CUT_EVERY;
    case EXPANDED:
      return this->expanded_;
    case MARGIN:
      return CMD_MARGIN;
    case PRINT:
      return CMD_PRINT;
    default:
      return nullptr;  // IMAGE is built a line at a time
  }
}

size_t BrotherQLDriver::part_size_(uint8_t part) const {
  switch (part) {
    case INVALIDATE:
      return sizeof(CMD_INVALIDATE);
    case INIT:
      return sizeof(CMD_INIT);
    case FIRST_MODE:
    case MODE:
      return sizeof(CMD_RASTER_MODE);
    case STATUS_REQUEST:
      return sizeof(CMD_STATUS_REQUEST);
    case MEDIA_INFO:
      return sizeof(this->media_info_);
    case AUTOCUT:
      return sizeof(this->autocut_);
    case CUT_EVERY:
      return this->cut_ ? sizeof(CMD_CUT_EVERY) : 0;
    case EXPANDED:
      return sizeof(this->expanded_);
    case MARGIN:
      return sizeof(CMD_MARGIN);
    case IMAGE:
      return this->raster_ != nullptr ? (size_t) this->raster_->height * LINE_SIZE : 0;
    case PRINT:
      return sizeof(CMD_PRINT);
    default:
      return 0;
  }
}

size_t BrotherQLDriver::next(uint8_t *buf, size_t max) {
  size_t n = 0;
  while (n < max && this->part_ < PARTS) {
    const size_t part_size = this->part_size_(this->part_);
    size_t take;
    if (this->part_ == IMAGE) {
      // One line at a time, built from the raster row as it's reached.
      const uint16_t row = this->part_pos_ / LINE_SIZE;
      const size_t in_line = this->part_pos_ % LINE_SIZE;
      if (part_size > 0 && this->line_row_ != row) this->build_line_(row);
      take = std::min(max - n, LINE_SIZE - in_line);
      take = std::min(take, part_size - this->part_pos_);
      memcpy(buf + n, this->line_ + in_line, take);
    } else {
      take = std::min(max - n, part_size - this->part_pos_);
      if (take > 0) memcpy(buf + n, this->part_data_(this->part_) + this->part_pos_, take);
    }
    n += take;
    this->part_pos_ += take;
    if (this->part_pos_ >= part_size) {
      this->part_++;
      this->part_pos_ = 0;
    }
  }
  return n;
}

Result BrotherQLDriver::on_reply(const uint8_t *data, size_t len) {
  // Frames may arrive split, or several at once; each starts 80 20 42.
  for (size_t i = 0; i < len; i++) {
    const uint8_t b = data[i];
    if (this->status_len_ == 0 && b != 0x80) continue;
    if (this->status_len_ == 1 && b != 0x20) {
      this->status_len_ = b == 0x80 ? 1 : 0;
      continue;
    }
    this->status_[this->status_len_++] = b;
    if (this->status_len_ < STATUS_SIZE) continue;
    this->status_len_ = 0;
    Result r = this->on_status_();
    if (r != Result::NONE) return r;
  }
  return Result::NONE;
}

Result BrotherQLDriver::on_status_() {
  const uint8_t err1 = this->status_[8];
  const uint8_t err2 = this->status_[9];
  const uint8_t type = this->status_[18];
  ESP_LOGD(TAG, "Status: type %02X, phase %02X, errors %02X %02X, media %u mm (type %02X)", type,
           this->status_[19], err1, err2, this->status_[10], this->status_[11]);
  if (type == STATUS_TURNED_OFF) return Result::LOST;
  // Errors count whatever the frame: the reply to the status request (type
  // 0x00) may be the only one to report an error the printer already had.
  if (type != STATUS_ERROR && err1 == 0 && err2 == 0) {
    // Otherwise a reply, a notification or a phase change.
    return type == STATUS_PRINTED ? Result::PRINTED : Result::NONE;
  }
  if (err2 & ERR2_COVER_OPEN) return Result::COVER_OPEN;
  if ((err1 & (ERR1_NO_MEDIA | ERR1_END_OF_MEDIA)) || (err2 & ERR2_FEED_ERROR)) return Result::NO_PAPER;
  if (err2 & ERR2_REPLACE_MEDIA) {
    // Usually the roll isn't the one the label was made for.
    ESP_LOGW(TAG, "The printer has %u mm media loaded, the label is for %u mm", this->status_[10], this->media_mm());
  } else if (err1 & ERR1_CUTTER_JAM) {
    ESP_LOGW(TAG, "Cutter jam");
  }
  return Result::PRINTER_ERROR;
}

void BrotherQLDriver::end() {
  // The raster is the core's; it frees it after this.
  this->raster_ = nullptr;
  this->line_row_ = -1;
  this->part_ = PARTS;
  this->part_pos_ = 0;
  this->status_len_ = 0;
}

}  // namespace esphome::label_printer_brother_ql
