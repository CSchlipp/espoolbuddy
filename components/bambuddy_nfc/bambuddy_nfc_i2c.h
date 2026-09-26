#pragma once
// Every header in the component directory is pulled into the build, even for
// the interface that is not configured — so this one compiles to nothing
// unless __init__.py selected `interface: i2c`.
#include "esphome/core/defines.h"
#ifdef USE_BAMBUDDY_NFC_I2C

#include "bambuddy_nfc.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace bambuddy_nfc {

// Bytes read in one go for a response frame (status byte excluded). Covers
// every response the reader logic uses — a 16-byte MIFARE/NTAG read is a
// 26-byte frame, InListPassiveTarget with a 7-byte UID 21 bytes — so the
// common case is a single bus transaction. Longer frames (e.g. an ISO-DEP
// card's ATS) fall back to a NACK and a full-length re-read.
static constexpr size_t PN532_I2C_FAST_READ_LEN = 32;

/**
 * BambuddyNFCI2CComponent — PN532 over I2C (default address 0x24).
 * All reader logic lives in BambuddyNFCComponent; this only moves bytes.
 *
 * I2C differs from SPI in two ways that matter here:
 *   - every read starts with the PN532 status byte, so a read issued before
 *     the PN532 is ready returns 0x00 followed by garbage;
 *   - a read cannot be continued — each one starts at the frame's beginning
 *     again. Reading the header first to learn the length would cost a NACK
 *     (retransmit request), another ready-wait and a second read, on every
 *     one of the ~ten exchanges of a Bambu read. Reading a fixed-size buffer
 *     once avoids that.
 */
class BambuddyNFCI2CComponent : public BambuddyNFCComponent,
                                public i2c::I2CDevice {
 public:
  const char *get_connection_type() const override { return "I2C"; }

 protected:
  void transport_setup() override {}  // the i2c: bus component owns the bus
  void dump_transport_config() override {
    ESP_LOGCONFIG(NFC_TAG, "  I2C Address: 0x%02X", this->get_i2c_address());
  }
  void transport_wakeup() override;
  bool transport_read_status(uint8_t &status) override;
  bool transport_read_raw(uint8_t *data, size_t len) override;
  bool transport_write_frame(const std::vector<uint8_t> &frame) override;
  bool transport_read_response(std::vector<uint8_t> &resp) override;
  // A 1-byte status read is ~0.2 ms at 100 kHz with no CS setup time, so
  // poll tightly: this wait sits twice in every exchange of a Bambu read.
  uint32_t status_poll_delay_ms() const override { return 1; }

  // Validate a response frame (status byte already stripped) and extract the
  // payload after TFI. Sets needed_len when the buffer is too short.
  static bool parse_frame(const uint8_t *frame, size_t frame_len,
                          std::vector<uint8_t> &resp, size_t &needed_len);
};

}  // namespace bambuddy_nfc
}  // namespace esphome

#endif  // USE_BAMBUDDY_NFC_I2C
