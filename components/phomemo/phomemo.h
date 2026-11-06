#pragma once

#include "esphome/components/label_printer/driver.h"


namespace esphome::phomemo {

// The Phomemo M series protocol: ESC/POS-like commands and a raster image,
// written to characteristic ff02, with "printed" reported on ff03. Models
// differ in head width and their start and end bytes; only the M220 has been
// printed on.
class PhomemoDriver : public label_printer::LabelDriver {
 public:
  enum Model : uint8_t { M220 };
  void set_model(Model model) { this->model_ = model; }

  const label_printer::PrinterInfo &info() const override;
  const label_printer::BleProfile *ble_profile() const override;

  bool begin(const label_printer::LabelRaster &raster) override;
  size_t next(uint8_t *buf, size_t max) override;
  size_t size() const override { return this->size_; }
  label_printer::Result on_reply(const uint8_t *data, size_t len) override;
  void end() override;

 protected:
  // The job is read straight from the raster, between a few bytes of
  // commands, so it needs no buffer of its own.
  enum Part : uint8_t { INIT, DENSITY, HEADER, IMAGE, FEED, PARTS };
  const uint8_t *part_data_(uint8_t part) const;
  size_t part_size_(uint8_t part) const;

  Model model_{M220};
  const label_printer::LabelRaster *raster_{nullptr};
  uint8_t header_[8]{};
  uint8_t part_{PARTS};
  size_t part_pos_{0};
  size_t size_{0};
  uint8_t done_match_{0};  // bytes of the "printed" status matched so far
};

}  // namespace esphome::phomemo
