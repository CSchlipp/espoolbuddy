#pragma once

#include "types.h"

namespace esphome::label_printer {

// What a printer model is, for the core and the transports.
struct PrinterInfo {
  const char *name;       // e.g. "Phomemo M220"
  uint16_t dpi;           // asked of Bambuddy, so the label prints dot for dot
  uint16_t head_dots;     // the print row, across the head
  uint16_t max_rows;      // the longest label it takes
  uint16_t max_write;     // bytes it takes in one write
  uint32_t write_gap_ms;  // pause between writes, for printers without flow control
  bool confirms;          // reports when a label has printed
};

// How a model shows up on Bluetooth: data, so a transport can connect to it
// and find it without knowing the model.
struct BleProfile {
  uint16_t service;      // GATT service with the characteristics below
  uint16_t write_char;   // takes the job's bytes (write without response)
  uint16_t notify_char;  // the printer's replies; 0 for none
  // Finding the printer with no address configured: an advertisement with
  // this 16-bit service (0: never search), and, if set, manufacturer data
  // that is the device's own address.
  uint16_t advertised_service;
  bool address_in_manufacturer_data;
};

// A printer's protocol. One job at a time: begin() with the raster (which
// the core keeps until end()), then next() until it returns 0. Replies from
// the printer come to on_reply() while the job is out.
class LabelDriver {
 public:
  virtual ~LabelDriver() = default;
  virtual const PrinterInfo &info() const = 0;
  virtual const BleProfile *ble_profile() const { return nullptr; }

  virtual bool begin(const LabelRaster &raster) = 0;
  // Copies up to max bytes of the job into buf; 0 once it's all out.
  virtual size_t next(uint8_t *buf, size_t max) = 0;
  // The whole job in bytes, for the log.
  virtual size_t size() const = 0;
  // NONE, or how the job ended (PRINTED, or what went wrong).
  virtual Result on_reply(const uint8_t *data, size_t len) = 0;
  virtual void end() = 0;
};

}  // namespace esphome::label_printer
