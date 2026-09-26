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
// PN532 low-level (bus-independent; bytes move through the transport hooks)
// ============================================================================

bool BambuddyNFCComponent::pn532_wait_ready(uint32_t timeout_ms) {
  // Elapsed-time comparison: stays correct across the millis() wrap (~49.7
  // days), where an absolute deadline would end the wait immediately.
  const uint32_t start = millis();
  if (irq_pin_ != nullptr) {
    // IRQ mode: PN532 pulls the line LOW (active-low open-drain) when it has
    // data ready.  Watching the GPIO avoids all bus traffic while waiting.
    while (millis() - start < timeout_ms) {
      if (!irq_pin_->digital_read()) return true;
      vTaskDelay(pdMS_TO_TICKS(1));  // 1ms resolution, yields to other tasks
    }
    return false;
  }
  // Fallback (no IRQ pin): poll the status byte over the bus
  while (millis() - start < timeout_ms) {
    uint8_t status = 0;
    if (transport_read_status(status) && status == PN532_READY) return true;
    delay(status_poll_delay_ms());
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

  return transport_write_frame(frame);
}

bool BambuddyNFCComponent::pn532_read_response(std::vector<uint8_t> &resp,
                                                uint32_t timeout_ms) {
  if (!pn532_wait_ready(timeout_ms)) return false;
  return transport_read_response(resp);
}

bool BambuddyNFCComponent::pn532_send_receive(const std::vector<uint8_t> &cmd,
                                               std::vector<uint8_t> &resp,
                                               uint32_t timeout_ms) {
  if (!pn532_write_command(cmd)) {
    note_link_failure();
    return false;
  }
  // Wait for the PN532 to assert the ready flag before reading the ACK.
  // Use 100ms here — 50ms was marginal during cold-boot when the PN532
  // oscillator is still stabilising and the first command takes longer.
  if (!pn532_wait_ready(100)) {
    ESP_LOGD(NFC_TAG, "PN532: no ACK for command 0x%02X", cmd[0]);
    stats_.no_ack++;
    note_link_failure();
    return false;
  }
  // The frame waiting here must be the ACK for *this* command. Anything else
  // (typically a late response to an earlier command) means host and reader
  // are out of step; reading on regardless would pair every following
  // command with the previous one's response.
  uint8_t ack[6] = {};
  if (!transport_read_raw(ack, sizeof(ack)) ||
      memcmp(ack, PN532_ACK_FRAME, sizeof(ack)) != 0) {
    ESP_LOGW(NFC_TAG,
             "PN532: expected ACK for command 0x%02X, got "
             "%02X %02X %02X %02X %02X %02X — resyncing",
             cmd[0], ack[0], ack[1], ack[2], ack[3], ack[4], ack[5]);
    stats_.bad_ack++;
    pn532_flush();
    note_link_failure();
    return false;
  }
  // A valid ACK proves the link — including on the idle path below, where
  // InListPassiveTarget legitimately times out on every no-tag poll.
  link_failures_ = 0;
  if (!pn532_read_response(resp, timeout_ms)) {
    // No response in time: the command is still running — normal for
    // InListPassiveTarget with no tag in the field, which the PN532 keeps
    // retrying indefinitely. Abort it, or its response turns up later in
    // place of the next command's ACK.
    // (A detect poll timing out is the idle case, not worth counting.)
    if (cmd[0] != PN532_CMD_INLISTPASSIVETARGET) stats_.resp_timeout++;
    pn532_abort();
    return false;
  }
  if (resp.empty() || resp[0] != (uint8_t) (cmd[0] + 1)) {
    ESP_LOGW(NFC_TAG, "PN532: response 0x%02X does not match command 0x%02X — resyncing",
             resp.empty() ? 0 : resp[0], cmd[0]);
    stats_.bad_resp++;
    pn532_flush();
    note_link_failure();
    return false;
  }
  return true;
}

void BambuddyNFCComponent::pn532_abort() {
  std::vector<uint8_t> ack(PN532_ACK_FRAME, PN532_ACK_FRAME + sizeof(PN532_ACK_FRAME));
  transport_write_frame(ack);
  delay(10);  // let the PN532 drop the command before the next one arrives
}

void BambuddyNFCComponent::pn532_flush() {
  uint8_t status = 0;
  if (transport_read_status(status) && status == PN532_READY) {
    uint8_t discard[64];
    transport_read_raw(discard, sizeof(discard));
  }
  pn532_abort();
}

void BambuddyNFCComponent::note_link_failure() {
  if (link_failures_ < 255) link_failures_++;
}

bool BambuddyNFCComponent::pn532_recover() {
  ESP_LOGW(NFC_TAG, "PN532 stopped responding (%u failed exchanges) — re-initialising",
           link_failures_);
  last_recover_ms_ = millis();
  stats_.recoveries++;
  if (api_) api_->set_nfc_ok(false);
  link_failures_ = 0;
  if (!pn532_init()) {
    ESP_LOGE(NFC_TAG, "PN532 re-initialisation failed — retrying in %u s",
             (unsigned) (PN532_RECOVER_RETRY_MS / 1000));
    link_failures_ = PN532_LINK_FAILURE_LIMIT;  // keep recovery armed
    return false;
  }
  link_failures_ = 0;
  if (api_) api_->set_nfc_ok(true);
  ESP_LOGI(NFC_TAG, "PN532 re-initialised — NFC scanning resumed");
  return true;
}

// ============================================================================
// PN532 high-level
// ============================================================================

bool BambuddyNFCComponent::pn532_init() {
  // Wake the PN532 from H_0 (power-down) or any unknown state after power-on /
  // ESP32 reset — how that is done depends on the host interface.
  transport_wakeup();
  delay(100);  // allow PN532 oscillator startup and internal reset to complete

  // After an ESP32 reset mid-transaction the PN532 may still assert "ready".
  // Flush any such stale state so it is not mistaken for a command ACK.
  {
    uint8_t status = 0;
    transport_read_status(status);
    if (status == PN532_READY) {
      ESP_LOGD(NFC_TAG, "PN532 had stale ready flag at init — flushing");
      uint8_t flush[32];
      transport_read_raw(flush, sizeof(flush));
    }
  }

  // SAMConfiguration: Normal mode, 500 ms RF timeout, IRQ enabled if wired.
  // Byte 4 (UseIRQ): 0x01 = PN532 asserts IRQ LOW when data ready,
  //                  0x00 = polled via the status byte (no IRQ pin).
  // Retry up to 3 times — the first attempt can fail if the PN532 is still
  // completing its internal initialisation after power-on or wakeup.
  const uint8_t use_irq = (irq_pin_ != nullptr) ? 0x01 : 0x00;
  std::vector<uint8_t> resp;
  bool sam_ok = false;
  for (int attempt = 0; attempt < 3 && !sam_ok; attempt++) {
    if (attempt > 0) {
      // Re-issue the wakeup sequence: if the PN532 lost sync after the failed
      // attempt, a fresh wakeup lets it re-enter the command-receive state.
      // DEBUG only: with no reader attached this runs on every init retry.
      ESP_LOGD(NFC_TAG, "SAMConfiguration attempt %d/3", attempt + 1);
      transport_wakeup();
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
  fw_ic_ = resp[1];
  fw_ver_ = resp[2];
  fw_rev_ = resp[3];
  ESP_LOGI(NFC_TAG, "PN532 firmware: IC=0x%02X Ver=%d.%d Rev=%d",
           resp[1], resp[2], resp[3], (resp.size() > 4 ? resp[4] : 0));
  return true;
}

void BambuddyNFCComponent::pn532_rf_off() {
  // RFConfiguration, CfgItem 0x01 (RF field): bit 0 = RF on, bit 1 = AutoRFCA.
  std::vector<uint8_t> cmd = {PN532_CMD_RFCONFIGURATION, 0x01, 0x00};
  std::vector<uint8_t> resp;
  rf_off_ = pn532_send_receive(cmd, resp, 50);
}

bool BambuddyNFCComponent::pn532_detect_tag(std::vector<uint8_t> &uid,
                                             uint8_t &sak) {
  // InListPassiveTarget: max 1 target, 106 kbps ISO14443A. Switches the RF
  // field on by itself if pn532_rf_off() turned it off.
  rf_off_ = false;
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
  // The whole series must complete while the spool sits still, so its length
  // is worth seeing — it differs by host interface and IRQ/polling mode.
  const uint32_t start_ms = millis();
  for (uint8_t attempt = 1; attempt <= BAMBU_READ_ATTEMPTS; attempt++) {
    blocks_out.clear();
    if (read_bambu_blocks(1, uid, blocks_out)) {
      stats_.bambu_ok++;
      if (attempt > 1) {
        stats_.bambu_retry++;
        ESP_LOGD(NFC_TAG, "Bambu read succeeded on attempt %u/%u", attempt,
                 BAMBU_READ_ATTEMPTS);
      }
      ESP_LOGD(NFC_TAG, "Bambu read took %u ms (%s)",
               (unsigned) (millis() - start_ms), get_connection_type());
      return true;
    }
    if (attempt == BAMBU_READ_ATTEMPTS) break;

    // A failed authentication or read leaves the MIFARE session unusable: the
    // tag must be re-selected with InListPassiveTarget before the next try.
    delay(BAMBU_RETRY_DELAY_MS);
    std::vector<uint8_t> again_uid;
    uint8_t again_sak = 0;
    if (!pn532_detect_tag(again_uid, again_sak)) {
      ESP_LOGD(NFC_TAG, "Bambu read attempt %u failed and the tag is gone (%u ms)",
               attempt, (unsigned) (millis() - start_ms));
      stats_.bambu_fail++;
      return false;
    }
    if (again_uid != uid) {
      ESP_LOGD(NFC_TAG, "Bambu read attempt %u failed and a different tag is present (%u ms)",
               attempt, (unsigned) (millis() - start_ms));
      stats_.bambu_fail++;
      return false;
    }
  }
  ESP_LOGW(NFC_TAG, "Bambu read failed after %u attempts (%u ms)",
           BAMBU_READ_ATTEMPTS, (unsigned) (millis() - start_ms));
  stats_.bambu_fail++;
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
      // Deliberately left empty. The colour value is already carried in
      // color_hex / rgba, so the swatch is correct either way; putting the hex
      // where a colour name belongs only produces inventory entries that read
      // as corrupt ("C0DF16"), and once stored they are indistinguishable from
      // a real name.
      ESP_LOGW(NFC_TAG, "No catalogue colour for %s #%s - leaving the name empty",
               info.detailed_type.c_str(), info.color_hex.c_str());
      info.color_name.clear();
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
  // inventory matching needs.
  //
  // There is deliberately no fallback. The obvious one - deriving something
  // from blocks 4+5 - produces a value that is identical for every spool of a
  // given filament type, because that is what those blocks hold: the type name
  // and the colour/weight record. A colliding identity is worse than none:
  // it silently merges distinct spools into a single inventory entry, and the
  // entry keeps that wrong identity long after the read that produced it. An
  // empty tray UID simply leaves the tag identified by its own card UID, which
  // is unique per tag; the only thing lost is linking a spool's two tags to
  // each other, and that recovers by itself on the next successful read.
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
    ESP_LOGW(NFC_TAG, "Block 9 is all zeros - reporting no tray UID");
    return "";
  }

  ESP_LOGW(NFC_TAG, "Block 9 was not read - reporting no tray UID");
  return "";
}

// ============================================================================
// Component lifecycle
// ============================================================================

void BambuddyNFCComponent::setup() {
  transport_setup();
  if (irq_pin_ != nullptr) {
    irq_pin_->setup();  // configure as input (pull-up set by ESPHome pin schema)
    ESP_LOGI(NFC_TAG, "BambuddyNFC setup (PN532 via %s, IRQ-driven)",
             get_connection_type());
  } else {
    ESP_LOGI(NFC_TAG, "BambuddyNFC setup (PN532 via %s, polling)",
             get_connection_type());
  }
  // Feed WDT then wait for PN532 power-on settle (200 ms covers the PN532's
  // maximum reset/oscillator startup time per the user manual).
  arch_feed_wdt();
  delay(200);

  init_attempts_ = 1;
  last_init_ms_ = millis();
  if (pn532_init()) {
    nfc_ok_ = true;
    if (api_) api_->set_nfc_ok(true);
    ESP_LOGI(NFC_TAG, "PN532 initialized");
  } else {
    // Not fatal: the poll task keeps retrying (PN532_INIT_RETRY_MS), so a
    // reader that powers up late or is plugged in later still comes up
    // without a reboot. This line is usually only visible on the serial
    // console — setup runs before WiFi/API logging — so dump_config() and
    // the retry warnings repeat the state for network log sessions.
    ESP_LOGE(NFC_TAG, "PN532 not responding at boot — will keep retrying every %u s",
             (unsigned) (PN532_INIT_RETRY_MS / 1000));
    nfc_ok_ = false;
    if (api_) api_->set_nfc_ok(false);
  }

  // Spawn the polling task on core 1 (the main loop / LVGL run on core 0), so
  // the PN532's busy-wait handshakes run in parallel and never stall the UI.
  // 8 kB stack: bus paths + mbedTLS HKDF/SHA-256 key derivation (Bambu MIFARE
  // reads) + ESP_LOG formatting + api_ callback std::string building.  6 kB
  // left too little margin: a wild-PC interrupt-WDT crash on core 1 pointed at
  // stack corruption.  The poll loop logs its high-water mark periodically so
  // the remaining headroom stays visible.
  xTaskCreatePinnedToCore(&BambuddyNFCComponent::poll_task_trampoline,
                          "bambuddy_nfc", 8192, this,
                          4 /* priority */, &poll_task_handle_, 1 /* core */);
  ESP_LOGD(NFC_TAG, "NFC polling task started on core 1");
}

// loop() is intentionally empty — polling runs on the dedicated task so the
// PN532's busy-wait handshakes never block the main loop / LVGL.
void BambuddyNFCComponent::loop() {}

void BambuddyNFCComponent::dump_config() {
  ESP_LOGCONFIG(NFC_TAG, "BambuddyNFC (PN532):");
  ESP_LOGCONFIG(NFC_TAG, "  Interface: %s", get_connection_type());
  dump_transport_config();
  if (irq_pin_ != nullptr) {
    log_pin(NFC_TAG, "  IRQ Pin: ", irq_pin_);
  } else {
    ESP_LOGCONFIG(NFC_TAG, "  Ready detection: polling (no IRQ pin), poll interval %u ms",
                  (unsigned) poll_interval_ms_);
  }
  ESP_LOGCONFIG(NFC_TAG, "  Miss threshold: %u", miss_threshold_);
  if (nfc_ok_) {
    ESP_LOGCONFIG(NFC_TAG, "  Reader: OK (IC 0x%02X, firmware %u.%u)", fw_ic_,
                  fw_ver_, fw_rev_);
  } else {
    ESP_LOGCONFIG(NFC_TAG,
                  "  Reader: NOT RESPONDING (%u init attempts) — check wiring, "
                  "power and the mode DIP switches",
                  (unsigned) init_attempts_);
  }
}

void BambuddyNFCComponent::poll_task_trampoline(void *arg) {
  static_cast<BambuddyNFCComponent *>(arg)->poll_task_loop();
}

void BambuddyNFCComponent::poll_task_loop() {
  uint32_t last_stack_diag_ms = 0;
  for (;;) {
    // Scanning disabled (console asleep with "NFC in sleep: Off"): leave the RF
    // field off entirely and idle until scanning is re-enabled on wake.
    if (!scan_enabled_) {
      // The field is still on if the last poll saw a tag — switch it off
      // once so "off" really means no RF (and no RF current).
      if (nfc_ok_ && !rf_off_) pn532_rf_off();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    // Reader never came up (see setup()): retry the full init periodically.
    if (!nfc_ok_) {
      if (millis() - last_init_ms_ >= PN532_INIT_RETRY_MS) {
        last_init_ms_ = millis();
        init_attempts_++;
        if (pn532_init()) {
          link_failures_ = 0;
          nfc_ok_ = true;
          if (api_) api_->set_nfc_ok(true);
          ESP_LOGI(NFC_TAG, "PN532 found after %u attempts — NFC scanning started",
                   (unsigned) init_attempts_);
        } else if (init_attempts_ == 2 || init_attempts_ % 30 == 0) {
          // Once right after boot (visible to a log session opened late),
          // then every ~5 minutes — not on every 10 s retry.
          ESP_LOGW(NFC_TAG,
                   "PN532 still not responding on %s (%u attempts) — check "
                   "wiring, power and the mode DIP switches",
                   get_connection_type(), (unsigned) init_attempts_);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    poll_once();
    log_stats_if_due();
    // Stack-headroom diagnostic (every 5 min): high-water mark is the minimum
    // free stack ever seen, in StackType_t words.  If this trends toward zero
    // the task is the prime suspect for wild-PC / int-WDT crashes on core 1.
    uint32_t now_ms = millis();
    if (now_ms - last_stack_diag_ms >= 300000UL) {
      last_stack_diag_ms = now_ms;
      ESP_LOGD(NFC_TAG, "NFC task stack high-water: %u bytes free",
               (unsigned) (uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    }
    // In IRQ mode the PN532 holds IRQ LOW until we read the response, then
    // de-asserts it.  A 10ms guard prevents hammering the bus if the PN532
    // re-asserts IRQ immediately (e.g. tag still present).
    // In polling mode honour poll_interval_ms_ to give the bus breathing room.
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
  // resp[0] = CMD+1, resp[1] = error code (0x00 = success), then 16 bytes —
  // same layout as mfc_read_block().
  if (resp.size() < 18 || resp[1] != 0x00) return false;
  memcpy(out, resp.data() + 2, 16);
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

void BambuddyNFCComponent::log_stats_if_due() {
  const uint32_t now = millis();
  if (now - last_stats_ms_ < 60000UL) return;
  last_stats_ms_ = now;
  const Stats s = stats_;
  stats_ = Stats{};
  const uint32_t errors = s.no_ack + s.bad_ack + s.bad_resp + s.resp_timeout +
                          s.bambu_fail + s.recoveries;
  // Quiet when idle and healthy; one line per minute otherwise.
  if (s.tags == 0 && errors == 0) return;
  ESP_LOGD(NFC_TAG,
           "NFC stats (60 s, %s): polls=%u tags=%u bambu ok=%u (retried %u) "
           "failed=%u | no_ack=%u bad_ack=%u bad_resp=%u resp_timeout=%u "
           "recoveries=%u",
           get_connection_type(), (unsigned) s.cycles, (unsigned) s.tags,
           (unsigned) s.bambu_ok, (unsigned) s.bambu_retry,
           (unsigned) s.bambu_fail, (unsigned) s.no_ack, (unsigned) s.bad_ack,
           (unsigned) s.bad_resp, (unsigned) s.resp_timeout,
           (unsigned) s.recoveries);
}

void BambuddyNFCComponent::poll_once() {
  if (!nfc_ok_) return;
  stats_.cycles++;

  // Link supervision: a reader that keeps failing exchanges is re-initialised
  // rather than polled forever in a state it will not leave by itself.
  if (link_failures_ >= PN532_LINK_FAILURE_LIMIT) {
    if (state_ == NFCState::TAG_PRESENT) {
      // Nothing can be read while the link is down; report the tag gone
      // instead of leaving it "present" indefinitely. It is re-reported as
      // soon as the reader is back.
      std::string old_uid = uid_to_hex(current_uid_);
      state_ = NFCState::IDLE;
      current_uid_.clear();
      current_sak_ = 0;
      miss_count_ = 0;
      if (api_) api_->on_tag_removed(old_uid);
    }
    if (last_recover_ms_ != 0 &&
        millis() - last_recover_ms_ < PN532_RECOVER_RETRY_MS)
      return;
    if (!pn532_recover()) return;
  }

  std::vector<uint8_t> uid;
  uint8_t sak = 0;
  bool detected = pn532_detect_tag(uid, sak);

  // Diagnostic: surface why a queued write may not be firing.
  if (api_ && api_->has_pending_write()) {
    ESP_LOGD(NFC_TAG,
             "Pending write active: detected=%d sak=0x%02X state=%s",
             detected, detected ? sak : current_sak_,
             state_ == NFCState::TAG_PRESENT ? "PRESENT" : "IDLE");
  }

  if (detected) {
    // Any detection means a tag is on the reader, so the removal counter
    // restarts — including when the tag that arrived is a *different* one.
    miss_count_ = 0;

    // A detection while TAG_PRESENT is not proof that the same tag is still
    // lying there.  Presenting a spool's second tag, or swapping spools,
    // usually happens faster than miss_threshold_ polls, so the reader never
    // sees the gap: it detects the new tag, resets miss_count_, and the old
    // "still present" branch swallows the scan entirely.  On a Bambu spool
    // that is the difference between "either side works" and "only the side
    // I happened to present first works".  Compare the UID and treat a change
    // as removal + arrival, which is what physically happened.
    const bool same_tag =
        (state_ == NFCState::TAG_PRESENT && uid == current_uid_);

    if (state_ != NFCState::TAG_PRESENT || !same_tag) {
      if (state_ == NFCState::TAG_PRESENT) {
        std::string old_uid = uid_to_hex(current_uid_);
        ESP_LOGI(NFC_TAG, "Tag swapped without a gap: %s -> %s",
                 old_uid.c_str(), uid_to_hex(uid).c_str());
        if (api_) api_->on_tag_removed(old_uid);
      }
      // New tag detected
      stats_.tags++;
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
          ESP_LOGD(NFC_TAG, "NDEF format detected: %s", fmt.c_str());
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
    // No tag detected: drop the RF field until the next poll re-enables it,
    // so it is off during the whole inter-poll gap instead of permanently on.
    // Skipped when the poll failed on the link itself — another command would
    // only fail too (link supervision handles that case).
    if (link_failures_ == 0) pn532_rf_off();
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
