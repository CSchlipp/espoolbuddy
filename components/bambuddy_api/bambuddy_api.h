#pragma once
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <atomic>
#include <functional>
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"   // httpd_handle_t (scale server mode only)
#include "esp_http_client.h"   // esp_http_client_method_t (http_request helper)
#include "esp_wifi_types.h"    // wifi_ps_type_t (dynamic WiFi modem sleep in low-power mode)

namespace esphome {
namespace bambuddy_api {

static const char *const TAG = "bambuddy_api";

// Firmware version reported to Bambuddy backend, built as "espoolbuddy-<version>"
// from esphome.project.version so it matches the version shown on the Settings
// screen (ESPHOME_PROJECT_VERSION). Falls back to a bare tag if no project is set.
#ifdef ESPHOME_PROJECT_VERSION
static const char *const FIRMWARE_VERSION = "espoolbuddy-" ESPHOME_PROJECT_VERSION;
#else
static const char *const FIRMWARE_VERSION = "espoolbuddy";
#endif

// Port the console receive server listens on, and the scale pushes to.
// Kept off port 80 so it does not conflict with ESPHome's captive_portal.
static constexpr uint16_t CONSOLE_PUSH_PORT = 8080;

// How often the scale sends a heartbeat to the console (ms).
// The heartbeat response carries any pending command (tare/calibrate/write_tag).
static constexpr uint32_t SCALE_HEARTBEAT_INTERVAL_MS = 1000;
// Faster heartbeat rate used when an NFC tag is present on the scale — reduces
// write_tag command latency so the write starts within ~200 ms of the command
// arriving at the console (instead of waiting up to 1 s at idle rate).
static constexpr uint32_t SCALE_HEARTBEAT_FAST_MS = 200;
// How long (ms) after the last scale push the console still reports the scale
// as connected (scale_ok).
static constexpr uint32_t SCALE_LIVE_TIMEOUT_MS = 10000;

// Remaining-filament bar colour, matching Bambuddy's getFillColor thresholds
// (frontend FilamentHoverCard.tsx). Takes the integer percentage remaining
// (0..100), like Bambuddy: <=15 red, <=30 orange, <=50 yellow, else green.
inline uint32_t remaining_bar_color(int pct) {
  if (pct <= 15) return 0xEF4444;  // red
  if (pct <= 30) return 0xF97316;  // orange
  if (pct <= 50) return 0xEAB308;  // yellow
  return 0x22C55E;                 // green
}

/** State of the backend connection */
enum class BackendState {
  DISCONNECTED,
  CONNECTING,
  REGISTERED,
  ERROR,
};

/** State of the last NFC tag */
enum class NFCTagState {
  ABSENT,
  PRESENT,
};

/** Where the most-recent tag was physically scanned */
enum class TagSource {
  LOCAL,  // console's own PN532
  SCALE,  // scale device, received via push
  AMS,    // synthetic: spool ejected from AMS (no physical NFC tag)
};

/** Filament information decoded from NFC tag / backend response */
struct FilamentInfo {
  std::string tray_uuid;
  std::string material_type;  // "PLA"
  std::string color_hex;      // 6-char RGB
  float min_temp = 0;
  float max_temp = 0;
  std::string tag_type;   // "mifare_classic" | "ntag" | "unknown"
  std::string tag_format; // "bambu_lab" | "open_tag_3d" | "ndef" | "" (unknown/not yet set)
  int sak = 0;
  std::string spool_name;
  int spool_id = 0;

  // Enriched detail from GET /api/v1/inventory/spools/{id}
  std::string brand;        // "eSUN"
  std::string subtype;      // "Plus"
  std::string color_name;   // "Bone White"
  float label_weight_g = 0; // full spool filament weight (g)
  float weight_used_g = 0;  // filament consumed (g)
  float core_weight_g = 0;  // empty spool body weight (g)
};

/** One spool slot inside an AMS unit */
struct AMSTray {
  int slot{0};
  bool present{false};
  std::string material_type;  // e.g. "PLA", "PETG"
  std::string color_hex;      // 6-char hex RGB e.g. "FF0000"
  int nozzle_temp_min{0};
  int nozzle_temp_max{0};
  int spool_id{0};            // Bambuddy inventory spool id (0 = unknown)
  float label_weight_g{0.0f}; // full spool filament weight from inventory (g)
  float remaining_g{0.0f};    // estimated remaining filament (g)
  std::string brand;           // e.g. "eSUN" (from inventory spool.brand)
  std::string subtype;         // e.g. "Matte" (from inventory spool.subtype)
  std::string color_name;      // e.g. "Bone White" (from inventory spool.color_name)
};

/** One physical AMS unit (up to 4 trays) */
struct AMSUnit {
  int id{0};
  std::string name;         // e.g. "AMS 1", "AMS HT"
  std::string custom_name;  // user-set label from /ams-labels (empty if none)
  float temp{0.0f};       // chamber temp °C
  int humidity{0};        // 0..100 % (Bambu reports a 1..5 scale on some units)
  int nozzle{-1};         // -1 unknown, 0 = right (R), 1 = left (L)
  bool is_ht{false};      // true for the single-slot AMS HT (high-temp) dryer
  bool is_vt{false};      // true for an external vt_tray slot (single-spool bypass)
  std::vector<AMSTray> trays;
};

/** A printer known to the Bambuddy backend */
struct PrinterInfo {
  std::string id;
  std::string name;
  bool online{false};
};

/** Lightweight summary of a spool for the picker grid (9 most-recent) */
struct SpoolSummary {
  int id{0};
  std::string material;    // "PLA"
  std::string brand;       // "eSUN"
  std::string color_hex;   // 6-char RGB
};

/** Display state shared between the component and LVGL callbacks */
struct DisplayState {
  BackendState backend_state = BackendState::DISCONNECTED;
  NFCTagState nfc_state = NFCTagState::ABSENT;
  std::string last_tag_uid;
  FilamentInfo current_filament;
  float weight_grams = 0.0f;
  bool weight_stable = false;
  std::string status_message;
  bool nfc_ok = false;
  bool scale_ok = false;
  int uptime_s = 0;
  std::string ip_address;
  float calibration_factor{1.0f};  // current scale calibration factor (from backend)

  // Sticky spool selection: once a tag resolves to a spool it stays displayed
  // until the user dismisses it or another spool is scanned (tag removal alone
  // does NOT clear it).
  bool spool_selected = false;

  // Auto-assign TTL: absolute millis() deadline while a spool awaits slot load.
  // 0 = no active pending assignment (spool_id == 0 or already assigned/expired).
  // The UI lambda computes remaining_s = (spool_assign_expiry_ms - millis()) / 1000.
  uint32_t spool_assign_expiry_ms{0};

  // Brief notification after a successful auto-assignment (cleared by component
  // ~4 s later, which also clears spool_selected).
  bool assign_success{false};
  std::string assign_slot_desc;  // e.g. "AMS-B · Slot 2"

  // Printer & AMS state
  std::vector<PrinterInfo> printers;
  int selected_printer_idx{0};
  std::string selected_printer_id;
  bool printer_connected{false};
  // True when the backend reports the printer is waiting for the user to
  // physically clear the build plate (awaiting_plate_clear on GET
  // /api/v1/printers/{id}/status). Drives the almost-full-screen confirm popup.
  bool awaiting_plate_clear{false};


  std::vector<AMSUnit> ams_units;
  bool dual_nozzle{false};  // printer reports two nozzles → split AMS view L/R
  // Currently fed tray (global ID): 254 = external spool, 255 = no filament,
  // otherwise ams_id * 4 + slot_id.
  int tray_now{255};

  // Recent spools for the NFC picker grid (up to 9, sorted by ID descending)
  std::vector<SpoolSummary> recent_spools;
  bool recent_spools_loading{false};
  // Bumped on every completed fetch so the UI repopulates the picker grid even
  // when the spool count is unchanged (e.g. a new spool was added meanwhile).
  uint32_t recent_spools_generation{0};

  // Deadline (millis) for the unlinked-tag sticky panel. Mirrored from the
  // private unlinked_tag_expiry_ms_ so the YAML can show a countdown.
  uint32_t unlinked_tag_expiry_ms{0};

  // True while the TAG_SCANNED HTTP call to the backend is in flight.
  // Suppresses the "unlinked tag" panel so it does not flash up before the
  // backend response arrives and resolves the tag to a spool (or confirms it
  // is genuinely unlinked).  Cleared by api_tag_scanned() on both success
  // and failure, and by on_tag_removed() in case the tag leaves mid-flight.
  bool tag_resolving{false};

  // Incremented every time on_tag_scanned() fires.  The UI lambda tracks
  // this to detect a new physical scan even when nfc_state stays PRESENT
  // (e.g. during the unlinked-tag sticky TTL).  Use case: if the user
  // discards an unlinked tag, removes it, then taps it again, the
  // generation change clears nfc_unlinked_dismissed so the panel re-appears.
  uint32_t nfc_scan_generation{0};

  // Set after linking a writable (non-Bambu) tag to a spool — prompts the UI
  // to offer writing spool data back to the physical tag.
  bool propose_nfc_write{false};
  bool propose_archive{false};  // set when Quick Weight confirms 0g remaining

  // AMS removal toast — shown on the AMS tab for 5 s after a spool is ejected
  uint32_t ams_removed_toast_expiry_ms{0};
  std::string ams_removed_spool_desc;
};

/**
 * BambuddyAPIComponent
 *
 * ESPHome component that implements the SpoolBuddy daemon behaviour targeting
 * the Bambuddy backend API.  It:
 *   - Registers the device on startup (retries until success)
 *   - Sends periodic heartbeats and processes commands from the backend
 *   - Accepts NFC tag-scanned / tag-removed callbacks from BambuddyNFCComponent
 *   - Accepts scale weight callbacks from the scale sensor automation
 *   - Maintains a DisplayState struct that LVGL lambdas can query
 *   - Mocks SSH key deployment (not supported on ESP32)
 */
class BambuddyAPIComponent : public Component {
 public:
  // ---- ESPHome lifecycle ----
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ---- Configuration setters (called from generated code) ----
  void set_backend_url(const std::string &url) { backend_url_ = url; }
  void set_api_key(const std::string &key) { api_key_ = key; }
  void set_device_id(const std::string &id) { device_id_ = id; }
  void set_hostname(const std::string &hostname) { hostname_ = hostname; }
  void set_heartbeat_interval(uint32_t ms) { heartbeat_interval_ms_ = ms * 1000; }
  void set_scale_report_interval(uint32_t ms) { scale_report_interval_ms_ = ms; }
  void set_printer_poll_interval(uint32_t s) { printer_poll_interval_ms_ = s * 1000; }
  // Scale server mode: this device IS the scale — serves weight/tare/calibrate
  // over HTTP and does not connect to BamBuddy at all.
  void set_scale_mode(bool v) { scale_mode_ = v; }
  // Scale device only: base URL of the console to push data to — no port suffix
  // (e.g. "http://spoolbuddy-console.local").  CONSOLE_PUSH_PORT is appended
  // automatically when building push request URLs and starting the receive server.
  void set_console_url(const std::string &url) { console_url_ = url; }
  // Sleep / low-power mode. After sleep_timeout of UI inactivity the YAML sleep
  // state machine turns the backlight off and calls set_low_power(true), which
  // multiplies the heartbeat and printer-poll cadence by sleep_factor to cut
  // backend traffic. set_sleep_timeout takes seconds (0 = feature disabled).
  void set_sleep_timeout(uint32_t s) { sleep_timeout_ms_ = s * 1000; }
  void set_sleep_factor(uint32_t f) { low_power_factor_ = (f < 1) ? 1 : f; }
  uint32_t sleep_timeout_ms() const { return sleep_timeout_ms_; }
  // Which Bambuddy inventory backend to call: local DB ("internal", default)
  // or Spoolman ("spoolman").  The two expose different endpoint shapes
  // (/inventory/... vs /spoolman/inventory/...) and some different request/
  // response field names, so every inventory call branches on this flag.
  void set_spoolman_inventory(bool v) { spoolman_inventory_ = v; }
  // Console only: header clock format. true = 24-hour (14:05), false = 12-hour (2:05 PM).
  void set_clock_24h(bool v) { clock_24h_ = v; }
  bool clock_24h() const { return clock_24h_; }

  // Called by UI to persist printer selection across reboots
  void set_selected_printer(int idx);

  // Called by the AMS slot detail modal to remove an inventory assignment for
  // a given slot.  Immediately zeroes the local spool_id for visual feedback
  // and enqueues an HTTP DELETE to the backend.
  void clear_slot_assignment(int ams_id, int tray_slot);

  // Directly assign an inventory spool to an (empty) AMS slot — used by the
  // assign-by-id dialog, no tag scan required. Enqueues an HTTP POST; on success
  // the assignment cache is refreshed so the slot shows the spool.
  void assign_spool_to_slot(int spool_id, int ams_id, int tray_slot);

  // NFC picker — fetch up to 9 most-recently-created spools for the picker UI.
  void request_recent_spools();

  // Link the current NFC tag (last_tag_uid) to an existing inventory spool.
  // Enqueues LINK_TAG_TO_SPOOL; on success fetches the spool and sets
  // spool_selected = true.
  void link_tag_to_spool(int spool_id);

  // Remove the tag link from the current spool (sends PATCH with tag_uid="").
  // On success sets spool_selected = false so the unlinked panel re-appears.
  void unlink_current_tag();

  // Create a minimal PLA spool in inventory pre-linked to the current tag,
  // then fetch it and set spool_selected = true.
  void create_spool_from_tag();

  // Record the current scale reading (filament + spool body) as the new weight
  // for a spool. Enqueues UPDATE_SPOOL_WEIGHT; the HTTP task POSTs the raw
  // total_grams to POST /scale/update-spool-weight and BamBuddy handles
  // core_weight subtraction and Spoolman routing server-side. The local display
  // is updated optimistically using core_weight_g fetched from the spool detail.
  void record_scale_weight(int spool_id, float total_grams);

  // Archive a spool; dismiss_archive_proposal() cancels without action.
  void archive_spool(int spool_id);

  // Build-plate-clear confirmation popup (gated by a runtime settings toggle
  // in the YAML). confirm_plate_cleared() optimistically clears the local
  // flag and notifies the backend via POST /api/v1/printers/{id}/clear-plate.
  // dismiss_plate_clear_popup() (the "Discard" button) does NOT touch
  // awaiting_plate_clear — the backend is not notified, so its flag stays
  // true until it's actually cleared some other way. The YAML-side
  // plate_clear_dismissed global (not this flag) is what suppresses the
  // popup from reappearing on the next poll while it's still true.
  void confirm_plate_cleared();
  void dismiss_plate_clear_popup() { set_status("Plate clear dismissed"); }
  void dismiss_archive_proposal() {
    lock_state();
    display_state_.propose_archive = false;
    unlock_state();
  }

  // Dismiss the "write spool data to NFC tag?" proposal banner.
  void dismiss_nfc_write_proposal() {
    lock_state();
    display_state_.propose_nfc_write = false;
    unlock_state();
  }

  // Called by the NFC component after reading NDEF pages to refine the format
  // detected from SAK alone.  Runs on the NFC poll task, so it must be brief.
  void set_tag_format(const std::string &fmt) {
    lock_state();
    display_state_.current_filament.tag_format = fmt;
    unlock_state();
  }

  // Called by UI to clear the currently displayed (sticky) spool.
  void dismiss_spool() {
    lock_state();
    display_state_.spool_selected = false;
    display_state_.current_filament = FilamentInfo{};
    display_state_.status_message = "Spool dismissed";
    display_state_.spool_assign_expiry_ms = 0;
    display_state_.assign_success = false;
    display_state_.assign_slot_desc.clear();
    display_state_.propose_nfc_write = false;
    display_state_.propose_archive = false;
    display_state_.unlinked_tag_expiry_ms = 0;
    pending_assign_spool_id_ = 0;
    unlinked_tag_expiry_ms_ = 0;
    unlock_state();
    assign_clear_ms_ = 0;
  }

  // ---- Callbacks from NFC component ----
  void on_tag_scanned(const std::string &uid, const std::string &tray_uuid,
                      int sak, const std::string &tag_type);
  void on_tag_removed(const std::string &uid);

  // ---- Callbacks from scale sensor ----
  void on_scale_reading(float grams, bool stable, int raw_adc = 0);

  // ---- Pending NTAG write (set by write_tag command) ----
  bool has_pending_write() const { return pending_write_active_; }
  const std::vector<uint8_t> &pending_write_data() const { return pending_write_ndef_; }
  int pending_write_spool_id() const { return pending_write_spool_id_; }
  void on_write_tag_result(const std::string &uid, bool success, const std::string &msg);
  void clear_pending_write() { pending_write_active_ = false; }

  // ---- Scale server status (scale_mode_ only) ----
  // True when the last push from this scale to the console succeeded within
  // timeout_ms. Drives the scale's status LED (green = connected, blue =
  // console absent). Safe to call from any task (uint32_t read is atomic).
  bool is_console_connected(uint32_t timeout_ms = 5000) const {
    return last_push_ok_ms_ > 0 &&
           (millis() - last_push_ok_ms_) < timeout_ms;
  }

  // Thread-safe copy of the display state for the LVGL render lambda.
  // Takes the state mutex, copies, releases — never blocks on HTTP.
  DisplayState snapshot();

  // Thread-safe setter for the NFC health flag (called from the NFC task).
  void set_nfc_ok(bool ok) {
    lock_state();
    display_state_.nfc_ok = ok;
    unlock_state();
  }

  // ---- Tare support ----
  // Single tare entry point for both the UI tare button and the backend
  // "tare" command (handle_command).  Routing: scale device → local_tare();
  // console with push-mode scale → deliver via next heartbeat response;
  // console with local weight only → capture reading + notify backend.
  void request_tare();

  // ---- Local tare (scale_mode only) ----
  // Called by the physical tare button or by a heartbeat-delivered tare command.
  // After updating tare_offset_, forces one weight push so the console immediately
  // sees the zeroed reading — necessary because the ESPHome sensor's delta filter
  // suppresses on_scale_reading() callbacks when the raw sensor value is stable.
  // No-op when not in scale_mode.
  void local_tare() {
    if (!scale_mode_) return;
    lock_state();
    tare_offset_           = display_state_.weight_grams;
    float  force_g         = display_state_.weight_grams;
    bool   force_stable    = display_state_.weight_stable;
    unlock_state();
    save_calibration_nvs();
    // Force-push the tared weight (will compute net≈0) unless a push is
    // already queued — if one is pending, it will use the new tare_offset_
    // at processing time and give the correct result.
    if (!console_url_.empty() && !push_weight_pending_.exchange(true)) {
      HttpJob job;
      job.kind = HttpJob::SCALE_PUSH_WEIGHT;
      job.f1   = force_g;
      job.b1   = force_stable;
      enqueue_job(job);
    }
  }

  // ---- Calibration support ----
  // Single calibrate entry point for both the UI calibrate button and the
  // backend "calibrate_with_weight" command (handle_command).  Place a known
  // reference weight on the scale, then call this with its value.  Push-mode
  // scale: the command is delivered via the next heartbeat response.  Local:
  // captures the current reading and sends both values to the backend, which
  // computes a new calibration_factor (returned in the next heartbeat
  // response and stored in calibration_factor_).  Rejects values <= 0.
  void request_calibration(float reference_weight_g);

  // ---- Sleep / low-power mode ----
  // Enter/leave low-power mode (called by the UI sleep state machine). Entering
  // stretches the heartbeat / printer-poll intervals by sleep_factor. Leaving
  // forces an immediate heartbeat + printer/AMS poll on the next task tick (so
  // the UI refreshes at once on wake) and then resumes the normal cadence.
  void set_low_power(bool enable);

  // Configure the ESP-IDF power-management profile. Call with true when
  // entering sleep (WiFi already disabled) to drop CPU to 80 MHz and enable
  // automatic light sleep between FreeRTOS ticks; false on wake to restore
  // 240 MHz before re-enabling WiFi. No-op when CONFIG_PM_ENABLE is not set.
  void configure_pm(bool light_sleep);

  // ---- Scale server lifecycle (scale_mode only) ----
  // Stops and restarts the httpd. Call from the WiFi on_connect hook to
  // recover the server socket after a disconnect/reconnect cycle.
  void restart_scale_server();

 protected:
  // ------------------------------------------------------------------
  // Background HTTP task — keeps all blocking network I/O off the main
  // loop so LVGL / touch stay responsive.
  // ------------------------------------------------------------------
  struct HttpJob {
    enum Kind {
      TAG_SCANNED,
      TAG_REMOVED,
      SCALE_READING,
      WRITE_RESULT,
      UPDATE_TARE,
      CLEAR_ASSIGNMENT,      // remove an inventory slot assignment
      ASSIGN_SPOOL,          // POST /inventory/assignments (assign a spool to a slot)
      FETCH_RECENT_SPOOLS,   // GET /inventory/spools → fill recent_spools
      LINK_TAG_TO_SPOOL,     // PATCH /inventory/spools/{id}/link-tag (or unlink)
      CREATE_SPOOL_FROM_TAG, // POST /inventory/spools with current tag_uid
      UPDATE_CALIBRATION,      // POST /calibration/set-factor (reference + measured net)
      UPDATE_SPOOL_WEIGHT,     // PATCH /inventory/spools/{id} {"weight_used": X}
      ARCHIVE_SPOOL,           // POST /inventory/spools/{id}/archive
      CLEAR_PLATE,             // POST /api/v1/printers/{id}/clear-plate
      // Scale push mode: scale → console (only processed when scale_mode_)
      SCALE_PUSH_WEIGHT,       // POST {console_url}/scale/weight
      SCALE_PUSH_NFC_SCANNED,  // POST {console_url}/scale/nfc/tag-scanned
      SCALE_PUSH_NFC_REMOVED,  // POST {console_url}/scale/nfc/tag-removed
    } kind;
    std::string s1, s2, s3;  // generic string params
    int i1{0};
    int i2{0};  // second int param (e.g. tray_slot for CLEAR_ASSIGNMENT)
    int i3{0};  // third int param (e.g. spool_id for CLEAR_ASSIGNMENT in Spoolman mode)
    float f1{0.0f};
    float f2{0.0f};  // second float param (e.g. measured_g for UPDATE_CALIBRATION)
    bool b1{false};
  };

  static void http_task_trampoline(void *arg);
  void http_task_loop();
  void enqueue_job(const HttpJob &job);
  bool dequeue_job(HttpJob &job);

  // State-mutex helpers (guard all concurrent access to display_state_)
  void lock_state() { if (state_mutex_) xSemaphoreTake(state_mutex_, portMAX_DELAY); }
  void unlock_state() { if (state_mutex_) xSemaphoreGive(state_mutex_); }
  void set_status(const std::string &m) {
    lock_state();
    display_state_.status_message = m;
    unlock_state();
  }

  // ------------------------------------------------------------------
  // Internal helpers
  // ------------------------------------------------------------------
  void ensure_device_id();
  std::string get_ip_address();

  // ------------------------------------------------------------------
  // HTTP helpers — all thin wrappers around http_request(); each returns
  // true and fills response_body on a 2xx status.
  //   http_post          /api/v1/spoolbuddy prefix (daemon endpoints)
  //   http_*_api         /api/v1 prefix (general Bambuddy API)
  //   http_post_direct   full URL (scale→console push: short timeout, no
  //                      API key, failures logged at DEBUG since pushes
  //                      fail routinely while the console is offline)
  // ------------------------------------------------------------------
  bool http_request(esp_http_client_method_t method, const std::string &url,
                    const std::string &json_body, std::string &response_body,
                    int timeout_ms, bool with_api_key, bool quiet);
  // Build <backend_url><prefix><path>, trimming a trailing '/' from backend_url.
  std::string backend_url_for(const char *prefix, const std::string &path) const;
  bool http_post(const std::string &path, const std::string &json_body,
                 std::string &response_body);
  bool http_get_api(const std::string &path, std::string &response_body);
  bool http_post_api(const std::string &path, const std::string &json_body,
                     std::string &response_body);
  bool http_patch_api(const std::string &path, const std::string &json_body,
                      std::string &response_body);
  bool http_delete_api(const std::string &path, std::string &response_body);
  bool http_post_direct(const std::string &url, const std::string &json_body,
                        std::string &response_body);

  // Network readiness guard — prevents HTTP before IP is assigned
  bool is_network_ready();

  // API calls (mirror api_client.py exactly)
  bool api_register_device();
  void api_heartbeat();
  // Unified spool activation: sets all DisplayState fields for quick-weight mode
  // and queues api_get_spool() for enrichment. Called from api_tag_scanned()
  // (NFC path) and api_get_ams() (AMS-removal path).
  void activate_spool(FilamentInfo fi, const std::string &source_uid, TagSource source);
  bool api_tag_scanned(const std::string &uid, const std::string &tray_uuid,
                       int sak, const std::string &tag_type);
  bool api_tag_removed(const std::string &uid);
  bool api_scale_reading(float grams, bool stable, int raw_adc);
  bool api_update_tare(float tare_offset);
  // POST /devices/{id}/calibration/set-factor — report a reference/measured
  // pair (translated to the backend's known_weight / raw_adc / tare_raw_adc
  // form) so the backend computes and stores the corrected calibration_factor.
  bool api_report_calibration_point(float reference_weight_g, float measured_weight_g);
  bool api_write_tag_result(int spool_id, const std::string &uid,
                            bool success, const std::string &message);
  bool api_diagnostic_result(const std::string &diagnostic, bool success,
                             const std::string &output, int exit_code);
  bool api_system_command_result(const std::string &command, bool success,
                                 const std::string &message);
  void api_get_printers();
  void api_get_ams();
  void api_get_printer_status(const std::string &printer_id);

  // Scale server (scale_mode_ == true only)
  void start_scale_server();
  void load_calibration_nvs();
  void save_calibration_nvs();
  // Static HTTP handlers registered with esp_http_server.
  // user_ctx in every httpd_req_t points to the BambuddyAPIComponent instance.
  static esp_err_t scale_http_weight(httpd_req_t *req);
  static esp_err_t scale_http_tare(httpd_req_t *req);
  static esp_err_t scale_http_calibrate(httpd_req_t *req);

  // Console receive server (started in console/standard mode, port CONSOLE_PUSH_PORT)
  // Accepts POST requests from the scale device and routes them through the
  // same on_tag_scanned / on_tag_removed / SCALE_READING paths as local events.
  void start_console_server();
  static esp_err_t console_http_scale_weight(httpd_req_t *req);
  static esp_err_t console_http_scale_nfc_scanned(httpd_req_t *req);
  static esp_err_t console_http_scale_nfc_removed(httpd_req_t *req);
  static esp_err_t console_http_scale_nfc_write_result(httpd_req_t *req);
  static esp_err_t console_http_scale_heartbeat(httpd_req_t *req);

  // Scale push helpers (scale_mode_ && !console_url_.empty() only)
  bool api_scale_push_weight(float grams, bool stable);
  void api_scale_push_heartbeat();
  void api_scale_push_nfc_scanned(const std::string &uid,
                                   const std::string &tray_uuid,
                                   int sak, const std::string &tag_type);
  void api_scale_push_nfc_removed(const std::string &uid);
  void api_scale_push_write_result(int spool_id, const std::string &uid,
                                    bool success, const std::string &msg);
  // Fetch inventory assignments for the given printer and populate spool_id
  // fields in display_state_.ams_units.  Called from api_get_ams() only when
  // the AMS content changed, so it does not fire every poll cycle.
  void api_get_assignments(const std::string &printer_id);
  // Fetch the user's custom AMS names from /printers/{id}/ams-labels into
  // ams_labels_; api_get_ams() stamps them onto each non-external unit.
  void api_get_ams_labels(const std::string &printer_id);
  // Merge cached_assignments_ into display_state_.ams_units, resetting every
  // tray's assignment fields first so removed assignments clear too.
  // Caller must hold the state mutex.
  void apply_cached_assignments_locked();
  // Console: true when the scale has pushed any message (heartbeat, weight,
  // NFC event) within SCALE_LIVE_TIMEOUT_MS.  Reads millis() at call time, so
  // a concurrent last_scale_push_ms_ update cannot cause an unsigned wrap.
  bool scale_live() const {
    return last_scale_push_ms_ > 0 &&
           (millis() - last_scale_push_ms_) < SCALE_LIVE_TIMEOUT_MS;
  }
  // spool_id is the id that was assigned to the slot (caller looks it up from
  // display_state_ before clearing). Only used in Spoolman mode, where the
  // unassign endpoint is keyed by spool id, not by printer/ams/tray.
  bool api_clear_assignment(const std::string &printer_id, int ams_id, int tray_slot,
                             int spool_id);
  // Fetch enriched spool detail and merge it into display_state_.current_filament.
  // Internal mode: GET /api/v1/inventory/spools/{id}.
  // Spoolman mode: no per-id GET exists, so this streams the spool list and
  // matches the id client-side (same brace-matcher as api_get_recent_spools).
  void api_get_spool(int spool_id, bool check_empty = false);

  // Assign pending spool to a loaded slot.
  // Internal: POST /api/v1/inventory/assignments {spool_id,...}.
  // Spoolman: POST /api/v1/spoolman/inventory/slot-assignments {spoolman_spool_id,...}.
  bool api_assign_spool(int spool_id, const std::string &printer_id,
                        int ams_id, int tray_id);

  // Fetch all active spools, sort by id desc, keep ≤ 9 in recent_spools.
  void api_get_recent_spools();
  // Link/unlink a tag to a spool. Pass uid="" to unlink.
  // Internal: PATCH /inventory/spools/{id}/link-tag (empty uid clears it).
  // Spoolman: PATCH /spoolman/inventory/spools/{id}/tag to link (its tag_uid
  // field requires length ≥ 8, so unlink instead goes through the general
  // PATCH /spoolman/inventory/spools/{id} with tag_uid:null).
  void api_link_tag(int spool_id, const std::string &uid,
                    const std::string &tray_uuid, const std::string &tag_type);
  void api_update_spool_weight(int spool_id, float remaining_g);
  void api_archive_spool(int spool_id);
  // POST /api/v1/printers/{id}/clear-plate — notifies the backend the build
  // plate has been physically cleared.
  void api_clear_plate(const std::string &printer_id);
  // Create a minimal PLA spool linked to uid.
  // Internal: single POST /inventory/spools with tag_uid in the body.
  // Spoolman: POST /spoolman/inventory/spools has no tag field, so this does
  // a create POST followed by a PATCH .../tag to link the new spool.
  void api_create_spool_from_tag(const std::string &uid);

  // JSON array / nested-object parsers
  static std::vector<std::string> json_array_objects(const std::string &json);
  static void parse_printer_list(const std::string &json,
                                 std::vector<PrinterInfo> &out);
  static void parse_ams_data(const std::string &json,
                             std::vector<AMSUnit> &out);

  // Command handlers (received in heartbeat response)
  void handle_command(const std::string &cmd, const std::string &payload_json,
                      const std::string &response_json);

  // JSON helpers
  static std::string json_string(const std::string &s);
  static std::string bool_str(bool v) { return v ? "true" : "false"; }
  // Parse a specific string field from a minimal JSON response
  static std::string parse_json_string(const std::string &json,
                                       const std::string &key);
  static bool parse_json_bool(const std::string &json, const std::string &key,
                              bool default_val = false);
  static float parse_json_float(const std::string &json, const std::string &key,
                                float default_val = 0.0f);
  static int parse_json_int(const std::string &json, const std::string &key,
                            int default_val = 0);

  // ------------------------------------------------------------------
  // Configuration
  // ------------------------------------------------------------------
  std::string backend_url_;
  std::string api_key_;
  std::string device_id_;
  std::string hostname_{"SpoolBuddy-ESP"};
  uint32_t heartbeat_interval_ms_{10000};
  uint32_t scale_report_interval_ms_{1000};
  bool spoolman_inventory_{false};  // false = Bambuddy local DB, true = Spoolman
  bool clock_24h_{true};  // console only: header clock format
  // Persistent HTTPS connection to the backend. Reused across requests so the
  // TLS handshake (full mbedTLS cert chain verification) happens once on first
  // connect and again only after a dropped connection — not on every HTTP call.
  // Only backend_url_ requests use this; direct scale-push calls keep the
  // per-call create/cleanup pattern (different host, short 3 s timeout).
  esp_http_client_handle_t http_client_{nullptr};

  // Sleep / low-power mode
  uint32_t sleep_timeout_ms_{600000};   // UI inactivity before deep sleep (0 = disabled)
  uint32_t low_power_factor_{6};         // heartbeat / poll interval multiplier while asleep
  std::atomic<bool> low_power_{false};   // true while running the reduced sleep cadence
  wifi_ps_type_t awake_ps_mode_{WIFI_PS_NONE};  // awake WiFi PS mode, captured on sleep / restored on wake

  // NFC tag-scan retry after WiFi wake: when api_tag_scanned() fails because
  // the backend is not yet registered (WiFi reconnecting after sleep), we keep
  // tag_resolving=true and re-enqueue the lookup once the backend comes up.
  bool pending_tag_retry_{false};
  uint32_t retry_started_ms_{0};        // millis() when the retry was armed (for timeout)
  std::string retry_uid_;
  std::string retry_tray_uuid_;
  std::string retry_tag_type_;
  int retry_sak_{0};

  // ------------------------------------------------------------------
  // Runtime state
  // ------------------------------------------------------------------
  bool registered_{false};
  uint32_t start_ms_{0};
  uint32_t last_heartbeat_ms_{0};
  uint32_t last_register_ms_{0};
  uint32_t last_scale_report_ms_{0};

  // Printer / AMS polling
  uint32_t printer_poll_interval_ms_{30000};
  uint32_t last_printer_poll_ms_{0};
  uint32_t last_ams_fast_poll_ms_{0};  // tracks the fast AMS-only poll while assign is pending
  bool printers_fetched_{false};

  // Scale server / push mode
  bool scale_mode_{false};
  std::string console_url_;          // base URL of the console, no port (scale push mode only)
  httpd_handle_t scale_server_handle_{nullptr};
  httpd_handle_t console_server_handle_{nullptr};
  // Scale push mode: millis() of the last successful push to the console.
  // Written by api_scale_push_heartbeat() and the NFC push helpers; read by
  // is_console_connected() for the status LED.  Atomic uint32 read — no lock.
  volatile uint32_t last_push_ok_ms_{0};
  uint32_t last_heartbeat_push_ms_{0};  // when the last heartbeat was sent (push task only)

  // Console push receive: millis() of the last message (heartbeat, weight, or
  // NFC event) from the scale.  Written by every console_http_scale_* handler
  // under state_mutex_.  Read by api_heartbeat() / api_register_device() and
  // the scale-liveness maintenance block in http_task_loop().
  uint32_t last_scale_push_ms_{0};

  // Pending console→scale command: delivered in the next /scale/heartbeat response.
  // Written by request_tare() / request_calibration() / handle_command() (under state_mutex_).
  // Read and cleared by console_http_scale_heartbeat() (under state_mutex_).
  std::string pending_scale_cmd_;
  // True after a "tare" command was handed to the scale, until the next weight
  // push carrying a tare_offset arrives.  Forces an unconditional set-tare POST
  // to the backend even when the offset value did not change — the Bambuddy UI
  // confirms tare completion by watching last_calibrated_at advance (15 s
  // timeout), which only happens on a set-tare POST.  Guarded by state_mutex_.
  bool tare_ack_pending_{false};
  float pending_scale_cmd_value_{0.0f};
  // Extra JSON key-value pairs for commands that carry a payload (e.g. write_tag).
  // Serialised as a comma-prefixed JSON fragment appended directly to the response object.
  std::string pending_scale_cmd_payload_;

  // Source of the most-recently-scanned NFC tag — LOCAL (console's PN532) or
  // SCALE (pushed from the scale device).  Written by on_tag_scanned() (LOCAL)
  // and console_http_scale_nfc_scanned() (SCALE).  Read by handle_command() to
  // decide where to route write_tag.  Int-sized enum, atomic on Xtensa; no mutex.
  TagSource last_tag_source_{TagSource::LOCAL};

  // Scale buffering
  float last_reported_weight_{0.0f};
  bool last_reported_valid_{false};
  static constexpr float REPORT_THRESHOLD = 2.0f;
  static constexpr uint32_t ASSIGN_TTL_MS = 60000;  // ms a pending scan stays valid

  // Tare / calibration state
  float tare_offset_{0.0f};
  float calibration_factor_{1.0f};

  // Pending AMS auto-assign (guarded by state_mutex_)
  // Set when a tag resolves to a known spool; cleared on successful assignment
  // or when the user dismisses the spool.
  int pending_assign_spool_id_{0};
  uint32_t pending_assign_expiry_ms_{0};

  // TTL for unlinked tags: keeps nfc_state=PRESENT after the tag is removed so
  // the user has time to tap "Add to inventory" or "Assign spool".  0 = idle.
  uint32_t unlinked_tag_expiry_ms_{0};

  // Millis() timestamp at which to clear the assign-success overlay and spool
  // display (set ~4 s after a successful auto-assignment). 0 = idle.
  // Written by the HTTP task; also cleared by dismiss_spool() from the UI
  // task — uint32 writes are atomic on ESP32, so no mutex is needed.
  uint32_t assign_clear_ms_{0};

  // Previous AMS snapshot used for slot-change detection.
  // Only accessed from the HTTP task — no lock required.
  std::vector<AMSUnit> prev_ams_units_;

  // Cached inventory assignments — re-merged into display_state_ after every
  // api_get_ams() poll so spool IDs survive the fresh parse overwrite.
  // Only accessed from the HTTP task — no lock required.
  struct SlotAssignment {
    int   ams_id{0};
    int   tray_id{0};
    int   spool_id{0};
    float label_weight_g{0.0f};
    float remaining_g{0.0f};
    std::string material_type;
    std::string color_hex;
    std::string brand;
    std::string subtype;
    std::string color_name;
  };
  std::vector<SlotAssignment> cached_assignments_;
  // Custom AMS names (ams_id → label) from /ams-labels, re-stamped onto units on
  // every api_get_ams() poll. Only accessed from the HTTP task — no lock needed.
  std::map<int, std::string> ams_labels_;

  // Deduplication flag: true while a SCALE_PUSH_WEIGHT job is in the queue or
  // being processed.  Prevents accumulation of back-to-back weight jobs when
  // the console is unreachable and each HTTP attempt stalls on mDNS resolution.
  // Written by on_scale_reading() and the scale push task (core-1); atomic.
  std::atomic<bool> push_weight_pending_{false};

  // Pending write — written by HTTP task, read by NFC (main) task.
  // atomic gate: the ndef vector is filled BEFORE the flag is set true, and
  // the NFC reader only touches the vector while the flag reads true, so the
  // release/acquire ordering of the atomic makes the hand-off race-free.
  std::atomic<bool> pending_write_active_{false};
  int pending_write_spool_id_{0};
  std::vector<uint8_t> pending_write_ndef_;

  // ------------------------------------------------------------------
  // Threading
  // ------------------------------------------------------------------
  TaskHandle_t  http_task_handle_{nullptr};
  StaticTask_t  http_task_tcb_{};            // TCB for xTaskCreateStaticPinnedToCore (must stay in internal SRAM)
  SemaphoreHandle_t state_mutex_{nullptr};   // guards display_state_
  SemaphoreHandle_t job_mutex_{nullptr};     // guards job_queue_
  std::deque<HttpJob> job_queue_;

  // Display / shared state (guarded by state_mutex_)
  DisplayState display_state_;
};

}  // namespace bambuddy_api
}  // namespace esphome
