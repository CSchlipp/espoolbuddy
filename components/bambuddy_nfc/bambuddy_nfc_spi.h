#pragma once
// Every header in the component directory is pulled into the build, even for
// the interface that is not configured — so this one compiles to nothing
// unless __init__.py selected `interface: spi`.
#include "esphome/core/defines.h"
#ifdef USE_BAMBUDDY_NFC_SPI

#include "bambuddy_nfc.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace bambuddy_nfc {

// PN532 SPI command bytes (first byte of every /SS assertion)
static constexpr uint8_t PN532_SPI_DATAWRITE = 0x01;
static constexpr uint8_t PN532_SPI_STATREAD  = 0x02;
static constexpr uint8_t PN532_SPI_DATAREAD  = 0x03;

/**
 * BambuddyNFCSPIComponent — PN532 over 4-wire SPI (mode 0, LSB first, 1 MHz).
 * All reader logic lives in BambuddyNFCComponent; this only moves bytes.
 */
class BambuddyNFCSPIComponent
    : public BambuddyNFCComponent,
      public spi::SPIDevice<spi::BIT_ORDER_LSB_FIRST, spi::CLOCK_POLARITY_LOW,
                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  const char *get_connection_type() const override { return "SPI"; }

 protected:
  void transport_setup() override { this->spi_setup(); }
  void dump_transport_config() override { log_pin(NFC_TAG, "  CS Pin: ", this->cs_); }
  void transport_wakeup() override;
  bool transport_read_status(uint8_t &status) override;
  bool transport_read_raw(uint8_t *data, size_t len) override;
  bool transport_write_frame(const std::vector<uint8_t> &frame) override;
  bool transport_read_response(std::vector<uint8_t> &resp) override;
  // Each status read already holds /SS for its 2 ms setup time.
  uint32_t status_poll_delay_ms() const override { return 5; }
};

}  // namespace bambuddy_nfc
}  // namespace esphome

#endif  // USE_BAMBUDDY_NFC_SPI
