#include "bambuddy_nfc_spi.h"
#ifdef USE_BAMBUDDY_NFC_SPI

#include "esphome/core/hal.h"

namespace esphome {
namespace bambuddy_nfc {

void BambuddyNFCSPIComponent::transport_wakeup() {
  // PN532 UM10232 §7.2.11: assert /SS (CS) low for at least 10ms to wake the
  // PN532 from H_0 (power-down) or any unknown state after power-on / ESP32 reset.
  // No SPI clock activity should occur during this window.
  this->cs_->digital_write(false);
  delay(15);  // spec minimum is 10ms; 15ms gives a comfortable margin
  this->cs_->digital_write(true);
}

bool BambuddyNFCSPIComponent::transport_read_status(uint8_t &status) {
  this->enable();
  delay(2);  // CS setup time: PN532 needs 2ms after /SS assertion before first clock
  this->write_byte(PN532_SPI_STATREAD);
  status = this->read_byte();
  this->disable();
  return true;
}

bool BambuddyNFCSPIComponent::transport_read_raw(uint8_t *data, size_t len) {
  this->enable();
  delay(2);
  this->write_byte(PN532_SPI_DATAREAD);
  this->read_array(data, len);
  this->disable();
  return true;
}

bool BambuddyNFCSPIComponent::transport_write_frame(
    const std::vector<uint8_t> &frame) {
  this->enable();
  delay(2);  // CS setup time before first clock edge
  this->write_byte(PN532_SPI_DATAWRITE);
  this->write_array(frame.data(), frame.size());
  this->disable();
  return true;
}

bool BambuddyNFCSPIComponent::transport_read_response(
    std::vector<uint8_t> &resp) {
  // The PN532 SPI state machine resets on every /SS de-assertion, so the
  // entire response frame — header and body — must be read within a SINGLE
  // /SS assertion.  Splitting into two transport_read_raw() calls (each
  // toggles /SS independently) causes the body read to receive garbage.
  this->enable();
  delay(2);  // CS setup time before first clock edge
  this->write_byte(PN532_SPI_DATAREAD);

  // Read header: preamble(1) + start(2) + len(1) + lcs(1) + tfi(1)
  // header[0] = preamble (0x00)
  // header[1] = start1 (0x00)
  // header[2] = start2 (0xFF)
  // header[3] = LEN
  // header[4] = LCS
  // header[5] = TFI (0xD5)
  uint8_t header[6];
  this->read_array(header, sizeof(header));

  uint8_t len = header[3];
  if (len < 1) {
    this->disable();
    return false;
  }

  // Read data bytes (len-1 after TFI) + DCS + postamble — still within same /SS
  size_t data_len = (size_t)(len - 1);
  std::vector<uint8_t> data(data_len + 2);  // +2 for DCS + POSTAMBLE
  this->read_array(data.data(), data.size());
  this->disable();

  resp.assign(data.begin(), data.begin() + data_len);
  return true;
}

}  // namespace bambuddy_nfc
}  // namespace esphome

#endif  // USE_BAMBUDDY_NFC_SPI
