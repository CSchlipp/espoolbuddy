#pragma once

#ifdef USE_HOST

#include "esphome/core/component.h"
#include "esphome/components/label_printer/driver.h"
#include "esphome/components/label_printer/transport.h"

#include <cstdio>
#include <string>

namespace esphome::label_printer_file {

// The desktop simulator's label printer: writes each label as a PBM image
// (the raster as it is: 1 bit per dot, MSB first, 1 = black) to a file, so
// placement and layout can be checked without paper. It's the driver and
// the transport at once.
class LabelPrinterFile : public Component, public label_printer::LabelDriver, public label_printer::LabelTransport {
 public:
  void dump_config() override;
  void set_path(const std::string &path) { this->path_ = path; }
  void set_head_dots(uint16_t dots);

  // Driver.
  const label_printer::PrinterInfo &info() const override { return this->info_; }
  bool begin(const label_printer::LabelRaster &raster) override;
  size_t next(uint8_t *buf, size_t max) override;
  size_t size() const override { return this->header_.size() + (this->raster_ ? this->raster_->bits.size() : 0); }
  label_printer::Result on_reply(const uint8_t *data, size_t len) override { return label_printer::Result::NONE; }
  void end() override { this->raster_ = nullptr; }

  // Transport.
  void open() override {}
  bool ready() const override { return true; }
  bool has_replies() const override { return false; }
  void send() override;
  SendState send_state() const override { return this->send_state_; }
  void close(bool unreachable) override { this->send_state_ = SendState::IDLE; }

 protected:
  std::string path_{"label.pbm"};
  label_printer::PrinterInfo info_{"PBM file", 203, 576, 1200, 4096, 0, false};
  const label_printer::LabelRaster *raster_{nullptr};
  std::string header_;
  size_t pos_{0};
  SendState send_state_{SendState::IDLE};
};

}  // namespace esphome::label_printer_file

#endif  // USE_HOST
