#include "bambuddy_nfc_i2c.h"
#ifdef USE_BAMBUDDY_NFC_I2C

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>

// Based on the PN532 user manual (UM0701-02 §6.2.4, I2C) and ESPHome's own
// pn532_i2c component.

namespace esphome {
namespace bambuddy_nfc {

// Sent to make the PN532 retransmit its last response frame.
static const uint8_t PN532_NACK[] = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};

void BambuddyNFCI2CComponent::transport_wakeup() {
  // On I2C the PN532 wakes from power-down on its own address match, but it
  // may not acknowledge that first transfer — so address it once and ignore
  // the result, then give it the same settle time the SPI wakeup gets.
  uint8_t dummy = 0;
  this->read(&dummy, 1);
  delay(15);
}

bool BambuddyNFCI2CComponent::transport_read_status(uint8_t &status) {
  uint8_t b = 0;
  if (this->read(&b, 1) != i2c::ERROR_OK) return false;
  // Only bit 0 is defined (1 = response ready).
  status = (b & 0x01) ? PN532_READY : 0x00;
  return true;
}

bool BambuddyNFCI2CComponent::transport_read_raw(uint8_t *data, size_t len) {
  std::vector<uint8_t> buf(len + 1);  // +1: leading status byte
  if (this->read(buf.data(), buf.size()) != i2c::ERROR_OK) return false;
  if (!(buf[0] & 0x01)) return false;  // not ready: the rest is garbage
  memcpy(data, buf.data() + 1, len);
  return true;
}

bool BambuddyNFCI2CComponent::transport_write_frame(
    const std::vector<uint8_t> &frame) {
  if (this->write(frame.data(), frame.size()) != i2c::ERROR_OK) return false;
  // Let the PN532 take the frame before the first status poll.
  delay(1);
  return true;
}

bool BambuddyNFCI2CComponent::parse_frame(const uint8_t *frame,
                                          size_t frame_len,
                                          std::vector<uint8_t> &resp,
                                          size_t &needed_len) {
  needed_len = 0;
  // frame[0] = preamble (0x00)
  // frame[1] = start1 (0x00)
  // frame[2] = start2 (0xFF)
  // frame[3] = LEN (TFI + payload)
  // frame[4] = LCS
  // frame[5] = TFI (0xD5)
  // frame[6 .. 6+LEN-2] = payload, then DCS, POSTAMBLE
  if (frame_len < 6) return false;
  if (frame[0] != PN532_PREAMBLE || frame[1] != PN532_STARTCODE1 ||
      frame[2] != PN532_STARTCODE2) {
    ESP_LOGV(NFC_TAG, "I2C response: bad preamble");
    return false;
  }
  uint8_t len = frame[3];
  if (len < 1 || (uint8_t) (len + frame[4]) != 0) {
    ESP_LOGV(NFC_TAG, "I2C response: bad length checksum");
    return false;
  }
  if (frame[5] != PN532_TFI_PN532) {
    ESP_LOGV(NFC_TAG, "I2C response: bad TFI 0x%02X", frame[5]);
    return false;
  }
  size_t total = 5 + (size_t) len + 2;  // header(5) + TFI/payload + DCS + POSTAMBLE
  if (frame_len < total) {
    needed_len = total;
    return false;
  }
  uint8_t dcs = 0;
  for (size_t i = 5; i < 5 + (size_t) len + 1; i++) dcs += frame[i];
  if (dcs != 0) {
    ESP_LOGV(NFC_TAG, "I2C response: bad data checksum");
    return false;
  }
  resp.assign(frame + 6, frame + 5 + len);
  return true;
}

bool BambuddyNFCI2CComponent::transport_read_response(
    std::vector<uint8_t> &resp) {
  // Fast path: one read covers every frame the reader logic normally sees.
  uint8_t buf[PN532_I2C_FAST_READ_LEN + 1];  // +1: leading status byte
  if (this->read(buf, sizeof(buf)) != i2c::ERROR_OK) return false;
  if (!(buf[0] & 0x01)) return false;

  size_t needed_len = 0;
  if (parse_frame(buf + 1, PN532_I2C_FAST_READ_LEN, resp, needed_len))
    return true;
  if (needed_len == 0) return false;  // malformed, not merely long

  // Longer frame: have the PN532 send it again and read it in full.
  ESP_LOGV(NFC_TAG, "I2C response of %u bytes exceeds fast read, re-reading",
           (unsigned) needed_len);
  if (this->write(PN532_NACK, sizeof(PN532_NACK)) != i2c::ERROR_OK) return false;
  delay(1);
  if (!pn532_wait_ready(100)) return false;
  const size_t full_len = needed_len;
  std::vector<uint8_t> full(full_len + 1);
  if (this->read(full.data(), full.size()) != i2c::ERROR_OK) return false;
  if (!(full[0] & 0x01)) return false;
  return parse_frame(full.data() + 1, full_len, resp, needed_len);
}

}  // namespace bambuddy_nfc
}  // namespace esphome

#endif  // USE_BAMBUDDY_NFC_I2C
