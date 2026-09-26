#pragma once
#include <cstdint>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "../bambuddy_api/bambuddy_api.h"
#include "bambu_colors.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace bambuddy_nfc {

static const char *const NFC_TAG = "bambuddy_nfc";

// PN532 frame bytes (identical on every host interface)
static constexpr uint8_t PN532_PREAMBLE   = 0x00;
static constexpr uint8_t PN532_STARTCODE1 = 0x00;
static constexpr uint8_t PN532_STARTCODE2 = 0xFF;
static constexpr uint8_t PN532_POSTAMBLE  = 0x00;
static constexpr uint8_t PN532_TFI_HOST   = 0xD4;  // Host → PN532
static constexpr uint8_t PN532_TFI_PN532  = 0xD5;  // PN532 → Host
static constexpr uint8_t PN532_READY  = 0x01;

// PN532 commands used
static constexpr uint8_t PN532_CMD_SAMCONFIGURATION  = 0x14;
static constexpr uint8_t PN532_CMD_INLISTPASSIVETARGET = 0x4A;
static constexpr uint8_t PN532_CMD_INDATAEXCHANGE    = 0x40;
static constexpr uint8_t PN532_CMD_GETFIRMWAREVERSION = 0x02;
static constexpr uint8_t PN532_CMD_RFCONFIGURATION   = 0x32;

// MIFARE commands
static constexpr uint8_t MFC_AUTH_KEY_A  = 0x60;
static constexpr uint8_t MFC_READ        = 0x30;

// NTAG commands
static constexpr uint8_t NTAG_WRITE = 0xA2;

// Bambu MIFARE Classic HKDF key derivation constants
// (ported from pico-nfc-bridge.ino / spoolbuddy pn5180.py)
static const uint8_t BAMBU_MASTER_KEY[16] = {
    0x9A, 0x75, 0x9C, 0xF2, 0xC4, 0xF7, 0xCA, 0xFF,
    0x22, 0x2C, 0xB9, 0x76, 0x9B, 0x41, 0xBC, 0x96,
};
// "RFID-A\0" — 7 bytes including null terminator
static const uint8_t BAMBU_CONTEXT[7] = {
    'R', 'F', 'I', 'D', '-', 'A', 0x00
};
// Bambu blocks to read for tray_uuid / material info.
// Blocks 1, 2, 4, 5 carry material identity and colour; block 6 adds the
// drying/bed/hotend temperatures.  All five live in sectors 0 and 1, so the
// extra block costs one read and no additional MIFARE authentication.
// Block 9 holds the Bambu tray UID — the identifier of the physical spool, as
// opposed to the tag's own card UID. It lives in sector 2, so reading it costs
// one extra authentication on top of the extra block read. Currently read for
// measurement only (logged, not yet used for matching).
static const uint8_t BAMBU_BLOCKS[] = {1, 2, 4, 5, 6, 9};

// Blocks the caller cannot do without: material identity and colour. A failure
// on any of these makes the scan useless, so it aborts the attempt.
// Everything else in BAMBU_BLOCKS is a bonus — block 6 adds temperatures and
// block 9 the tray UID. Losing one of those costs a detail, not the scan, so a
// failure there is logged and skipped rather than discarding blocks that were
// already read successfully. This matters because each additional sector costs
// another authentication, and the RF link is not always good for that long.
static const uint8_t BAMBU_BLOCKS_REQUIRED[] = {1, 2, 4, 5};

inline bool bambu_block_is_required(uint8_t block) {
  for (uint8_t b : BAMBU_BLOCKS_REQUIRED)
    if (b == block) return true;
  return false;
}

// A Bambu read is a long RF exchange — one authentication plus five block
// reads per sector run — and a single dropout anywhere in it aborts the whole
// series, losing the scan even though the spool is still sitting on the
// reader. Retry the series a few times while the same tag stays present.
static const uint8_t BAMBU_READ_ATTEMPTS = 4;
static const uint16_t BAMBU_RETRY_DELAY_MS = 30;

// ACK frame: sent by the PN532 to accept a command, and by the host to abort
// the command the PN532 is still executing.
static const uint8_t PN532_ACK_FRAME[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// Link supervision. A PN532 that stops acknowledging commands (brown-out
// reset, lost SAMConfiguration, host/reader frame desync) never comes back on
// its own, and every poll after that just fails silently — NFC looks dead
// until the next reboot. After this many consecutive exchanges without a
// valid ACK/response the reader is re-initialised; a failed attempt is
// retried after PN532_RECOVER_RETRY_MS.
static const uint8_t PN532_LINK_FAILURE_LIMIT = 5;
static const uint32_t PN532_RECOVER_RETRY_MS = 5000;
// A PN532 that did not answer at boot (not connected yet, slow to power up,
// wrong mode switches) is retried at this interval instead of leaving NFC
// dead until the next reboot.
static const uint32_t PN532_INIT_RETRY_MS = 10000;

enum class NFCState {
  IDLE,
  TAG_PRESENT,
};

/**
 * BambuddyNFCComponent
 *
 * Drives a PN532 NFC reader and implements:
 *   - ISO 14443A tag activation (MIFARE Classic + NTAG)
 *   - HKDF-SHA256 key derivation for Bambu Lab MIFARE Classic tags
 *   - Tag-presence state machine (detect, present, removed)
 *   - NTAG write support (triggered by pending_write in BambuddyAPIComponent)
 *   - Callbacks to BambuddyAPIComponent on scan/remove events
 *
 * Abstract: everything above the byte transport lives here and is shared by
 * both host interfaces. The concrete classes only move bytes —
 * BambuddyNFCSPIComponent (bambuddy_nfc_spi.h) and BambuddyNFCI2CComponent
 * (bambuddy_nfc_i2c.h) — and are chosen by the YAML `interface:` key.
 * bambuddy_api only ever sees this base class.
 */
class BambuddyNFCComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  // Printed whenever a log client connects, so the reader's state is visible
  // in every log session — setup() itself runs before WiFi/API are up.
  void dump_config() override;
  float get_setup_priority() const override {
    return setup_priority::DATA;
  }

  // Host interface name, "SPI" or "I2C" — reported to the backend by
  // bambuddy_api (nfc_connection) and used in the log lines here.
  virtual const char *get_connection_type() const = 0;

  void set_api_component(bambuddy_api::BambuddyAPIComponent *api) {
    api_ = api;
  }
  void set_poll_interval(uint32_t ms) { poll_interval_ms_ = ms; }
  void set_miss_threshold(uint8_t n) { miss_threshold_ = n; }
  // Optional IRQ pin (PN532 open-drain, active-LOW).
  // When set, pn532_wait_ready() watches this GPIO instead of polling the
  // status byte over the bus — eliminates ~100 bus transactions per no-tag
  // cycle.
  void set_irq_pin(GPIOPin *pin) { irq_pin_ = pin; }
  // Low-power mode: widen the detect cadence while the console sleeps (called by
  // the UI sleep state machine). The RF field is energized for most of each
  // detect cycle, so a longer inter-cycle gap markedly cuts RF power and lets
  // core 1 idle more — at the cost of higher tag-detect (wake) latency.
  void set_low_power(bool v) { low_power_ = v; }
  // Enable/disable tag scanning entirely. When false the poll task leaves the
  // RF field off and idles — used for "NFC in sleep: Off" (max power saving, no
  // local-reader wake). Re-enabled on wake.
  void set_scan_enabled(bool v) { scan_enabled_ = v; }

 protected:
  // ---- Background polling task ----
  // The PN532 handshake busy-waits up to ~500 ms per poll when no tag is
  // present.  Running it on its own task keeps that latency off the main loop
  // so LVGL / touch stay responsive.
  static void poll_task_trampoline(void *arg);
  void poll_task_loop();
  void poll_once();  // one detect/handle cycle

  TaskHandle_t poll_task_handle_{nullptr};

  // ---- Transport (implemented per host interface) ----
  // Bus-specific setup, called first thing in setup().
  virtual void transport_setup() = 0;
  // Bus-specific lines for dump_config() (address, CS pin, ...).
  virtual void dump_transport_config() = 0;
  // Wake the PN532 from power-down / an unknown state (init + SAM retries).
  virtual void transport_wakeup() = 0;
  // Read the PN532 status byte; status == PN532_READY means data is waiting.
  virtual bool transport_read_status(uint8_t &status) = 0;
  // Read len raw bytes of pending data (the ACK frame, a stale-state flush).
  virtual bool transport_read_raw(uint8_t *data, size_t len) = 0;
  // Send a complete, already-built PN532 frame.
  virtual bool transport_write_frame(const std::vector<uint8_t> &frame) = 0;
  // Read one response frame, called once the PN532 is ready. Fills resp with
  // the payload after TFI, so resp[0] is the command code + 1.
  virtual bool transport_read_response(std::vector<uint8_t> &resp) = 0;
  // Gap between two status-byte polls when no IRQ pin is wired. This sits
  // inside every command/response cycle, so on the long Bambu read (about ten
  // cycles, two waits each) it adds up to the time the spool must stay still.
  virtual uint32_t status_poll_delay_ms() const = 0;

  // ---- PN532 low-level ----
  // Abort the command the PN532 is still executing (send an ACK frame) —
  // e.g. InListPassiveTarget, which keeps searching indefinitely when no tag
  // is in the field. Leaving it running lets its late response be mistaken
  // for the ACK/response of the next command.
  void pn532_abort();
  // Discard any response frame the PN532 is holding (resync after a
  // mismatched ACK or response).
  void pn532_flush();
  // Count an exchange that got no valid ACK/response; see
  // PN532_LINK_FAILURE_LIMIT.
  void note_link_failure();
  // Re-initialise a PN532 that stopped responding. Returns true on success.
  bool pn532_recover();
  bool pn532_wait_ready(uint32_t timeout_ms = 100);
  bool pn532_write_command(const std::vector<uint8_t> &cmd);
  bool pn532_read_response(std::vector<uint8_t> &resp, uint32_t timeout_ms = 100);
  bool pn532_send_receive(const std::vector<uint8_t> &cmd,
                          std::vector<uint8_t> &resp,
                          uint32_t timeout_ms = 200);

  // ---- PN532 high-level ----
  bool pn532_init();
  // Detect a single ISO 14443A tag; fills uid + sak on success
  bool pn532_detect_tag(std::vector<uint8_t> &uid, uint8_t &sak);
  // Switch the RF field off. The PN532 leaves it on after every command
  // until told otherwise; InListPassiveTarget switches it back on by itself.
  // The field is the reader's main current draw (~100 mA+), so it must not
  // stay energised between polls or while scanning is disabled.
  void pn532_rf_off();
  bool rf_off_{false};  // poll-task only: field known to be off

  // ---- MIFARE Classic ----
  bool mfc_authenticate(uint8_t target_num, uint8_t block,
                         const uint8_t *key6, const uint8_t *uid4);
  bool mfc_read_block(uint8_t target_num, uint8_t block,
                      uint8_t data_out[16]);
  // Read Bambu blocks 1,2,4,5 using HKDF-derived keys
  // Re-selects the tag and re-runs read_bambu_blocks() on failure, up to
  // BAMBU_READ_ATTEMPTS times. Gives up early if the tag left the reader or a
  // different tag took its place.
  bool read_bambu_blocks_retry(
      const std::vector<uint8_t> &uid,
      std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks_out);

  bool read_bambu_blocks(uint8_t target_num,
                          const std::vector<uint8_t> &uid,
                          std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks_out);

  // ---- NTAG ----
  bool ntag_write_page(uint8_t target_num, uint8_t page,
                       const uint8_t data[4]);
  // Read 4 pages (16 bytes) from an activated NTAG starting at start_page.
  // Uses the same 0x30 READ command as Mifare Classic block reads.
  bool ntag_read_pages(uint8_t target_num, uint8_t start_page, uint8_t out[16]);
  // Read and parse the first NDEF record in the NTAG data area (pages 4+).
  // Returns "open_tag_3d" if the record type contains "opentag",
  // "ndef" for any other valid NDEF, or "" if the tag is unreadable/blank.
  std::string ntag_detect_ndef_format(uint8_t target_num);
  // If the API has a pending NDEF write and the activated tag is an NTAG,
  // perform it.  Returns true if a write was attempted (success or failure),
  // false if there was nothing to do.  Fully logged for diagnosis.
  bool attempt_pending_write(const std::vector<uint8_t> &uid, uint8_t sak);

  // ---- HKDF-SHA256 ----
  // Derive 96 bytes of key material from UID using Bambu master key
  void hkdf_derive_keys(const uint8_t *uid, size_t uid_len,
                         uint8_t okm[96]);
  // Compute HMAC-SHA256
  void hmac_sha256(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t out[32]);

  // ---- UUID extraction ----
  // Decodes material, colour and temperature data from the Bambu blocks read
  // by read_bambu_blocks().  Purely additive: the tray-UUID extraction above
  // is left untouched so spool matching behaves exactly as before.
  static bambuddy_api::BambuTagInfo parse_bambu_tag(
      const std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks);

  static std::string extract_tray_uuid(
      const std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks);

  // ---- State ----
  GPIOPin *irq_pin_{nullptr};
  bambuddy_api::BambuddyAPIComponent *api_{nullptr};
  NFCState state_{NFCState::IDLE};
  std::vector<uint8_t> current_uid_;
  uint8_t current_sak_{0};
  uint8_t miss_count_{0};
  uint8_t miss_threshold_{3};
  uint32_t poll_interval_ms_{300};
  std::atomic<bool> low_power_{false};  // true while the console sleeps → slow polling
  std::atomic<bool> scan_enabled_{true};  // false → skip detection entirely (RF off)
  // true once the PN532 answered pn532_init(). Written by setup() and later
  // by the poll task (init retries), read by dump_config() on the main task.
  std::atomic<bool> nfc_ok_{false};
  // Firmware version reported by the PN532 at init (0 = never answered).
  uint8_t fw_ic_{0}, fw_ver_{0}, fw_rev_{0};
  // Poll-task only (see note_link_failure / pn532_recover).
  uint8_t link_failures_{0};
  uint32_t last_recover_ms_{0};
  // Reader not found yet: pn532_init() is retried every
  // PN532_INIT_RETRY_MS by the poll task (see poll_task_loop).
  uint32_t init_attempts_{0};
  uint32_t last_init_ms_{0};

  // Reader health counters, logged at DEBUG once a minute (only when something
  // happened) and then reset — so a debug log shows *where* scans go wrong
  // (bus, PN532 handshake, RF/tag).
  struct Stats {
    uint32_t cycles{0};         // detect polls
    uint32_t tags{0};           // new tags reported
    uint32_t bambu_ok{0};       // Bambu reads that succeeded
    uint32_t bambu_retry{0};    // ...of which needed more than one attempt
    uint32_t bambu_fail{0};     // Bambu reads that gave up
    uint32_t no_ack{0};         // command not acknowledged (bus / reader down)
    uint32_t bad_ack{0};        // wrong frame where the ACK belongs (desync)
    uint32_t bad_resp{0};       // response for a different command (desync)
    uint32_t resp_timeout{0};   // no response to a tag command (RF / tag left)
    uint32_t recoveries{0};     // PN532 re-initialisations
  } stats_;
  uint32_t last_stats_ms_{0};
  void log_stats_if_due();
};

}  // namespace bambuddy_nfc
}  // namespace esphome
