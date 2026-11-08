#pragma once

#include "esphome/components/label_printer/driver.h"

#include <cstddef>
#include <cstdint>

namespace esphome::label_printer_brother_ql {

// The Brother QL raster protocol for the 720-dot (300 dpi) models: QL-500 to
// QL-580N, QL-700, QL-710W, QL-720NW and QL-800 to QL-820NWB. Continuous
// (DK endless) rolls only, printed black, cut after each label.
//
// The core's raster is the roll's printable width (rounded down to whole
// bytes); every row goes out as a 90-byte line for the whole head, placed
// between the roll's margins and mirrored, because the head prints its first
// dot at the label's right edge. The printer answers with 32-byte status
// frames: "printing completed", or an error.
class BrotherQLDriver : public label_printer::LabelDriver {
 public:
  // Continuous roll widths in mm.
  enum Media : uint8_t { MEDIA_12, MEDIA_29, MEDIA_38, MEDIA_50, MEDIA_54, MEDIA_62 };
  void set_media(Media media);
  void set_cut(bool cut) { this->cut_ = cut; }
  void set_model(const char *model) { this->info_.name = model; }

  const label_printer::PrinterInfo &info() const override { return this->info_; }

  bool begin(const label_printer::LabelRaster &raster) override;
  size_t next(uint8_t *buf, size_t max) override;
  size_t size() const override { return this->size_; }
  label_printer::Result on_reply(const uint8_t *data, size_t len) override;
  void end() override;

  // For the log: the roll's width in mm and the dots it prints.
  uint8_t media_mm() const;
  uint16_t printable_dots() const;

 protected:
  static constexpr uint16_t HEAD_DOTS = 720;
  static constexpr uint16_t LINE_BYTES = HEAD_DOTS / 8;  // 90
  static constexpr size_t LINE_SIZE = 3 + LINE_BYTES;    // 'g' 0x00 90, then the dots
  static constexpr size_t STATUS_SIZE = 32;

  enum Part : uint8_t {
    FIRST_MODE,
    INVALIDATE,
    INIT,
    MODE,
    STATUS_REQUEST,
    MEDIA_INFO,
    AUTOCUT,
    CUT_EVERY,
    EXPANDED,
    MARGIN,
    IMAGE,
    PRINT,
    PARTS,
  };
  const uint8_t *part_data_(uint8_t part) const;
  size_t part_size_(uint8_t part) const;
  void build_line_(uint16_t row);
  label_printer::Result on_status_();

  Media media_{MEDIA_62};
  bool cut_{true};
  label_printer::PrinterInfo info_{"Brother QL", 300, 696, 3000, 1024, 0, true};
  // Where the raster's first dot lands, counted from the label's right edge
  // (the head's first dot), and so mirrored.
  uint16_t right_dots_{12};

  const label_printer::LabelRaster *raster_{nullptr};
  uint8_t media_info_[13]{};
  uint8_t autocut_[4]{};
  uint8_t expanded_[4]{};
  uint8_t line_[LINE_SIZE]{};
  int32_t line_row_{-1};  // the row in line_
  uint8_t part_{PARTS};
  size_t part_pos_{0};
  size_t size_{0};

  uint8_t status_[STATUS_SIZE]{};
  uint8_t status_len_{0};
};

}  // namespace esphome::label_printer_brother_ql
