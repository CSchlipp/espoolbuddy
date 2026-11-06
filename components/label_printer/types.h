#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::label_printer {

// How a print ended. Drivers map what their printer reports onto these, so
// the UI never needs to know the model. Values are never renumbered.
enum class Result : uint8_t {
  NONE,
  PRINTED,       // the printer said the label is out
  SENT,          // every byte went out; the printer didn't (or can't) confirm
  NO_LABEL,      // Bambuddy didn't send a label
  BAD_LABEL,     // the label isn't a PNG that fits this printer
  NOT_FOUND,     // no printer answered
  LOST,          // the connection dropped mid-label
  WRITE_FAILED,  // the printer stopped taking bytes
  COVER_OPEN,
  NO_PAPER,
  PRINTER_ERROR,  // anything else the printer reported
};

// Words for the log and for a button.
const char *result_log_text(Result result);
const char *result_button_text(Result result);

// A 1-bit label image a whole print row wide: row_bytes per row, MSB first,
// 1 = black. The core fills it from Bambuddy's PNG; a driver turns it into
// its printer's bytes.
struct LabelRaster {
  uint16_t width{0};      // dots across (row_bytes * 8)
  uint16_t height{0};     // rows, along the feed
  uint16_t row_bytes{0};
  std::vector<uint8_t> bits;

  const uint8_t *row(uint16_t y) const { return this->bits.data() + (size_t) y * this->row_bytes; }
  void clear() {
    std::vector<uint8_t>().swap(this->bits);
    this->width = this->height = this->row_bytes = 0;
  }
};

}  // namespace esphome::label_printer
