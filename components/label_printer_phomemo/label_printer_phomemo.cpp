#include "label_printer_phomemo.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::label_printer_phomemo {

static const char *const TAG = "label_printer_phomemo";

using label_printer::Result;

// 203 dpi head, 576 dots (72 mm) across; 150 mm is the longest stock it takes.
// The printer doesn't flow-control: without a pause between writes the link
// drops mid-raster. No write is longer than 128 bytes, what it takes at once.
static const label_printer::PrinterInfo M220_INFO = {
    "Phomemo M220", 203, 576, 1200, 128, 20, true,
};

// ff00 holds the write (ff02) and status (ff03) characteristics on the whole
// M series. An M220 advertises 0xAF30 and its own address as manufacturer data.
static const label_printer::BleProfile M220_BLE = {0xff00, 0xff02, 0xff03, 0xaf30, true};

// ESC @ resets, ESC 7 sets the heat (what the vendor app sends), GS v 0 is a
// raster image, ESC J feeds the label out past the tear bar. The printer says
// 1A 0F 0C on ff03 when the job has printed.
static const uint8_t CMD_INIT[] = {0x1b, 0x40};
static const uint8_t CMD_DENSITY[] = {0x1b, 0x37, 0x07, 0x64, 0x64};
static const uint8_t CMD_FEED[] = {0x1b, 0x4a, 0x20};
static const uint8_t STATUS_PRINTED[] = {0x1a, 0x0f, 0x0c};

const label_printer::PrinterInfo &PhomemoDriver::info() const { return M220_INFO; }

const label_printer::BleProfile *PhomemoDriver::ble_profile() const { return &M220_BLE; }

bool PhomemoDriver::begin(const label_printer::LabelRaster &raster) {
  const uint16_t row_bytes = raster.row_bytes;
  const uint16_t rows = raster.height;
  if (row_bytes == 0 || rows == 0 || row_bytes * 8 > M220_INFO.head_dots ||
      raster.bits.size() != (size_t) row_bytes * rows)
    return false;
  // GS v 0, mode 0: width in bytes, then height in rows, both little-endian.
  const uint8_t header[] = {0x1d, 0x76, 0x30, 0x00, (uint8_t) row_bytes, (uint8_t) (row_bytes >> 8),
                            (uint8_t) rows, (uint8_t) (rows >> 8)};
  memcpy(this->header_, header, sizeof(header));
  this->raster_ = &raster;
  this->size_ = 0;
  for (uint8_t p = 0; p < PARTS; p++) this->size_ += this->part_size_(p);
  this->part_ = INIT;
  this->part_pos_ = 0;
  this->done_match_ = 0;
  return true;
}

const uint8_t *PhomemoDriver::part_data_(uint8_t part) const {
  switch (part) {
    case INIT:
      return CMD_INIT;
    case DENSITY:
      return CMD_DENSITY;
    case HEADER:
      return this->header_;
    case IMAGE:
      return this->raster_->bits.data();
    case FEED:
      return CMD_FEED;
    default:
      return nullptr;
  }
}

size_t PhomemoDriver::part_size_(uint8_t part) const {
  switch (part) {
    case INIT:
      return sizeof(CMD_INIT);
    case DENSITY:
      return sizeof(CMD_DENSITY);
    case HEADER:
      return sizeof(this->header_);
    case IMAGE:
      return this->raster_->bits.size();
    case FEED:
      return sizeof(CMD_FEED);
    default:
      return 0;
  }
}

size_t PhomemoDriver::next(uint8_t *buf, size_t max) {
  // Fills the write across part boundaries, so each one is as full as the
  // old single buffer made it.
  size_t n = 0;
  while (n < max && this->part_ < PARTS) {
    const size_t left = this->part_size_(this->part_) - this->part_pos_;
    const size_t take = std::min(max - n, left);
    memcpy(buf + n, this->part_data_(this->part_) + this->part_pos_, take);
    n += take;
    this->part_pos_ += take;
    if (this->part_pos_ == this->part_size_(this->part_)) {
      this->part_++;
      this->part_pos_ = 0;
    }
  }
  return n;
}

Result PhomemoDriver::on_reply(const uint8_t *data, size_t len) {
  ESP_LOGV(TAG, "Status: %s", format_hex_pretty(data, len).c_str());
  // Look for 1A 0F 0C, which may arrive split or among other status bytes.
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    if (b == STATUS_PRINTED[this->done_match_]) {
      if (++this->done_match_ == sizeof(STATUS_PRINTED)) {
        this->done_match_ = 0;
        return Result::PRINTED;
      }
    } else {
      this->done_match_ = b == STATUS_PRINTED[0] ? 1 : 0;
    }
  }
  return Result::NONE;
}

void PhomemoDriver::end() {
  // The raster is the core's; it frees it after this.
  this->raster_ = nullptr;
  this->part_ = PARTS;
  this->part_pos_ = 0;
}

}  // namespace esphome::label_printer_phomemo
