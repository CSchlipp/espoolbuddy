#include "bambuddy_nfc.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

// mbedTLS HMAC-SHA256 (available in ESP-IDF)
#include "mbedtls/md.h"

#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace esphome {
namespace bambuddy_nfc {

// Render a tag UID as an uppercase hex string (e.g. "04A3B2C1").
static std::string uid_to_hex(const std::vector<uint8_t> &uid) {
  std::ostringstream out;
  for (uint8_t b : uid)
    out << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << (int) b;
  return out.str();
}

// ============================================================================
// SHA-256 / HMAC-SHA256 wrappers (using mbedTLS)
// ============================================================================

void BambuddyNFCComponent::hmac_sha256(const uint8_t *key, size_t key_len,
                                        const uint8_t *data, size_t data_len,
                                        uint8_t out[32]) {
  const mbedtls_md_info_t *md_info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md_info, key, key_len, data, data_len, out);
}

/**
 * HKDF-SHA256 key derivation (RFC 5869).
 *
 * Matches the SpoolBuddy Python daemon's HKDF derivation exactly:
 *   salt    = BAMBU_MASTER_KEY (16 bytes)
 *   IKM     = uid (4 bytes)
 *   context = "RFID-A\0" (7 bytes)
 *   L       = 96 bytes  (16 sectors × 6 bytes each)
 */
void BambuddyNFCComponent::hkdf_derive_keys(const uint8_t *uid, size_t uid_len,
                                             uint8_t okm[96]) {
  // Extract: PRK = HMAC-SHA256(salt=MASTER_KEY, IKM=uid)
  uint8_t prk[32];
  hmac_sha256(BAMBU_MASTER_KEY, sizeof(BAMBU_MASTER_KEY), uid, uid_len, prk);

  // Expand: T(i) = HMAC-SHA256(PRK, T(i-1) || context || counter)
  uint8_t t[32] = {};
  size_t t_len = 0;
  size_t offset = 0;
  uint8_t counter = 1;

  while (offset < 96) {
    // Build HMAC input: T(i-1) || context || counter
    std::vector<uint8_t> hmac_input;
    hmac_input.insert(hmac_input.end(), t, t + t_len);
    hmac_input.insert(hmac_input.end(), BAMBU_CONTEXT,
                      BAMBU_CONTEXT + sizeof(BAMBU_CONTEXT));
    hmac_input.push_back(counter);

    hmac_sha256(prk, 32, hmac_input.data(), hmac_input.size(), t);
    t_len = 32;

    size_t copy = std::min((size_t)32, (size_t)(96 - offset));
    memcpy(okm + offset, t, copy);
    offset += copy;
    counter++;
  }
}

// ============================================================================
// PN532 SPI low-level helpers
// ============================================================================

bool BambuddyNFCComponent::pn532_spi_read_status(uint8_t &status) {
  this->enable();
  delay(2);  // CS setup time: PN532 needs 2ms after /SS assertion before first clock
  this->write_byte(PN532_SPI_STATREAD);
  status = this->read_byte();
  this->disable();
  return true;
}

bool BambuddyNFCComponent::pn532_spi_read_data(uint8_t *data, size_t len) {
  this->enable();
  delay(2);
  this->write_byte(PN532_SPI_DATAREAD);
  this->read_array(data, len);
  this->disable();
  return true;
}

bool BambuddyNFCComponent::pn532_wait_ready(uint32_t timeout_ms) {
  uint32_t deadline = millis() + timeout_ms;
  if (irq_pin_ != nullptr) {
    // IRQ mode: PN532 pulls the line LOW (active-low open-drain) when it has
    // data ready.  Watching the GPIO avoids all SPI traffic while waiting.
    while (millis() < deadline) {
      if (!irq_pin_->digital_read()) return true;
      vTaskDelay(pdMS_TO_TICKS(1));  // 1ms resolution, yields to other tasks
    }
    return false;
  }
  // Fallback (no IRQ pin): poll the SPI status register
  while (millis() < deadline) {
    uint8_t status = 0;
    if (pn532_spi_read_status(status) && status == PN532_READY) return true;
    delay(5);
  }
  return false;
}

bool BambuddyNFCComponent::pn532_write_command(
    const std::vector<uint8_t> &cmd) {
  // Frame: PREAMBLE START1 START2 LEN LCS TFI cmd... DCS POSTAMBLE
  uint8_t len = (uint8_t)(cmd.size() + 1);  // +1 for TFI
  uint8_t lcs = (uint8_t)(~len + 1);

  uint8_t dcs = PN532_TFI_HOST;
  for (uint8_t b : cmd) dcs += b;
  dcs = (uint8_t)(~dcs + 1);

  std::vector<uint8_t> frame;
  frame.push_back(PN532_PREAMBLE);
  frame.push_back(PN532_STARTCODE1);
  frame.push_back(PN532_STARTCODE2);
  frame.push_back(len);
  frame.push_back(lcs);
  frame.push_back(PN532_TFI_HOST);
  frame.insert(frame.end(), cmd.begin(), cmd.end());
  frame.push_back(dcs);
  frame.push_back(PN532_POSTAMBLE);

  this->enable();
  delay(2);  // CS setup time before first clock edge
  this->write_byte(PN532_SPI_DATAWRITE);
  this->write_array(frame.data(), frame.size());
  this->disable();
  return true;
}

bool BambuddyNFCComponent::pn532_read_response(std::vector<uint8_t> &resp,
                                                uint32_t timeout_ms) {
  if (!pn532_wait_ready(timeout_ms)) return false;

  // The PN532 SPI state machine resets on every /SS de-assertion, so the
  // entire response frame — header and body — must be read within a SINGLE
  // /SS assertion.  Splitting into two pn532_spi_read_data() calls (each
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

bool BambuddyNFCComponent::pn532_send_receive(const std::vector<uint8_t> &cmd,
                                               std::vector<uint8_t> &resp,
                                               uint32_t timeout_ms) {
  if (!pn532_write_command(cmd)) return false;
  // Wait for the PN532 to assert the ready flag before reading the ACK.
  // Use 100ms here — 50ms was marginal during cold-boot when the PN532
  // oscillator is still stabilising and the first command takes longer.
  if (!pn532_wait_ready(100)) return false;
  uint8_t ack[6];
  if (!pn532_spi_read_data(ack, sizeof(ack))) return false;
  return pn532_read_response(resp, timeout_ms);
}

// ============================================================================
// PN532 high-level
// ============================================================================

bool BambuddyNFCComponent::pn532_init() {
  // PN532 UM10232 §7.2.11: assert /SS (CS) low for at least 10ms to wake the
  // PN532 from H_0 (power-down) or any unknown state after power-on / ESP32 reset.
  // No SPI clock activity should occur during this window.
  auto do_wakeup = [this]() {
    this->cs_->digital_write(false);
    delay(15);  // spec minimum is 10ms; 15ms gives a comfortable margin
    this->cs_->digital_write(true);
  };
  do_wakeup();
  delay(100);  // allow PN532 oscillator startup and internal reset to complete

  // After an ESP32 reset mid-transaction the PN532 may still assert "ready".
  // Flush any such stale state so it is not mistaken for a command ACK.
  {
    uint8_t status = 0;
    pn532_spi_read_status(status);
    if (status == PN532_READY) {
      ESP_LOGD(NFC_TAG, "PN532 had stale ready flag at init — flushing");
      uint8_t flush[32];
      pn532_spi_read_data(flush, sizeof(flush));
    }
  }

  // SAMConfiguration: Normal mode, 500 ms RF timeout, IRQ enabled if wired.
  // Byte 4 (UseIRQ): 0x01 = PN532 asserts IRQ LOW when data ready,
  //                  0x00 = polled via SPI status register (no IRQ pin).
  // Retry up to 3 times — the first attempt can fail if the PN532 is still
  // completing its internal initialisation after power-on or wakeup.
  const uint8_t use_irq = (irq_pin_ != nullptr) ? 0x01 : 0x00;
  ESP_LOGI(NFC_TAG, "PN532 ready-detection: %s",
           use_irq ? "IRQ pin (GPIO)" : "SPI status register (polling)");
  std::vector<uint8_t> resp;
  bool sam_ok = false;
  for (int attempt = 0; attempt < 3 && !sam_ok; attempt++) {
    if (attempt > 0) {
      // Re-issue the wakeup sequence: if the PN532 lost sync after the failed
      // attempt, a fresh CS+byte pulse lets it re-enter the command-receive state.
      ESP_LOGW(NFC_TAG, "SAMConfiguration attempt %d/3", attempt + 1);
      do_wakeup();
      delay(50);
    }
    std::vector<uint8_t> cmd = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x0A, use_irq};
    sam_ok = pn532_send_receive(cmd, resp, 200);
  }
  if (!sam_ok) return false;

  // Verify firmware version response
  std::vector<uint8_t> cmd = {PN532_CMD_GETFIRMWAREVERSION};
  if (!pn532_send_receive(cmd, resp, 200)) return false;
  if (resp.size() < 4) return false;

  // resp[0]=CMD+1(0x03), resp[1]=IC, resp[2]=Ver, resp[3]=Rev, resp[4]=Support
  ESP_LOGI(NFC_TAG, "PN532 firmware: IC=0x%02X Ver=%d.%d Rev=%d",
           resp[1], resp[2], resp[3], (resp.size() > 4 ? resp[4] : 0));
  return true;
}

bool BambuddyNFCComponent::pn532_detect_tag(std::vector<uint8_t> &uid,
                                             uint8_t &sak) {
  // InListPassiveTarget: max 1 target, 106 kbps ISO14443A
  std::vector<uint8_t> cmd = {PN532_CMD_INLISTPASSIVETARGET, 0x01, 0x00};
  std::vector<uint8_t> resp;
  if (!pn532_send_receive(cmd, resp, 500)) return false;

  // Response: CMD+1, NumTg, Tg, ATQA(2), SAK(1), NfcIdLen(1), NfcId...
  if (resp.size() < 7) return false;
  if (resp[0] != (PN532_CMD_INLISTPASSIVETARGET + 1)) return false;
  if (resp[1] == 0) return false;  // no target found

  // resp[2] = target number (usually 1)
  // resp[3..4] = ATQA
  sak = resp[5];
  uint8_t uid_len = resp[6];
  if (resp.size() < (size_t)(7 + uid_len)) return false;

  uid.assign(resp.begin() + 7, resp.begin() + 7 + uid_len);
  return true;
}

// ============================================================================
// MIFARE Classic
// ============================================================================

bool BambuddyNFCComponent::mfc_authenticate(uint8_t target_num, uint8_t block,
                                              const uint8_t *key6,
                                              const uint8_t *uid4) {
  // InDataExchange: MFC_AUTH_KEY_A + block + key6 + uid4
  std::vector<uint8_t> cmd = {PN532_CMD_INDATAEXCHANGE, target_num,
                               MFC_AUTH_KEY_A, block};
  for (int i = 0; i < 6; i++) cmd.push_back(key6[i]);
  for (int i = 0; i < 4; i++) cmd.push_back(uid4[i]);

  std::vector<uint8_t> resp;
  if (!pn532_send_receive(cmd, resp, 300)) return false;
  if (resp.size() < 2) return false;
  // resp[0] = CMD+1, resp[1] = error code (0x00 = success)
  return resp[1] == 0x00;
}

bool BambuddyNFCComponent::mfc_read_block(uint8_t target_num, uint8_t block,
                                           uint8_t data_out[16]) {
  std::vector<uint8_t> cmd = {PN532_CMD_INDATAEXCHANGE, target_num, MFC_READ,
                               block};
  std::vector<uint8_t> resp;
  if (!pn532_send_receive(cmd, resp, 300)) return false;
  if (resp.size() < 18) return false;  // CMD+1 + errcode + 16 bytes
  if (resp[1] != 0x00) return false;
  memcpy(data_out, &resp[2], 16);
  return true;
}

bool BambuddyNFCComponent::read_bambu_blocks(
    uint8_t target_num, const std::vector<uint8_t> &uid,
    std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks_out) {
  if (uid.size() < 4) return false;

  // Derive HKDF keys
  uint8_t okm[96];
  hkdf_derive_keys(uid.data(), uid.size(), okm);

  int current_sector = -1;

  for (uint8_t block : BAMBU_BLOCKS) {
    int sector = block / 4;

    if (sector != current_sector) {
      // Sector key: okm[sector*6 .. sector*6+5]
      const uint8_t *key = okm + sector * 6;
      if (!mfc_authenticate(target_num, block, key, uid.data())) {
        if (!bambu_block_is_required(block)) {
          ESP_LOGW(NFC_TAG,
                   "Bambu auth failed for optional block %d (sector %d) - "
                   "skipping it, the scan stands",
                   block, sector);
          continue;
        }
        ESP_LOGW(NFC_TAG, "Bambu auth failed for block %d sector %d", block,
                 sector);
        return false;
      }
      current_sector = sector;
    }

    uint8_t data[16];
    if (!mfc_read_block(target_num, block, data)) {
      if (!bambu_block_is_required(block)) {
        ESP_LOGW(NFC_TAG,
                 "Bambu read failed for optional block %d - skipping it, the "
                 "scan stands",
                 block);
        continue;
      }
      ESP_LOGW(NFC_TAG, "Bambu read failed for block %d", block);
      return false;
    }

    std::array<uint8_t, 16> arr;
    memcpy(arr.data(), data, 16);
    blocks_out.push_back({block, arr});
  }
  return true;
}

// ============================================================================
// NTAG read / write
// ============================================================================

bool BambuddyNFCComponent::ntag_write_page(uint8_t target_num, uint8_t page,
                                            const uint8_t data[4]) {
  std::vector<uint8_t> cmd = {PN532_CMD_INDATAEXCHANGE, target_num, NTAG_WRITE,
                               page};
  for (int i = 0; i < 4; i++) cmd.push_back(data[i]);

  std::vector<uint8_t> resp;
  if (!pn532_send_receive(cmd, resp, 300)) return false;
  if (resp.size() < 2) return false;
  return resp[1] == 0x00;
}

// ============================================================================
// UUID extraction (matches the Python daemon's tray-UUID extraction)
// ============================================================================

bool BambuddyNFCComponent::read_bambu_blocks_retry(
    const std::vector<uint8_t> &uid,
    std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks_out) {
  for (uint8_t attempt = 1; attempt <= BAMBU_READ_ATTEMPTS; attempt++) {
    blocks_out.clear();
    if (read_bambu_blocks(1, uid, blocks_out)) {
      if (attempt > 1)
        ESP_LOGI(NFC_TAG, "Bambu read succeeded on attempt %u/%u", attempt,
                 BAMBU_READ_ATTEMPTS);
      return true;
    }
    if (attempt == BAMBU_READ_ATTEMPTS) break;

    // A failed authentication or read leaves the MIFARE session unusable: the
    // tag must be re-selected with InListPassiveTarget before the next try.
    delay(BAMBU_RETRY_DELAY_MS);
    std::vector<uint8_t> again_uid;
    uint8_t again_sak = 0;
    if (!pn532_detect_tag(again_uid, again_sak)) {
      ESP_LOGD(NFC_TAG, "Bambu read attempt %u: tag left the reader", attempt);
      return false;
    }
    if (again_uid != uid) {
      ESP_LOGD(NFC_TAG, "Bambu read attempt %u: a different tag is present",
               attempt);
      return false;
    }
  }
  ESP_LOGW(NFC_TAG, "Bambu read failed after %u attempts", BAMBU_READ_ATTEMPTS);
  return false;
}

// ============================================================================
// Bambu tag payload decoding
//
// Ported from bemble/esphome-bambuddy-reader (MIT), which decodes the same
// block layout.  Kept strictly additive: nothing here influences the tray-UUID
// used for spool matching.
// ============================================================================

namespace {

// Bambu stores short strings NUL- or space-padded inside a 16-byte block.
std::string bambu_trim(const uint8_t *b) {
  std::string out(reinterpret_cast<const char *>(b), 16);
  size_t end = out.find('\0');
  if (end != std::string::npos) out.erase(end);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  return out;
}

uint16_t bambu_u16(const uint8_t *b, size_t off) {
  return static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8);
}

float bambu_f32(const uint8_t *b, size_t off) {
  float v;
  memcpy(&v, b + off, 4);
  return v;
}

const uint8_t *find_block(
    const std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks,
    uint8_t n) {
  for (const auto &b : blocks)
    if (b.first == n) return b.second.data();
  return nullptr;
}

}  // namespace

bambuddy_api::BambuTagInfo BambuddyNFCComponent::parse_bambu_tag(
    const std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks) {
  bambuddy_api::BambuTagInfo info;

  const uint8_t *b1 = find_block(blocks, 1);
  const uint8_t *b2 = find_block(blocks, 2);
  const uint8_t *b4 = find_block(blocks, 4);
  const uint8_t *b5 = find_block(blocks, 5);
  const uint8_t *b6 = find_block(blocks, 6);

  // Block 2 carries the base material and is what makes the payload usable at
  // all — without it we return an invalid struct and the caller falls back.
  if (b2 == nullptr) return info;
  info.material = bambu_trim(b2);
  if (info.material.empty()) return info;

  // Block 1: variant id (8B) + material id (8B), both NUL-padded ASCII.
  if (b1 != nullptr) {
    std::string variant(reinterpret_cast<const char *>(b1), 8);
    std::string material_id(reinterpret_cast<const char *>(b1) + 8, 8);
    for (std::string *v : {&variant, &material_id}) {
      size_t end = v->find('\0');
      if (end != std::string::npos) v->erase(end);
      while (!v->empty() && v->back() == ' ') v->pop_back();
    }
    info.variant_id  = variant;
    info.material_id = material_id;
  }

  // Block 4: detailed type, e.g. "PLA Matte" -> subtype "Matte".
  if (b4 != nullptr) {
    info.detailed_type = bambu_trim(b4);
    if (info.detailed_type.size() > info.material.size() + 1 &&
        info.detailed_type.compare(0, info.material.size(), info.material) == 0 &&
        info.detailed_type[info.material.size()] == ' ') {
      info.subtype = info.detailed_type.substr(info.material.size() + 1);
    }
  }
  if (info.detailed_type.empty()) info.detailed_type = info.material;

  // Block 5: colour RGBA (4B) + spool weight (2B) + pad (2B) + diameter (4B).
  if (b5 != nullptr) {
    char hex[7];
    snprintf(hex, sizeof(hex), "%02X%02X%02X", b5[0], b5[1], b5[2]);
    info.color_hex = hex;

    const char *name = find_bambu_color_name(info.detailed_type.c_str(), hex);
    if (name != nullptr) {
      std::string cn(name);
      // The table stores names like "Matte Charcoal"; strip the subtype word so
      // the inventory entry reads "Charcoal" next to its "Matte" subtype.
      if (!info.subtype.empty()) {
        if (cn.size() > info.subtype.size() + 1 &&
            cn.compare(0, info.subtype.size() + 1, info.subtype + " ") == 0) {
          cn = cn.substr(info.subtype.size() + 1);
        } else if (cn.size() > info.subtype.size() + 1 &&
                   cn.compare(cn.size() - info.subtype.size() - 1,
                              info.subtype.size() + 1, " " + info.subtype) == 0) {
          cn = cn.substr(0, cn.size() - info.subtype.size() - 1);
        }
      }
      info.color_name = cn;
    } else {
      info.color_name = info.color_hex;
    }

    info.spool_weight = bambu_u16(b5, 4);
    info.diameter     = bambu_f32(b5, 8);
  }

  // Block 6: drying temp (2B) + drying time (2B) + pad (2B) + bed temp (2B)
  //          + hotend max (2B) + hotend min (2B).
  if (b6 != nullptr) {
    info.drying_temp     = bambu_u16(b6, 0);
    info.drying_time     = bambu_u16(b6, 2);
    info.bed_temp        = bambu_u16(b6, 6);
    info.nozzle_temp_max = bambu_u16(b6, 8);
    info.nozzle_temp_min = bambu_u16(b6, 10);
  }

  info.valid = true;
  return info;
}

std::string BambuddyNFCComponent::extract_tray_uuid(
    const std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> &blocks) {
  // Block 9 holds the Bambu tray UID: the identity of the *physical spool*, as
  // opposed to the tag's own card UID. Both tags of a spool carry the same
  // value and different spools carry different ones, which is exactly what
  // inventory matching needs. Prefer it whenever the block was read.
  //
  // The heuristic below is kept as a fallback for tags whose sector 2 could not
  // be read. Note that it does not yield a spool-unique value on Bambu tags:
  // it falls through to the hex of block 4, which holds the filament type, so
  // every spool of the same type produces an identical "UUID". Anything that
  // relies on tray_uuid to tell two spools apart breaks in that case.
  for (const auto &b : blocks) {
    if (b.first != 9) continue;
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02X", b.second[i]);
    hex[32] = '\0';
    std::string uuid(hex);
    if (uuid.find_first_not_of('0') != std::string::npos) {
      ESP_LOGD(NFC_TAG, "Tray UID from block 9: %s", uuid.c_str());
      return uuid;
    }
    ESP_LOGW(NFC_TAG, "Block 9 is all zeros - falling back to the heuristic");
    break;
  }

  const uint8_t *blk4 = nullptr;
  const uint8_t *blk5 = nullptr;
  for (const auto &b : blocks) {
    if (b.first == 4) blk4 = b.second.data();
    if (b.first == 5) blk5 = b.second.data();
  }
  if (!blk4 || !blk5) return "";

  // Combine blocks 4+5 (32 bytes)
  uint8_t raw[32];
  memcpy(raw, blk4, 16);
  memcpy(raw + 16, blk5, 16);

  // Preferred: decode as ASCII and keep hex chars
  std::string hex_chars;
  for (int i = 0; i < 32; i++) {
    char c = (char)raw[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F')) {
      hex_chars += c;
    }
  }
  if (hex_chars.size() >= 32) {
    std::string uuid = hex_chars.substr(0, 32);
    // Convert to uppercase
    for (char &ch : uuid) {
      if (ch >= 'a' && ch <= 'f') ch -= 32;
    }
    // Check not all zeros
    bool all_zero = true;
    for (char ch : uuid) {
      if (ch != '0') { all_zero = false; break; }
    }
    if (!all_zero) return uuid;
  }

  // Fallback: first 16 raw bytes as hex
  char buf[33];
  for (int i = 0; i < 16; i++) {
    snprintf(buf + i * 2, 3, "%02X", raw[i]);
  }
  buf[32] = '\0';
  return std::string(buf);
}

// ============================================================================
// Component lifecycle
// ============================================================================

void BambuddyNFCComponent::setup() {
  this->spi_setup();
  if (irq_pin_ != nullptr) {
    irq_pin_->setup();  // configure as input (pull-up set by ESPHome pin schema)
    ESP_LOGI(NFC_TAG, "BambuddyNFC setup (PN532 via SPI, IRQ-driven)");
  } else {
    ESP_LOGI(NFC_TAG, "BambuddyNFC setup (PN532 via SPI, polling)");
  }
  // Feed WDT then wait for PN532 power-on settle (200 ms covers the PN532's
  // maximum reset/oscillator startup time per the user manual).
  arch_feed_wdt();
  delay(200);

  if (!pn532_init()) {
    ESP_LOGE(NFC_TAG, "PN532 init failed — NFC will be unavailable");
    nfc_ok_ = false;
    if (api_) api_->set_nfc_ok(false);
    return;
  }

  nfc_ok_ = true;
  if (api_) api_->set_nfc_ok(true);
  ESP_LOGI(NFC_TAG, "PN532 initialized");

  // Spawn the polling task on core 1 (the main loop / LVGL run on core 0), so
  // the PN532's busy-wait handshakes run in parallel and never stall the UI.
  // 8 kB stack: SPI paths + mbedTLS HKDF/SHA-256 key derivation (Bambu MIFARE
  // reads) + ESP_LOG formatting + api_ callback std::string building.  6 kB
  // left too little margin: a wild-PC interrupt-WDT crash on core 1 pointed at
  // stack corruption.  The poll loop logs its high-water mark periodically so
  // the remaining headroom stays visible.
  xTaskCreatePinnedToCore(&BambuddyNFCComponent::poll_task_trampoline,
                          "bambuddy_nfc", 8192, this,
                          4 /* priority */, &poll_task_handle_, 1 /* core */);
  ESP_LOGI(NFC_TAG, "NFC polling task started on core 1");
}

// loop() is intentionally empty — polling runs on the dedicated task so the
// PN532's busy-wait handshakes never block the main loop / LVGL.
void BambuddyNFCComponent::loop() {}

void BambuddyNFCComponent::poll_task_trampoline(void *arg) {
  static_cast<BambuddyNFCComponent *>(arg)->poll_task_loop();
}

void BambuddyNFCComponent::poll_task_loop() {
  uint32_t last_stack_diag_ms = 0;
  for (;;) {
    // Scanning disabled (console asleep with "NFC in sleep: Off"): leave the RF
    // field off entirely and idle until scanning is re-enabled on wake.
    if (!scan_enabled_) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    poll_once();
    // Stack-headroom diagnostic (every 5 min): high-water mark is the minimum
    // free stack ever seen, in StackType_t words.  If this trends toward zero
    // the task is the prime suspect for wild-PC / int-WDT crashes on core 1.
    uint32_t now_ms = millis();
    if (now_ms - last_stack_diag_ms >= 300000UL) {
      last_stack_diag_ms = now_ms;
      ESP_LOGI(NFC_TAG, "NFC task stack high-water: %u bytes free",
               (unsigned) (uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
    // In IRQ mode the PN532 holds IRQ LOW until we read the response, then
    // de-asserts it.  A 10ms guard prevents hammering the bus if the PN532
    // re-asserts IRQ immediately (e.g. tag still present).
    // In polling mode honour poll_interval_ms_ to give the SPI bus breathing room.
    uint32_t gap_ms = (irq_pin_ != nullptr) ? 10 : poll_interval_ms_;
    // While the console sleeps, widen the gap to ~750 ms so the RF field is
    // energized far less often (big power saving), at the cost of slower
    // tag-detect/wake latency. Reset to the normal gap on wake.
    if (low_power_) gap_ms = 750;
    vTaskDelay(pdMS_TO_TICKS(gap_ms));
  }
}

bool BambuddyNFCComponent::ntag_read_pages(uint8_t target_num,
                                            uint8_t start_page,
                                            uint8_t out[16]) {
  // NTAG READ (0x30) and Mifare READ share the same command byte.
  // The PN532 returns 4 pages (16 bytes) starting at start_page.
  std::vector<uint8_t> cmd = {PN532_CMD_INDATAEXCHANGE, target_num,
                               MFC_READ, start_page};
  std::vector<uint8_t> resp;
  if (!pn532_send_receive(cmd, resp, 300)) return false;
  if (resp.size() < 17 || resp[0] != 0x00) return false;
  memcpy(out, resp.data() + 1, 16);
  return true;
}

std::string BambuddyNFCComponent::ntag_detect_ndef_format(uint8_t target_num) {
  // Read the first 16 bytes of the NTAG NDEF data area (pages 4–7).
  uint8_t pages[16];
  if (!ntag_read_pages(target_num, 4, pages)) {
    ESP_LOGD(NFC_TAG, "NDEF detect: page read failed");
    return "";
  }

  // Walk TLV blocks to find the NDEF Message TLV (0x03).
  size_t pos = 0;
  while (pos < 16) {
    uint8_t tlv_t = pages[pos++];
    if (tlv_t == 0x03) break;     // NDEF Message TLV — found
    if (tlv_t == 0xFE) return ""; // Terminator — no NDEF content
    if (tlv_t == 0x00) continue;  // NULL TLV (padding byte)
    // Any other TLV: skip length + data
    if (pos >= 16) return "";
    uint8_t tlv_l = pages[pos++];
    if (tlv_l == 0xFF) pos += 2;  // 3-byte length encoding
    pos += tlv_l;
  }
  if (pos >= 16) return "";  // never found 0x03

  // Skip the NDEF message length byte(s).
  uint8_t msg_len = pages[pos++];
  if (msg_len == 0xFF) pos += 2;  // 3-byte length
  if (pos >= 16) return "ndef";

  // Parse the first NDEF record header byte.
  uint8_t hdr      = pages[pos++];
  uint8_t tnf      = hdr & 0x07;  // Type Name Format
  bool    sr       = (hdr & 0x10) != 0;  // Short Record
  bool    il       = (hdr & 0x08) != 0;  // ID Length present
  if (pos >= 16) return "ndef";

  uint8_t type_len = pages[pos++];
  if (pos >= 16) return "ndef";

  // Skip payload length: 1 byte (SR=1) or 4 bytes.
  pos += sr ? 1 : 4;
  // Skip optional ID length field.
  if (il && pos < 16) pos++;
  if (pos + type_len > 16 || type_len == 0) return "ndef";

  // Extract record type bytes.
  std::string rtype(reinterpret_cast<const char *>(pages + pos), type_len);
  ESP_LOGD(NFC_TAG, "NDEF detect: TNF=0x%02X type='%.*s'", tnf,
           (int)type_len, pages + pos);

  // NFC Forum External Type (TNF=0x04) with "opentag" in the domain name
  // → OpenTag3D format (e.g. "opentag3d.org:f")
  if (tnf == 0x04) {
    std::string lower = rtype;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("opentag") != std::string::npos) return "open_tag_3d";
  }

  return "ndef";
}

bool BambuddyNFCComponent::attempt_pending_write(
    const std::vector<uint8_t> &uid, uint8_t sak) {
  if (!api_ || !api_->has_pending_write()) return false;

  // UID hex string for result reporting.
  std::string uid_str = uid_to_hex(uid);

  // NTAG (NfcForum Type 2) reports SAK 0x00; some readers report 0x04.
  bool is_ntag = (sak == 0x00 || sak == 0x04);
  if (!is_ntag) {
    ESP_LOGW(NFC_TAG,
             "Pending write, but tag SAK=0x%02X is not an NTAG — cannot write",
             sak);
    api_->on_write_tag_result(
        uid_str, false,
        "Incompatible tag type — place a writable NTAG on the reader");
    api_->clear_pending_write();
    return true;
  }

  const std::vector<uint8_t> &ndef_data = api_->pending_write_data();
  if (ndef_data.empty()) {
    ESP_LOGW(NFC_TAG, "Pending write flagged but NDEF payload is empty");
    api_->clear_pending_write();
    return true;
  }

  size_t padded_len = ((ndef_data.size() + 3) / 4) * 4;
  std::vector<uint8_t> padded = ndef_data;
  padded.resize(padded_len, 0x00);
  uint8_t num_pages = (uint8_t) (padded_len / 4);

  ESP_LOGI(NFC_TAG,
           "Writing NDEF to NTAG %s: %zu bytes -> %u pages (4..%u), spool %d",
           uid_str.c_str(), ndef_data.size(), num_pages,
           4 + num_pages - 1, api_->pending_write_spool_id());

  bool write_ok = true;
  for (size_t i = 0; i < padded_len; i += 4) {
    uint8_t page = (uint8_t) (4 + i / 4);
    if (!ntag_write_page(1, page, padded.data() + i)) {
      write_ok = false;
      ESP_LOGW(NFC_TAG, "NTAG write failed at page %u (%u/%u)", page,
               (unsigned) (i / 4 + 1), num_pages);
      break;
    }
    delay(2);
  }

  std::string msg = write_ok ? "Write successful" : "Write failed";
  ESP_LOGI(NFC_TAG, "NTAG write result: %s (%zu bytes)", msg.c_str(),
           ndef_data.size());
  api_->on_write_tag_result(uid_str, write_ok, msg);
  api_->clear_pending_write();
  return true;
}

void BambuddyNFCComponent::poll_once() {
  if (!nfc_ok_) return;

  std::vector<uint8_t> uid;
  uint8_t sak = 0;
  bool detected = pn532_detect_tag(uid, sak);

  // Diagnostic: surface why a queued write may not be firing.
  if (api_ && api_->has_pending_write()) {
    ESP_LOGI(NFC_TAG,
             "Pending write active: detected=%d sak=0x%02X state=%s",
             detected, detected ? sak : current_sak_,
             state_ == NFCState::TAG_PRESENT ? "PRESENT" : "IDLE");
  }

  if (detected) {
    miss_count_ = 0;

    if (state_ == NFCState::IDLE) {
      // New tag detected
      state_ = NFCState::TAG_PRESENT;
      current_uid_ = uid;
      current_sak_ = sak;

      // Determine tag type
      std::string tag_type;
      if (sak == 0x08 || sak == 0x18) {
        tag_type = "mifare_classic";
      } else if (sak == 0x00 || sak == 0x04) {
        tag_type = "ntag";
      } else {
        tag_type = "unknown";
      }

      std::string uid_str = uid_to_hex(uid);

      // Try to read Bambu tag data for MIFARE Classic
      std::string tray_uuid;
      bambuddy_api::BambuTagInfo bambu_info;
      if (sak == 0x08 || sak == 0x18) {
        std::vector<std::pair<uint8_t, std::array<uint8_t, 16>>> blocks;
        if (read_bambu_blocks_retry(uid, blocks)) {
          tray_uuid   = extract_tray_uuid(blocks);
          bambu_info  = parse_bambu_tag(blocks);
          if (bambu_info.valid) {
            ESP_LOGI(NFC_TAG,
                     "Bambu tag decoded: %s / %s / %s (#%s) %u g, "
                     "hotend %u-%u C, bed %u C",
                     bambu_info.material.c_str(),
                     bambu_info.subtype.empty() ? "-" : bambu_info.subtype.c_str(),
                     bambu_info.color_name.c_str(), bambu_info.color_hex.c_str(),
                     bambu_info.spool_weight, bambu_info.nozzle_temp_min,
                     bambu_info.nozzle_temp_max, bambu_info.bed_temp);
          }
        }
      }

      ESP_LOGI(NFC_TAG, "Tag detected: uid=%s sak=0x%02X type=%s tray_uuid=%s",
               uid_str.c_str(), sak, tag_type.c_str(), tray_uuid.c_str());

      if (api_) {
        // The decoded payload rides along with the scan instead of being set
        // afterwards: on a scale device on_tag_scanned() queues the push to the
        // console straight away, so anything set after the call would arrive
        // too late to be forwarded.
        api_->on_tag_scanned(uid_str, tray_uuid, (int)sak, tag_type,
                             bambu_info.valid ? &bambu_info : nullptr);
      }

      // For NTAG tags: read NDEF pages and refine the format beyond SAK alone.
      // This runs after on_tag_scanned() so the UI gets immediate feedback,
      // and before attempt_pending_write() so we classify the current content.
      if ((sak == 0x00 || sak == 0x04) && api_) {
        std::string fmt = ntag_detect_ndef_format(1);
        if (!fmt.empty()) {
          ESP_LOGI(NFC_TAG, "NDEF format detected: %s", fmt.c_str());
          api_->set_tag_format(fmt);
        }
      }

      // A write command may already be queued (e.g. tag re-placed); try it.
      attempt_pending_write(uid, sak);

    } else {
      // Tag still on the reader from a previous cycle.  The write command
      // usually arrives here, a poll or two after the initial scan, via the
      // backend's heartbeat response.
      attempt_pending_write(current_uid_, current_sak_);
    }

  } else {
    // No tag detected
    if (state_ == NFCState::TAG_PRESENT) {
      miss_count_++;
      if (miss_count_ >= miss_threshold_) {
        std::string old_uid = uid_to_hex(current_uid_);

        ESP_LOGI(NFC_TAG, "Tag removed: %s", old_uid.c_str());
        state_ = NFCState::IDLE;
        current_uid_.clear();
        current_sak_ = 0;
        miss_count_ = 0;

        if (api_) {
          api_->on_tag_removed(old_uid);
        }
      }
    }
  }
}

}  // namespace bambuddy_nfc
}  // namespace esphome
