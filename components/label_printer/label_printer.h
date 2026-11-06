#pragma once

#include "esphome/core/component.h"
#include "esphome/components/bambuddy_api/bambuddy_api.h"

#include "driver.h"
#include "png_raster.h"
#include "transport.h"

#include <cstring>
#include <string>

#ifdef USE_LVGL
#include <lvgl.h>
#endif

namespace esphome::label_printer {

// Prints one spool label at a time, on whatever printer the driver and
// transport describe:
//   1. Bambuddy draws the label as a PNG (fetched on bambuddy_api's HTTP
//      task); the transport connects in parallel.
//   2. The PNG is decoded a slice per loop into a 1-bit raster the width of
//      the print row, placed by the x offset.
//   3. The transport sends the driver's bytes; the driver reads the
//      printer's replies for "printed" or an error.
// Everything runs on the main loop. The UI talks only to this class.
class LabelPrinter : public Component, public ReplyHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_api(bambuddy_api::BambuddyAPIComponent *api) { this->api_ = api; }
  void set_driver(LabelDriver *driver) { this->driver_ = driver; }
  void set_transport(LabelTransport *transport) { this->transport_ = transport; }
  void set_label_template(const std::string &tmpl) { this->template_ = tmpl; }
  void set_label_fields(const std::string &fields) { this->fields_ = fields; }
  void set_x_offset(int dots) { this->x_offset_ = dots; }

  // Starts printing this spool's label. False while another print runs.
  bool print_spool(int spool_id);
  bool busy() const { return this->state_ != State::IDLE; }
  // A few words for this spool's Print button: how its print is going, or
  // how it ended (for a few seconds), or that another spool's label is
  // printing. Empty when there's nothing to say.
  const char *status_text(int spool_id) const;
  // The button's whole text: the status, or "Print Label".
  const char *button_text(int spool_id) const {
    const char *s = this->status_text(spool_id);
    return s[0] ? s : "Print Label";
  }

  void on_reply(const uint8_t *data, size_t len) override;

 protected:
  enum class State : uint8_t { IDLE, FETCH, DECODE, CONNECT, SEND, WAIT_DONE };

  void finish_(Result result);

  bambuddy_api::BambuddyAPIComponent *api_{nullptr};
  LabelDriver *driver_{nullptr};
  LabelTransport *transport_{nullptr};
  std::string template_{"box_40x30"};
  std::string fields_;  // comma-separated; empty = Bambuddy's default
  int x_offset_{0};

  State state_{State::IDLE};
  Result result_{Result::NONE};
  uint32_t state_ms_{0};
  uint32_t result_ms_{0};
  int spool_id_{0};

  std::string png_;
  size_t png_pos_{0};
  PngRasterizer rasterizer_;
  LabelRaster raster_;
};

extern LabelPrinter *global_label_printer;  // NOLINT

#ifdef USE_LVGL
// The text for this spool's Print button when label shows something else,
// nullptr when it's already right. small: it's a status, longer than "Print
// Label", so the UI shows it in a smaller font.
inline const char *button_text_update(lv_obj_t *label, int spool_id, bool *small) {
  if (global_label_printer == nullptr) return nullptr;
  const char *status = global_label_printer->status_text(spool_id);
  const char *text = status[0] ? status : "Print Label";
  *small = status[0] != '\0';
  return strcmp(lv_label_get_text(label), text) == 0 ? nullptr : text;
}
#endif

}  // namespace esphome::label_printer
