#include "bambuddy_api.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"

// ESP-IDF
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"     // esp_restart()
#include "esp_heap_caps.h"  // heap_caps_malloc() for PSRAM task stack
#include "esp_task_wdt.h"   // esp_task_wdt_add() / reset for HTTP task

#include <cstring>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace esphome {
namespace bambuddy_api {

// ---------------------------------------------------------------------------
// HTTP response accumulation buffer used by the ESP-IDF event handler
// ---------------------------------------------------------------------------
struct HttpContext {
  std::string body;
  int status_code = 0;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  auto *ctx = reinterpret_cast<HttpContext *>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      if (evt->data_len > 0 && ctx) {
        ctx->body.append(reinterpret_cast<const char *>(evt->data),
                         evt->data_len);
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

// Escape double-quotes and backslashes for embedding in a JSON string literal.
static std::string esc(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Decode a hex string ("0A1B…") into raw bytes; a trailing odd nibble is ignored.
static std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back((uint8_t)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
  return out;
}

// Send a JSON response on an esp_http_server request.
static esp_err_t send_json(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

// Read a request body (up to 511 bytes) into `out`.
// Returns false when no body was received.
static bool read_req_body(httpd_req_t *req, std::string &out) {
  char buf[512] = {};
  int len = (req->content_len > 0 && req->content_len < (int)sizeof(buf) - 1)
                ? req->content_len
                : (int)sizeof(buf) - 1;
  int received = httpd_req_recv(req, buf, len);
  if (received <= 0) return false;
  out.assign(buf, received);
  return true;
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::setup() {
  start_ms_ = millis();
  ensure_device_id();
  display_state_.ip_address = get_ip_address();

  // Always create the state mutex — it guards display_state_ which is accessed
  // by both the main/LVGL task and the HTTP task (or scale HTTP server task).
  state_mutex_ = xSemaphoreCreateMutex();
  job_mutex_   = xSemaphoreCreateMutex();

  // ---- Scale server mode ------------------------------------------------
  // The scale device runs a lightweight HTTP server for weight/tare/calibrate
  // and does not communicate with BamBuddy at all.
  if (scale_mode_) {
    display_state_.status_message = "Scale server ready";
    ESP_LOGI(TAG, "BambuddyAPI scale server mode: device_id=%s", device_id_.c_str());
    load_calibration_nvs();
    start_scale_server();

    // If a console URL is configured, start a push task that forwards weight
    // and NFC events to the console.  The HTTP task runs in push-only mode
    // (scale_mode_ == true); it never contacts BamBuddy directly.
    if (!console_url_.empty()) {
      ESP_LOGI(TAG, "Scale push mode active — console: %s", console_url_.c_str());
      constexpr uint32_t PUSH_STACK_BYTES = 12288;
      auto *push_stack = static_cast<StackType_t *>(
          heap_caps_malloc(PUSH_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (push_stack == nullptr)
        push_stack = static_cast<StackType_t *>(
            heap_caps_malloc(PUSH_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      http_task_handle_ = xTaskCreateStaticPinnedToCore(
          &BambuddyAPIComponent::http_task_trampoline,
          "scale_push",
          PUSH_STACK_BYTES / sizeof(StackType_t),
          this, 5, push_stack, &http_task_tcb_, 1);
      ESP_LOGI(TAG, "Scale push worker task started on core 1");
    }
    return;  // no BamBuddy communication
  }

  // ---- Console / standard mode ------------------------------------------
  display_state_.status_message = "Registering with Bambuddy...";
  ESP_LOGI(TAG, "BambuddyAPI setup: device_id=%s backend=%s",
           device_id_.c_str(), backend_url_.c_str());
  if (backend_url_.rfind("http://", 0) != 0 &&
      backend_url_.rfind("https://", 0) != 0) {
    ESP_LOGW(TAG,
             "backend_url '%s' does not start with http:// or https:// — "
             "HTTP requests will fail",
             backend_url_.c_str());
  }

  // Start the console HTTP receive server (CONSOLE_PUSH_PORT) so the scale device
  // can push weight and NFC events to us.
  start_console_server();

  // Start the background HTTP task (mutexes already created above).
  // ESPHome's main loop (and LVGL) run on core 0 in the esp-idf port.  Pin the
  // HTTP worker to core 1 so its blocking calls and JSON parsing run in true
  // parallel and never stall the UI.
  //
  // Stack lives in PSRAM to reclaim internal SRAM for the LVGL DMA buffer.
  // On ESP32-S3 the PSRAM is cache-mapped so FreeRTOS context-switches and
  // stack accesses work correctly — only the very small StaticTask_t TCB must
  // stay in internal SRAM, which it does as a member variable.
  // Fallback to internal SRAM if PSRAM allocation fails (e.g. early boot).
  // 28 kB budget: mbedTLS ~10–12 kB + esp_http_client ~3 kB + api_get_printers
  // calls api_get_assignments while still on its own frame (two HTTP client
  // lifetimes overlap), plus ostringstream and JSON buffers.  20 kB was too
  // tight once the LINK_TAG ostringstream and printer-status path were added.
  constexpr uint32_t HTTP_STACK_BYTES = 28672;
  auto *http_stack = static_cast<StackType_t *>(
      heap_caps_malloc(HTTP_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (http_stack == nullptr) {
    ESP_LOGW(TAG, "PSRAM stack alloc failed — falling back to internal SRAM");
    http_stack = static_cast<StackType_t *>(
        heap_caps_malloc(HTTP_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  http_task_handle_ = xTaskCreateStaticPinnedToCore(
      &BambuddyAPIComponent::http_task_trampoline,
      "bambuddy_http",
      HTTP_STACK_BYTES / sizeof(StackType_t),  // depth in words, not bytes
      this,
      5 /* priority */,
      http_stack,
      &http_task_tcb_,
      1 /* core */);
  ESP_LOGI(TAG, "BambuddyAPI HTTP worker task started on core 1 (stack in %s)",
           (reinterpret_cast<uintptr_t>(http_stack) >= 0x3C000000 ? "PSRAM" : "SRAM"));
}

// loop() is intentionally empty: all network I/O runs on the HTTP task so the
// main loop (LVGL/touch) is never blocked.
void BambuddyAPIComponent::loop() {}

// ---------------------------------------------------------------------------
// Background HTTP task
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::http_task_trampoline(void *arg) {
  static_cast<BambuddyAPIComponent *>(arg)->http_task_loop();
}

void BambuddyAPIComponent::http_task_loop() {
  // Register with the ESP-IDF task WDT only in console mode.  In scale push
  // mode the push task blocks inside esp_http_client_perform() during mDNS
  // resolution (up to ~30 s when the console is unreachable), which would
  // trigger the TWDT if this task were registered.  The idle tasks always
  // feed their own TWDT slots because FreeRTOS yields to them while our task
  // blocks on the network — so not registering here is safe.
  if (!scale_mode_) {
    esp_task_wdt_add(NULL);  // NULL = current task
  }
  uint32_t last_stack_diag_ms = 0;
  for (;;) {
    uint32_t now = millis();

    // Feed the task watchdog every iteration. We subscribe this task to the TWDT
    // above but only do network work intermittently; without an unconditional
    // reset here, any stretch with no work (e.g. a long heartbeat/poll interval)
    // could let the 30 s TWDT expire. Resetting per loop ties watchdog health to
    // the loop running, not to how often we hit the backend — so interval/cadence
    // choices can't starve it. (esp_task_wdt_reset only valid for a subscribed
    // task, so guard on the same condition as the esp_task_wdt_add above.)
    if (!scale_mode_) esp_task_wdt_reset();

    // Core-1 health diagnostic (every 5 min): stack high-water mark of this
    // task plus minimum-ever free internal heap.  Creeping stack exhaustion
    // or heap pressure on core 1 manifests as wild-PC / int-WDT crashes with
    // useless idle-task backtraces — this log makes the trend visible first.
    if (now - last_stack_diag_ms >= 300000UL) {
      last_stack_diag_ms = now;
      ESP_LOGI(TAG, "%s task stack high-water: %u bytes free, min free internal heap: %u bytes",
               scale_mode_ ? "scale_push" : "bambuddy_http",
               (unsigned) (uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
               (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }

    // Wait for a valid IP before any network I/O.
    if (!is_network_ready()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // ---- Scale push mode (scale_mode_ && !console_url_.empty()) ----
    // Heartbeat is sent on a fixed timer; response carries pending commands.
    // Weight and NFC events are processed one-job-per-tick so a blocked HTTP
    // call never starves the loop.
    if (scale_mode_) {
      // Heartbeat — use a faster interval when an NFC tag is present so
      // write_tag commands are picked up quickly (~200 ms vs 1 s at idle).
      // The NFC task runs on the main core; reading nfc_state under the lock
      // is a brief, non-blocking check that is safe here.
      {
        lock_state();
        bool tag_present = (display_state_.nfc_state == NFCTagState::PRESENT);
        unlock_state();
        uint32_t hb_interval = tag_present ? SCALE_HEARTBEAT_FAST_MS
                                           : SCALE_HEARTBEAT_INTERVAL_MS;
        uint32_t now_hb = millis();
        if (now_hb - last_heartbeat_push_ms_ >= hb_interval) {
          last_heartbeat_push_ms_ = now_hb;
          api_scale_push_heartbeat();
        }
      }

      // Process one queued job (weight or NFC event) per tick.
      HttpJob job;
      if (dequeue_job(job)) {
        bool push_failed = false;
        switch (job.kind) {
          case HttpJob::SCALE_PUSH_WEIGHT:
            push_weight_pending_ = false;  // allow next on_scale_reading enqueue
            if (!api_scale_push_weight(job.f1, job.b1)) push_failed = true;
            break;
          case HttpJob::SCALE_PUSH_NFC_SCANNED:
            api_scale_push_nfc_scanned(job.s1, job.s2, job.i1, job.s3);
            break;
          case HttpJob::SCALE_PUSH_NFC_REMOVED:
            api_scale_push_nfc_removed(job.s1);
            break;
          case HttpJob::WRITE_RESULT:
            // NFC write completed on scale — relay result to the console.
            api_scale_push_write_result(job.i1, job.s1, job.b1, job.s2);
            break;
          default:
            break;
        }
        // Back off for 2 s on weight push failure to avoid hammering an
        // unreachable console at the DNS-retry rate (~3 s per attempt).
        vTaskDelay(pdMS_TO_TICKS(push_failed ? 2000 : 10));
      } else {
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      continue;  // skip all console/backend sections below
    }

    // ---- Maintenance: TTL expiry + post-assign clear timer ----
    // Runs every loop iteration (~50 ms idle) so the UI countdown stays accurate
    // and the NFC page clears promptly on TTL expiry or after assign display.
    {
      lock_state();
      if (pending_assign_spool_id_ > 0 && now >= pending_assign_expiry_ms_) {
        ESP_LOGI(TAG, "Auto-assign TTL expired for spool %d",
                 pending_assign_spool_id_);
        pending_assign_spool_id_ = 0;
        display_state_.spool_assign_expiry_ms = 0;
        display_state_.spool_selected = false;
        display_state_.current_filament = FilamentInfo{};
        display_state_.status_message = "Timed out — rescan to assign";
        // AMS-sourced loads have no physical tag, so nfc_state stays PRESENT
        // (set by activate_spool).  Clear it here so the unlinked panel doesn't
        // flash up after the TTL; real NFC tags are cleared by on_tag_removed().
        if (last_tag_source_ == TagSource::AMS) {
          display_state_.nfc_state = NFCTagState::ABSENT;
          display_state_.last_tag_uid.clear();
        }
      }
      if (assign_clear_ms_ > 0 && now >= assign_clear_ms_) {
        display_state_.assign_success = false;
        display_state_.assign_slot_desc.clear();
        display_state_.spool_selected = false;
        display_state_.current_filament = FilamentInfo{};
        assign_clear_ms_ = 0;
        display_state_.status_message = "Spool assigned";
        // Same cleanup as above: AMS loads must clear nfc_state since no
        // on_tag_removed() callback fires for a synthetic scan.
        if (last_tag_source_ == TagSource::AMS) {
          display_state_.nfc_state = NFCTagState::ABSENT;
          display_state_.last_tag_uid.clear();
        }
      }
      // Unlinked tag sticky TTL: keep nfc_state=PRESENT until the user acts or
      // the timer expires so the unlinked-panel buttons stay reachable.
      if (unlinked_tag_expiry_ms_ > 0 && now >= unlinked_tag_expiry_ms_) {
        unlinked_tag_expiry_ms_ = 0;
        display_state_.unlinked_tag_expiry_ms = 0;
        display_state_.nfc_state = NFCTagState::ABSENT;
        display_state_.last_tag_uid.clear();
        display_state_.current_filament = FilamentInfo{};
        ESP_LOGI(TAG, "Unlinked-tag TTL expired, clearing NFC state");
      }
      // Keep scale_ok in sync with the push timeout. scale_live() reads millis()
      // at call time — NOT the stale `now` from the loop top — so a concurrent
      // last_scale_push_ms_ update from the httpd task (core 0) cannot cause an
      // unsigned-subtraction wrap.
      display_state_.scale_ok = scale_live();
      unlock_state();
    }

    // ---- Registration (retry until success) ----
    if (!registered_) {
      if (now - last_register_ms_ >= 5000) {
        last_register_ms_ = now;
        bool ok = api_register_device();
        lock_state();
        if (ok) {
          registered_ = true;
          display_state_.backend_state = BackendState::REGISTERED;
          display_state_.status_message = "Connected to Bambuddy";
          last_printer_poll_ms_ = 0;  // fetch printers/AMS immediately
        } else {
          display_state_.backend_state = BackendState::ERROR;
          display_state_.status_message = "Registration failed, retrying...";
        }
        unlock_state();
        if (ok) ESP_LOGI(TAG, "Device registered successfully");
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // ---- Drain pending event jobs first (tag scans, scale, etc.) ----
    HttpJob job;
    bool did_work = false;
    while (dequeue_job(job)) {
      did_work = true;
      switch (job.kind) {
        case HttpJob::TAG_SCANNED:
          api_tag_scanned(job.s1, job.s2, job.i1, job.s3);
          // Follow immediately with a heartbeat so a pending write_tag command
          // is fetched while the tag is still on the reader.
          api_heartbeat();
          last_heartbeat_ms_ = millis();
          break;
        case HttpJob::TAG_REMOVED:
          api_tag_removed(job.s1);
          break;
        case HttpJob::SCALE_READING:
          api_scale_reading(job.f1, job.b1, job.i1);
          break;
        case HttpJob::WRITE_RESULT:
          api_write_tag_result(job.i1, job.s1, job.b1, job.s2);
          break;
        case HttpJob::UPDATE_TARE:
          api_update_tare(job.f1);
          break;
        case HttpJob::CLEAR_ASSIGNMENT: {
          int ams_id   = job.i1;
          int tray_slot = job.i2;
          int spool_id  = job.i3;
          std::string printer_id;
          lock_state();
          printer_id = display_state_.selected_printer_id;
          unlock_state();
          if (api_clear_assignment(printer_id, ams_id, tray_slot, spool_id))
            api_get_assignments(printer_id);
          break;
        }
        case HttpJob::ASSIGN_SPOOL: {
          int spool_id  = job.i1;
          int ams_id    = (job.i2 >> 8) & 0xFFFF;
          int tray_slot = job.i2 & 0xFF;
          std::string printer_id;
          lock_state();
          printer_id = display_state_.selected_printer_id;
          unlock_state();
          // Refresh assignments on success so the slot shows the spool at once.
          if (api_assign_spool(spool_id, printer_id, ams_id, tray_slot))
            api_get_assignments(printer_id);
          break;
        }
        case HttpJob::FETCH_RECENT_SPOOLS:
          api_get_recent_spools();
          break;
        case HttpJob::LINK_TAG_TO_SPOOL:
          api_link_tag(job.i1, job.s1, job.s2, job.s3);
          break;
        case HttpJob::CREATE_SPOOL_FROM_TAG:
          api_create_spool_from_tag(job.s1);
          break;
        case HttpJob::UPDATE_CALIBRATION:
          api_report_calibration_point(job.f1, job.f2);
          break;
        case HttpJob::UPDATE_SPOOL_WEIGHT:
          api_update_spool_weight(job.i1, job.f1);
          break;
        case HttpJob::ARCHIVE_SPOOL:
          api_archive_spool(job.i1);
          break;
      }
    }

    // ---- NFC wake-from-sleep retry ----
    // When a tag is scanned while WiFi is reconnecting, api_tag_scanned() fails
    // and sets pending_tag_retry_=true instead of clearing tag_resolving. Once
    // the backend registers we re-enqueue the job. Give up after 30 s or if the
    // tag UID changed (user pulled the tag).
    if (pending_tag_retry_) {
      uint32_t elapsed = now - retry_started_ms_;
      lock_state();
      bool backend_up   = (display_state_.backend_state == BackendState::REGISTERED);
      bool tag_present  = (display_state_.nfc_state == NFCTagState::PRESENT);
      std::string cur_uid = display_state_.last_tag_uid;
      unlock_state();
      bool same_tag = (cur_uid == retry_uid_);

      if (elapsed > 30000 || !tag_present || (!same_tag && !cur_uid.empty())) {
        // Timed out or tag gone / replaced — give up.
        pending_tag_retry_ = false;
        retry_uid_.clear();
        lock_state();
        display_state_.tag_resolving = false;
        unlock_state();
        ESP_LOGW(TAG, "tag-scanned retry: giving up (elapsed=%ums tag_present=%d same_tag=%d)",
                 elapsed, tag_present, same_tag);
      } else if (backend_up) {
        // Backend is up — re-enqueue with the saved params.
        pending_tag_retry_ = false;
        ESP_LOGI(TAG, "tag-scanned retry: backend up — re-enqueueing uid=%s", retry_uid_.c_str());
        HttpJob job;
        job.kind = HttpJob::TAG_SCANNED;
        job.s1   = retry_uid_;
        job.s2   = retry_tray_uuid_;
        job.s3   = retry_tag_type_;
        job.i1   = retry_sak_;
        enqueue_job(job);
        did_work = true;
      }
    }

    // ---- Scheduled heartbeat ----
    // Stretched by low_power_factor_ while asleep to cut backend traffic. The
    // task watchdog is fed every loop iteration (above), so the stretched
    // cadence cannot starve it.
    uint32_t hb_interval = low_power_ ? heartbeat_interval_ms_ * low_power_factor_
                                      : heartbeat_interval_ms_;
    if (now - last_heartbeat_ms_ >= hb_interval) {
      last_heartbeat_ms_ = now;
      lock_state();
      display_state_.uptime_s = (now - start_ms_) / 1000;
      display_state_.ip_address = get_ip_address();
      unlock_state();
      api_heartbeat();
      did_work = true;
    }

    // ---- Printer & AMS polling ----
    // Until the first successful printer fetch, retry quickly (every 5 s) so the
    // UI populates soon after a cold boot — when early connects time out while the
    // network path is still converging — instead of waiting a full poll interval.
    uint32_t poll_due = printers_fetched_ ? printer_poll_interval_ms_ : 5000;
    if (low_power_ && printers_fetched_) poll_due *= low_power_factor_;
    if (now - last_printer_poll_ms_ >= poll_due) {
      last_printer_poll_ms_ = now;
      last_ams_fast_poll_ms_ = now;  // full poll resets the fast-poll clock too
      api_get_printers();
      api_get_ams();
      {
        lock_state();
        std::string pid = display_state_.selected_printer_id;
        unlock_state();
        if (!pid.empty()) api_get_printer_status(pid);
      }
      did_work = true;
    }

    if (pending_assign_spool_id_ > 0 &&
               now - last_ams_fast_poll_ms_ >= 5000) {
      // While an auto-assign is pending, poll AMS every 5 s (instead of waiting
      // the full 30 s interval) so slot-load detection fires quickly within the
      // 60 s TTL window (~12 opportunities vs 2 at 30 s).
      last_ams_fast_poll_ms_ = now;
      api_get_ams();
      did_work = true;
    }

    // Idle delay — short so events are picked up quickly, long enough to
    // yield generously to other tasks when nothing is happening.
    vTaskDelay(pdMS_TO_TICKS(did_work ? 10 : 50));

#if CORE_DEBUG_LEVEL >= 4  // VERBOSE only
    // Log the stack high-water mark every ~30 s so we can verify headroom.
    {
      static uint32_t last_hwm_log_ms = 0;
      if (millis() - last_hwm_log_ms >= 30000) {
        last_hwm_log_ms = millis();
        ESP_LOGV(TAG, "HTTP task stack HWM: %u bytes free",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
      }
    }
#endif
  }
}

void BambuddyAPIComponent::enqueue_job(const HttpJob &job) {
  if (!job_mutex_) return;
  xSemaphoreTake(job_mutex_, portMAX_DELAY);
  // Cap the queue so a stalled backend can't grow it without bound.
  if (job_queue_.size() < 32) job_queue_.push_back(job);
  xSemaphoreGive(job_mutex_);
}

bool BambuddyAPIComponent::dequeue_job(HttpJob &job) {
  if (!job_mutex_) return false;
  bool got = false;
  xSemaphoreTake(job_mutex_, portMAX_DELAY);
  if (!job_queue_.empty()) {
    job = job_queue_.front();
    job_queue_.pop_front();
    got = true;
  }
  xSemaphoreGive(job_mutex_);
  return got;
}

DisplayState BambuddyAPIComponent::snapshot() {
  lock_state();
  DisplayState copy = display_state_;  // deep copy of vectors/strings
  unlock_state();
  // Keep uptime ticking smoothly between heartbeats.
  copy.uptime_s = (millis() - start_ms_) / 1000;
  return copy;
}

// ---------------------------------------------------------------------------
// NFC callbacks
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::on_tag_scanned(const std::string &uid,
                                           const std::string &tray_uuid,
                                           int sak,
                                           const std::string &tag_type) {
  ESP_LOGI(TAG, "Tag scanned: uid=%s tray_uuid=%s sak=0x%02X type=%s",
           uid.c_str(), tray_uuid.c_str(), sak, tag_type.c_str());

  // Immediate (non-blocking) UI feedback under the state lock.
  // tag_format is seeded here for BambuLab (Mifare Classic) where SAK is
  // sufficient; for NTAG the NFC component updates it via set_tag_format()
  // after reading NDEF pages (a few ms later, still before the HTTP response).
  std::string initial_format;
  if (tag_type == "mifare_classic") initial_format = "bambu_lab";
  lock_state();
  display_state_.nfc_state = NFCTagState::PRESENT;
  display_state_.last_tag_uid = uid;
  // Discard any pending assignment TTL from a previous scan so the maintenance
  // loop can't expire it mid-flight while this new tag's HTTP request runs.
  pending_assign_spool_id_              = 0;
  pending_assign_expiry_ms_             = 0;
  display_state_.spool_assign_expiry_ms = 0;
  display_state_.spool_selected         = false;
  display_state_.current_filament       = FilamentInfo{};
  display_state_.current_filament.tray_uuid  = tray_uuid;
  display_state_.current_filament.tag_type   = tag_type;
  display_state_.current_filament.tag_format = initial_format;
  display_state_.current_filament.sak        = sak;
  display_state_.status_message = "Tag detected: " + uid;
  display_state_.nfc_scan_generation++;  // lets the UI detect new physical scans
  display_state_.propose_archive = false;  // new scan cancels any pending archive proposal
  last_tag_source_ = TagSource::LOCAL;   // overridden to SCALE by console_http_scale_nfc_scanned
  unlock_state();

  // Wrong tag type for a pending write → report failure (queued, non-blocking).
  if (pending_write_active_.load() && !(sak == 0x00 || sak == 0x04)) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X", sak);
    std::string msg = std::string("Incompatible tag type (SAK=0x") + buf +
                      "). Place an NTAG tag to write.";
    on_write_tag_result(uid, false, msg);
  } else if (pending_write_active_.load()) {
    ESP_LOGI(TAG, "Pending write for spool %d — NFC component will execute",
             pending_write_spool_id_);
  }

  // Queue the HTTP work for the background task so this callback returns instantly.
  if (!scale_mode_) {
    // Console mode: POST tag-scanned to BamBuddy backend.
    // Set tag_resolving so the UI shows "Identifying tag..." instead of the
    // unlinked panel while the HTTP call is in flight.
    lock_state();
    display_state_.tag_resolving = true;
    unlock_state();
    // Save params for a possible retry (e.g. wake-from-sleep where WiFi is
    // still reconnecting when the first attempt runs).
    pending_tag_retry_ = false;
    retry_uid_        = uid;
    retry_tray_uuid_  = tray_uuid;
    retry_tag_type_   = tag_type;
    retry_sak_        = sak;
    HttpJob job;
    job.kind = HttpJob::TAG_SCANNED;
    job.s1 = uid;
    job.s2 = tray_uuid;
    job.s3 = tag_type;
    job.i1 = sak;
    enqueue_job(job);
  } else if (!console_url_.empty()) {
    // Scale push mode: forward tag event to the console.
    HttpJob job;
    job.kind = HttpJob::SCALE_PUSH_NFC_SCANNED;
    job.s1 = uid;
    job.s2 = tray_uuid;
    job.s3 = tag_type;
    job.i1 = sak;
    enqueue_job(job);
  }
}

void BambuddyAPIComponent::on_tag_removed(const std::string &uid) {
  ESP_LOGI(TAG, "Tag removed: uid=%s", uid.c_str());
  lock_state();
  display_state_.tag_resolving = false;  // cancel any in-flight resolution
  if (display_state_.spool_selected || scale_mode_) {
    // Linked spool, OR scale-server mode: clear physical presence immediately.
    // In scale_mode_ the console manages its own sticky-TTL once it
    // receives the push event; the scale must mirror physical reality.
    display_state_.nfc_state = NFCTagState::ABSENT;
  } else {
    // Unlinked tag on the console: start a TTL so the user can still tap
    // "Add to inventory" or "Assign spool" after lifting the tag.
    unlinked_tag_expiry_ms_ = millis() + ASSIGN_TTL_MS;
    display_state_.unlinked_tag_expiry_ms = unlinked_tag_expiry_ms_;
    // Keep nfc_state = PRESENT so the unlinked panel stays visible.
    ESP_LOGI(TAG, "Unlinked tag removed — keeping panel for %u ms", ASSIGN_TTL_MS);
  }
  unlock_state();

  if (!scale_mode_) {
    // Console mode: POST tag-removed to BamBuddy backend.
    HttpJob job;
    job.kind = HttpJob::TAG_REMOVED;
    job.s1 = uid;
    enqueue_job(job);
  } else if (!console_url_.empty()) {
    // Scale push mode: forward removal event to the console.
    HttpJob job;
    job.kind = HttpJob::SCALE_PUSH_NFC_REMOVED;
    job.s1 = uid;
    enqueue_job(job);
  }
}

// ---------------------------------------------------------------------------
// Scale callbacks
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::on_scale_reading(float grams, bool stable,
                                             int raw_adc) {
  // Fast UI update (no HTTP) under the state lock.
  lock_state();
  display_state_.weight_grams = grams;
  display_state_.weight_stable = stable;
  display_state_.scale_ok = true;
  unlock_state();

  uint32_t now = millis();
  if (now - last_scale_report_ms_ < scale_report_interval_ms_) return;
  last_scale_report_ms_ = now;

  if (!scale_mode_) {
    // Console mode: only report to the backend when weight changes significantly
    // to avoid spamming the backend with identical readings.
    bool weight_changed = !last_reported_valid_ ||
                          fabsf(grams - last_reported_weight_) >= REPORT_THRESHOLD;
    if (!weight_changed) return;
    last_reported_weight_ = grams;
    last_reported_valid_ = true;

    HttpJob job;
    job.kind = HttpJob::SCALE_READING;
    job.f1 = grams;
    job.b1 = stable;
    job.i1 = raw_adc;
    enqueue_job(job);
  } else if (!console_url_.empty()) {
    // Scale push mode: push at scale_report_interval rate so tare and
    // calibration changes reach the console promptly.  Connectivity itself is
    // maintained by the heartbeat — the weight push is a pure data channel.
    // Only enqueue if no weight job is already pending — prevents the queue
    // from accumulating back-to-back jobs during a DNS stall.
    bool was_pending = push_weight_pending_.exchange(true);
    if (!was_pending) {
      HttpJob job;
      job.kind = HttpJob::SCALE_PUSH_WEIGHT;
      job.f1 = grams;
      job.b1 = stable;
      enqueue_job(job);
    }
  }
}

// ---------------------------------------------------------------------------
// Sleep / low-power mode
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::set_low_power(bool enable) {
  if (enable == low_power_.load()) return;
  low_power_ = enable;
  if (enable) {
    // Drop WiFi to the most aggressive modem sleep (MAX_MODEM) while idle for
    // maximum battery saving — restored to the awake mode on wake. We stay
    // associated, so scale-pushed wakes still arrive (buffered to the next radio
    // wake). The HA API connection may drop during sleep and reconnect on wake —
    // harmless (api reboot_timeout is 0). Capture the awake mode first so wake
    // restores exactly what was configured.
    if (esp_wifi_get_ps(&awake_ps_mode_) != ESP_OK) awake_ps_mode_ = WIFI_PS_NONE;
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    // Also stretch the backend cadence by low_power_factor_ (http_task_loop) to
    // cut backend traffic while idle.
    ESP_LOGI(TAG, "Sleep: entering low-power mode (WiFi MAX_MODEM, heartbeat/poll x%u)",
             low_power_factor_);
  } else {
    // Restore the awake WiFi power-save mode, then force an immediate heartbeat +
    // printer/AMS poll on the next task tick so the UI is fresh at once.
    esp_wifi_set_ps(awake_ps_mode_);
    last_heartbeat_ms_ = 0;
    last_printer_poll_ms_ = 0;
    ESP_LOGI(TAG, "Sleep: leaving low-power mode (WiFi PS restored), forcing immediate refresh");
  }
}

// ---------------------------------------------------------------------------
// Power Management — CPU frequency scaling + automatic light sleep
// ---------------------------------------------------------------------------
#include "esp_pm.h"

void BambuddyAPIComponent::configure_pm(bool light_sleep) {
#ifdef CONFIG_PM_ENABLE
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz       = light_sleep ? 80 : 240;
  cfg.min_freq_mhz       = light_sleep ? 40 : 240;
  cfg.light_sleep_enable = light_sleep;
  esp_err_t err = esp_pm_configure(&cfg);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "PM: esp_pm_configure failed: %s", esp_err_to_name(err));
  else
    ESP_LOGI(TAG, "PM: %s (max=%d MHz, light_sleep=%s)",
             light_sleep ? "entering light-sleep profile" : "restoring full-speed profile",
             cfg.max_freq_mhz, light_sleep ? "on" : "off");
#else
  (void)light_sleep;
  ESP_LOGW(TAG, "PM: CONFIG_PM_ENABLE not set — skipping pm_configure");
#endif
}

// ---------------------------------------------------------------------------
// Write-tag result
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::on_write_tag_result(const std::string &uid,
                                                bool success,
                                                const std::string &msg) {
  ESP_LOGI(TAG, "Write tag result: uid=%s success=%d msg=%s",
           uid.c_str(), success, msg.c_str());
  if (pending_write_active_.load()) {
    int spool_id = pending_write_spool_id_;
    pending_write_active_.store(false);
    lock_state();
    display_state_.status_message = success ? "Tag written successfully"
                                            : "Tag write failed: " + msg;
    unlock_state();

    HttpJob job;
    job.kind = HttpJob::WRITE_RESULT;
    job.i1 = spool_id;
    job.s1 = uid;
    job.b1 = success;
    job.s2 = msg;
    enqueue_job(job);
  }
}

// ---------------------------------------------------------------------------
// Tare
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::request_tare() {
  if (scale_mode_) {
    local_tare();
    return;
  }
  // Push mode: scale is connected — deliver tare via next heartbeat response.
  if (last_scale_push_ms_ > 0) {
    lock_state();
    pending_scale_cmd_ = "tare";
    unlock_state();
    set_status("Tare sent to scale...");
    return;
  }
  // Local scale (no external scale device).
  float new_offset;
  lock_state();
  new_offset = display_state_.weight_grams;
  display_state_.status_message = "Tare applied";
  unlock_state();
  tare_offset_ = new_offset;

  HttpJob job;
  job.kind = HttpJob::UPDATE_TARE;
  job.f1 = new_offset;
  enqueue_job(job);
}

// ---------------------------------------------------------------------------
// API methods — mirror the SpoolBuddy Python daemon's endpoint structure
// ---------------------------------------------------------------------------

bool BambuddyAPIComponent::api_register_device() {
  std::string hostname = hostname_.empty() ? "SpoolBuddy-ESP" : hostname_;
  std::string ip = get_ip_address();

  std::ostringstream js;
  js << "{"
     << "\"device_id\":" << json_string(device_id_) << ","
     << "\"hostname\":" << json_string(hostname) << ","
     << "\"ip_address\":" << json_string(ip) << ","
     << "\"firmware_version\":" << json_string(FIRMWARE_VERSION) << ","
     // has_scale=true when in scale_mode (physical HX711) or when the push-mode
     // scale is live (pushed within SCALE_LIVE_TIMEOUT_MS).  Goes false when
     // the scale disconnects so the backend reflects reality.
     << "\"has_nfc\":true,"
     << "\"has_scale\":" << bool_str(scale_mode_ || scale_live()) << ","
     // tare_offset is an integer in the backend schema — see api_update_tare().
     << "\"tare_offset\":" << (int)lroundf(tare_offset_) << ","
     << "\"calibration_factor\":" << calibration_factor_ << ","
     << "\"nfc_reader_type\":\"PN532\","
     << "\"nfc_connection\":\"SPI\","
     << "\"backend_url\":" << json_string(backend_url_) << ","
     << "\"has_backlight\":true"
     << "}";

  std::string resp;
  if (!http_post("/devices/register", js.str(), resp)) return false;

  // Parse server-side calibration if provided
  float cal = parse_json_float(resp, "calibration_factor", calibration_factor_);
  float tare = parse_json_float(resp, "tare_offset", tare_offset_);
  if (cal != calibration_factor_ || tare != tare_offset_) {
    calibration_factor_ = cal;
    tare_offset_ = tare;
    lock_state();
    display_state_.calibration_factor = cal;
    unlock_state();
    ESP_LOGI(TAG, "Calibration from backend: tare=%.2f factor=%.6f", tare, cal);
  }

  // SSH key deployment — not supported on ESPHome, log and ignore
  std::string ssh_key = parse_json_string(resp, "ssh_public_key");
  if (!ssh_key.empty()) {
    ESP_LOGI(TAG,
             "SSH key deployment not supported on ESPHome (mocked response). "
             "Key ignored.");
    // Return a mocked "OK" to the backend via system_command_result
    api_system_command_result("deploy_ssh_key", true,
                              "SSH not supported on ESPHome device");
  }
  return true;
}

void BambuddyAPIComponent::api_heartbeat() {
  // Read the few state fields needed for the request body under a brief lock.
  lock_state();
  int uptime = display_state_.uptime_s;
  std::string ip = display_state_.ip_address;
  // scale_ok: true when the scale sensor is live (scale device) or when a push
  // was received within the liveness window (console).
  bool scale_ok = scale_mode_ ? display_state_.scale_ok : scale_live();
  unlock_state();

  // Note: the backend's HeartbeatRequest schema has no tare/calibration
  // fields — tare sync goes through POST /calibration/set-tare instead
  // (api_update_tare), which also advances last_calibrated_at.
  std::ostringstream js;
  js << "{"
     << "\"nfc_ok\":true,"
     << "\"scale_ok\":" << bool_str(scale_ok) << ","
     << "\"uptime_s\":" << uptime << ","
     << "\"ip_address\":" << json_string(ip) << ","
     << "\"firmware_version\":" << json_string(FIRMWARE_VERSION) << ","
     << "\"nfc_reader_type\":\"PN532\","
     << "\"nfc_connection\":\"SPI\","
     << "\"backend_url\":" << json_string(backend_url_)
     << "}";

  std::string resp;
  bool ok = http_post("/devices/" + device_id_ + "/heartbeat", js.str(), resp);
  if (!ok) {
    lock_state();
    display_state_.backend_state = BackendState::ERROR;
    display_state_.status_message = "Lost connection to Bambuddy";
    unlock_state();
    return;
  }

  lock_state();
  display_state_.backend_state = BackendState::REGISTERED;
  display_state_.nfc_ok = true;
  // Do NOT force scale_ok here — on a scale device it is set true by
  // on_scale_reading(); on the console it is kept in sync with scale push
  // liveness by the maintenance block in http_task_loop().
  unlock_state();

  // Handle SSH key in heartbeat response (same mock as registration)
  std::string ssh_key = parse_json_string(resp, "ssh_public_key");
  if (!ssh_key.empty()) {
    ESP_LOGD(TAG, "SSH key in heartbeat ignored (not supported on ESPHome)");
    api_system_command_result("deploy_ssh_key", true,
                              "SSH not supported on ESPHome device");
  }

  // Process pending_command
  std::string cmd = parse_json_string(resp, "pending_command");
  if (!cmd.empty()) {
    ESP_LOGI(TAG, "Heartbeat received command: '%s'", cmd.c_str());
    handle_command(cmd, resp, resp);
  } else {
    ESP_LOGD(TAG, "Heartbeat OK — no pending command");
  }

  // Parse printer list if the heartbeat response includes one.
  {
    std::vector<PrinterInfo> hb_printers;
    parse_printer_list(resp, hb_printers);
    if (!hb_printers.empty()) {
      lock_state();
      display_state_.printers = hb_printers;
      if (display_state_.selected_printer_idx >=
          (int)display_state_.printers.size())
        display_state_.selected_printer_idx = 0;
      display_state_.selected_printer_id =
          display_state_.printers[display_state_.selected_printer_idx].id;
      unlock_state();
      ESP_LOGD(TAG, "Heartbeat contained %d printer(s)",
               (int)hb_printers.size());
    }
  }

  // ---- Calibration sync: backend → console → scale ----
  // Skip when this same response delivered a tare/calibrate command (its
  // tare/calibration fields predate the command's effect — the Python daemon
  // does the same), and while a tare ack is still in flight, so stale backend
  // values are never forwarded back to the scale and revert a fresh tare.
  lock_state();
  bool tare_in_flight = tare_ack_pending_;
  unlock_state();
  if (cmd != "tare" && cmd != "calibrate_with_weight" && !tare_in_flight) {
    float cal  = parse_json_float(resp, "calibration_factor", calibration_factor_);
    float tare = parse_json_float(resp, "tare_offset",        tare_offset_);
    // Thresholds absorb the backend's integer tare rounding and the
    // set-factor quantisation, so values settled via the scale-report path
    // don't ping-pong between the scale and backend copies.
    bool tare_changed = fabsf(tare - tare_offset_) > 0.5f;
    bool cal_changed  = cal > 0.0f &&
                        fabsf(cal - calibration_factor_) >
                            calibration_factor_ * 1e-3f;
    if (tare_changed || cal_changed) {
      if (cal > 0.0f) calibration_factor_ = cal;
      tare_offset_ = tare;
      lock_state();
      display_state_.calibration_factor = calibration_factor_;
      unlock_state();
      ESP_LOGI(TAG, "Calibration from backend: tare=%.2f factor=%.6f",
               tare_offset_, calibration_factor_);
      // The push-mode scale applies tare/factor itself — forward the new
      // values via its next heartbeat response so they take effect on the
      // actual readings and persist in the scale's NVS.
      if (scale_live()) {
        char pl[64];
        snprintf(pl, sizeof(pl), "\"tare\":%.2f,\"factor\":%.6f",
                 tare_offset_, calibration_factor_);
        lock_state();
        pending_scale_cmd_         = "set_calibration";
        pending_scale_cmd_payload_ = pl;
        unlock_state();
        ESP_LOGI(TAG, "Forwarding calibration to scale via heartbeat response");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// activate_spool — unified "spool ready for quick weight" state transition.
// Sets all DisplayState fields that the NFC and AMS-removal paths share:
//   spool_selected, nfc_state=PRESENT, TTL timer, assign state, status message.
// Does NOT increment nfc_scan_generation — callers own that so the timing
// is correct (NFC: on_tag_scanned fires immediately; AMS: after detection).
// ---------------------------------------------------------------------------
void BambuddyAPIComponent::activate_spool(FilamentInfo fi,
                                           const std::string &source_uid,
                                           TagSource source) {
  int spool_id = fi.spool_id;
  uint32_t expiry = spool_id > 0 ? millis() + ASSIGN_TTL_MS : 0;
  last_tag_source_ = source;
  lock_state();
  display_state_.current_filament       = fi;
  display_state_.spool_selected         = true;
  display_state_.tag_resolving          = false;
  display_state_.nfc_state              = NFCTagState::PRESENT;
  display_state_.last_tag_uid           = source_uid;
  display_state_.propose_archive        = false;
  display_state_.assign_success         = false;
  display_state_.assign_slot_desc.clear();
  display_state_.status_message = !fi.spool_name.empty() ? "Spool: " + fi.spool_name
                                : !fi.material_type.empty() ? "Spool: " + fi.material_type
                                : "Spool ready";
  pending_assign_spool_id_              = spool_id;
  pending_assign_expiry_ms_             = expiry;
  display_state_.spool_assign_expiry_ms = expiry;
  assign_clear_ms_ = 0;
  if (spool_id > 0)
    ESP_LOGI(TAG, "activate_spool: spool %d uid='%s' src=%d TTL=%us",
             spool_id, source_uid.c_str(), (int)source, ASSIGN_TTL_MS / 1000);
  unlock_state();
  if (spool_id > 0)
    api_get_spool(spool_id);
}

bool BambuddyAPIComponent::api_tag_scanned(const std::string &uid,
                                            const std::string &tray_uuid,
                                            int sak,
                                            const std::string &tag_type) {
  ESP_LOGI(TAG, "api_tag_scanned: POST uid=%s tray_uuid=%s sak=0x%02X type=%s",
           uid.c_str(), tray_uuid.c_str(), sak, tag_type.c_str());
  std::ostringstream js;
  js << "{"
     << "\"device_id\":" << json_string(device_id_) << ","
     << "\"tag_uid\":" << json_string(uid) << ","
     << "\"tray_uuid\":" << json_string(tray_uuid) << ","
     << "\"sak\":" << sak << ","
     << "\"tag_type\":" << json_string(tag_type)
     << "}";
  std::string resp;
  bool ok = http_post("/nfc/tag-scanned", js.str(), resp);

  // Parse filament info from response (into locals, then publish under lock)
  if (ok) {
    FilamentInfo fi;
    fi.tray_uuid     = tray_uuid;
    fi.tag_type      = tag_type;
    fi.sak           = sak;
    fi.material_type = parse_json_string(resp, "material_type");
    fi.color_hex     = parse_json_string(resp, "color_hex");
    fi.spool_name    = parse_json_string(resp, "spool_name");
    fi.spool_id      = parse_json_int(resp, "spool_id", 0);
    fi.min_temp      = parse_json_float(resp, "min_temp", 0.0f);
    fi.max_temp      = parse_json_float(resp, "max_temp", 0.0f);
    // A tag "belongs to a spool" when the backend resolves material or a
    // spool_id.  Such spools become the sticky selection shown on the NFC page.
    bool is_spool = !fi.material_type.empty() || fi.spool_id > 0;
    // Preserve tag_format: set by on_tag_scanned() and refined by the NFC
    // component's NDEF read (set_tag_format()), both of which happen before or
    // concurrently with this HTTP response.  Reading it here under the lock
    // gives us the most up-to-date value.
    lock_state();
    fi.tag_format = display_state_.current_filament.tag_format;
    std::string scan_uid = display_state_.last_tag_uid;  // set by on_tag_scanned()
    unlock_state();

    ESP_LOGI(TAG, "Tag-scanned response: spool_id=%d material='%s' "
             "color=#%s name='%s' temp=%.0f-%.0f",
             fi.spool_id,
             fi.material_type.empty() ? "-" : fi.material_type.c_str(),
             fi.color_hex.empty()     ? "??????" : fi.color_hex.c_str(),
             fi.spool_name.empty()    ? "-" : fi.spool_name.c_str(),
             fi.min_temp, fi.max_temp);

    if (is_spool) {
      // activate_spool() sets all quick-weight state and calls api_get_spool().
      // last_tag_source_ is already LOCAL or SCALE from on_tag_scanned().
      activate_spool(fi, scan_uid, last_tag_source_);
    } else {
      // Unlinked tag: clear resolving flag and store minimal tag info for the
      // unlinked panel, but do not arm the auto-assign TTL.
      lock_state();
      display_state_.tag_resolving    = false;
      display_state_.current_filament = fi;
      unlock_state();
    }
  } else {
    ESP_LOGW(TAG, "tag-scanned POST failed");
    lock_state();
    bool backend_up = (display_state_.backend_state == BackendState::REGISTERED);
    unlock_state();
    if (!backend_up && !retry_uid_.empty()) {
      // Backend not reachable yet (e.g. WiFi just reconnecting after sleep).
      // Keep tag_resolving=true so the UI stays on "Identifying..." and schedule
      // a retry for when the backend registers.
      pending_tag_retry_    = true;
      retry_started_ms_     = millis();
      ESP_LOGI(TAG, "tag-scanned: backend not up — will retry (uid=%s)", retry_uid_.c_str());
    } else {
      lock_state();
      display_state_.tag_resolving = false;  // backend up but call failed — show unlinked panel
      unlock_state();
      pending_tag_retry_ = false;
    }
  }
  return ok;
}

void BambuddyAPIComponent::api_get_spool(int spool_id, bool check_empty) {
  std::string resp;

  if (spoolman_inventory_) {
    // Spoolman has no per-id GET, so stream the spool list (same brace-matcher
    // as api_get_recent_spools) and stop as soon as the matching id is found.
    bool found = false;
    std::string url = backend_url_for("/api/v1", "/spoolman/inventory/spools");
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.transport_type = HTTP_TRANSPORT_UNKNOWN;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
      ESP_LOGW(TAG, "api_get_spool (spoolman): client init failed");
      return;
    }
    if (!api_key_.empty())
      esp_http_client_set_header(client, "X-API-Key", api_key_.c_str());
    arch_feed_wdt();
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "api_get_spool (spoolman): open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
      ESP_LOGW(TAG, "api_get_spool (spoolman): HTTP %d", status);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return;
    }

    std::string obj;
    int  depth = 0;
    bool in_str = false, esc = false, capturing = false;
    char buf[512];
    int r;
    while (!found && (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
      arch_feed_wdt();
      for (int i = 0; i < r && !found; i++) {
        char c = buf[i];
        if (in_str) {
          if (capturing) obj.push_back(c);
          if (esc)            esc = false;
          else if (c == '\\') esc = true;
          else if (c == '"')  in_str = false;
          continue;
        }
        if (c == '"') { in_str = true; if (capturing) obj.push_back(c); continue; }
        if (c == '{') {
          if (depth == 0) { capturing = true; obj.clear(); }
          depth++; obj.push_back(c); continue;
        }
        if (c == '}') {
          if (depth > 0) depth--;
          obj.push_back(c);
          if (depth == 0 && capturing) {
            capturing = false;
            if (parse_json_int(obj, "id", 0) == spool_id) { resp = obj; found = true; }
            obj.clear();
          }
          continue;
        }
        if (capturing) obj.push_back(c);
      }
      if (capturing && obj.size() > 8192) {
        capturing = false; obj.clear(); depth = 0; in_str = false; esc = false;
      }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!found) {
      ESP_LOGW(TAG, "api_get_spool (spoolman): spool %d not found in list", spool_id);
      return;
    }
  } else if (!http_get_api("/inventory/spools/" + std::to_string(spool_id), resp)) {
    ESP_LOGW(TAG, "GET /inventory/spools/%d failed", spool_id);
    return;
  }

  std::string material   = parse_json_string(resp, "material");
  std::string subtype    = parse_json_string(resp, "subtype");
  std::string color_name = parse_json_string(resp, "color_name");
  std::string rgba       = parse_json_string(resp, "rgba");
  std::string brand      = parse_json_string(resp, "brand");
  std::string slicer     = parse_json_string(resp, "slicer_filament_name");
  float label_w   = parse_json_float(resp, "label_weight", 0.0f);
  float used_w    = parse_json_float(resp, "weight_used", 0.0f);
  float core_w    = parse_json_float(resp, "core_weight", 0.0f);
  float tmin      = parse_json_float(resp, "nozzle_temp_min", 0.0f);
  float tmax      = parse_json_float(resp, "nozzle_temp_max", 0.0f);

  lock_state();
  // Only merge into the still-selected spool (guard against a race where the
  // user dismissed or scanned another tag while this GET was in flight).
  if (display_state_.spool_selected &&
      display_state_.current_filament.spool_id == spool_id) {
    auto &fi = display_state_.current_filament;
    if (!material.empty())   fi.material_type = material;
    fi.subtype    = subtype;
    fi.color_name = color_name;
    fi.brand      = brand;
    if (rgba.size() >= 6)    fi.color_hex = rgba.substr(0, 6);
    if (label_w > 0)         fi.label_weight_g = label_w;
    fi.weight_used_g = used_w;
    if (core_w >= 0)         fi.core_weight_g  = core_w;
    if (tmin > 0)            fi.min_temp = tmin;
    if (tmax > 0)            fi.max_temp = tmax;
    if (fi.spool_name.empty() && !slicer.empty()) fi.spool_name = slicer;
    if (check_empty && label_w > 0 && used_w >= label_w) {
      display_state_.propose_archive = true;
    }
  }
  unlock_state();

  ESP_LOGI(TAG, "Spool %d: %s %s '%s' brand=%s rgba=%s %.0f/%.0f g",
           spool_id, material.c_str(), subtype.c_str(), color_name.c_str(),
           brand.c_str(), rgba.c_str(), used_w, label_w);
}

bool BambuddyAPIComponent::api_tag_removed(const std::string &uid) {
  std::ostringstream js;
  js << "{"
     << "\"device_id\":" << json_string(device_id_) << ","
     << "\"tag_uid\":" << json_string(uid)
     << "}";
  std::string resp;
  return http_post("/nfc/tag-removed", js.str(), resp);
}

bool BambuddyAPIComponent::api_scale_reading(float grams, bool stable,
                                              int raw_adc) {
  std::ostringstream js;
  js << std::fixed << std::setprecision(1);
  js << "{"
     << "\"device_id\":" << json_string(device_id_) << ","
     << "\"weight_grams\":" << grams << ","
     << "\"stable\":" << bool_str(stable) << ","
     << "\"raw_adc\":" << raw_adc
     << "}";
  std::string resp;
  return http_post("/scale/reading", js.str(), resp);
}

bool BambuddyAPIComponent::api_update_tare(float tare_offset) {
  // The backend's SetTareRequest declares tare_offset as an INTEGER (the
  // Python daemon reports a raw ADC count) — a fractional float fails
  // Pydantic validation with a 422, so round to the nearest integer.
  // This POST also advances last_calibrated_at on the backend, which the
  // Bambuddy UI polls to confirm a tare command completed (15 s timeout).
  std::ostringstream js;
  js << "{\"tare_offset\":" << (int)lroundf(tare_offset) << "}";
  std::string resp;
  return http_post("/devices/" + device_id_ + "/calibration/set-tare", js.str(), resp);
}

bool BambuddyAPIComponent::api_report_calibration_point(float reference_weight_g,
                                                         float measured_weight_g) {
  // The backend's only calibration endpoint is POST /calibration/set-factor,
  // which computes factor = known_weight_grams / (raw_adc - tare) from
  // integer raw readings.  measured_weight_g is our tare-adjusted net raw
  // reading, so raw_adc = tare + measured reproduces the right delta.
  std::ostringstream js;
  js << std::fixed << std::setprecision(2);
  js << "{"
     << "\"known_weight_grams\":" << reference_weight_g << ","
     << "\"raw_adc\":" << (int)lroundf(tare_offset_ + measured_weight_g) << ","
     << "\"tare_raw_adc\":" << (int)lroundf(tare_offset_)
     << "}";
  std::string resp;
  bool ok = http_post("/devices/" + device_id_ + "/calibration/set-factor",
                      js.str(), resp);
  if (!ok) {
    set_status("Calibration update failed");
    ESP_LOGW(TAG, "api_report_calibration_point: POST failed");
    return false;
  }

  // Parse the updated calibration params the backend computed and echo back.
  float new_factor = parse_json_float(resp, "calibration_factor", calibration_factor_);
  float new_tare   = parse_json_float(resp, "tare_offset",        tare_offset_);
  calibration_factor_ = new_factor;
  tare_offset_        = new_tare;
  lock_state();
  display_state_.calibration_factor = new_factor;
  unlock_state();
  char msg[64];
  snprintf(msg, sizeof(msg), "Calibration applied (factor %.4f)", new_factor);
  set_status(msg);
  ESP_LOGI(TAG, "Calibration applied: ref=%.2f g measured=%.2f g → factor=%.6f tare=%.2f",
           reference_weight_g, measured_weight_g, new_factor, new_tare);
  return true;
}

void BambuddyAPIComponent::request_calibration(float reference_weight_g) {
  if (reference_weight_g <= 0.0f) {
    ESP_LOGW(TAG, "request_calibration: reference_weight_g must be > 0");
    return;
  }

  // Push mode: scale is connected — deliver calibrate cmd via next heartbeat response.
  if (last_scale_push_ms_ > 0) {
    lock_state();
    pending_scale_cmd_       = "calibrate";
    pending_scale_cmd_value_ = reference_weight_g;
    unlock_state();
    set_status("Calibrate sent to scale...");
    ESP_LOGI(TAG, "request_calibration (push): ref=%.1f g queued for scale", reference_weight_g);
    return;
  }

  // Local scale device path: capture the live reading and report reference + measured.
  float measured;
  lock_state();
  measured = display_state_.weight_grams;
  unlock_state();
  // Use the tare-adjusted net reading — the backend expects the measured
  // weight of the reference object alone, not the gross ADC value.
  float measured_net = measured - tare_offset_;
  set_status("Calibrating...");
  ESP_LOGI(TAG, "request_calibration (local): ref=%.1f g gross=%.1f g tare=%.2f net=%.1f g",
           reference_weight_g, measured, tare_offset_, measured_net);
  HttpJob job;
  job.kind = HttpJob::UPDATE_CALIBRATION;
  job.f1 = reference_weight_g;
  job.f2 = measured_net;
  enqueue_job(job);
}

bool BambuddyAPIComponent::api_write_tag_result(int spool_id,
                                                 const std::string &uid,
                                                 bool success,
                                                 const std::string &message) {
  std::ostringstream js;
  js << "{"
     << "\"device_id\":" << json_string(device_id_) << ","
     << "\"spool_id\":" << spool_id << ","
     << "\"tag_uid\":" << json_string(uid) << ","
     << "\"success\":" << bool_str(success) << ","
     << "\"message\":" << json_string(message)
     << "}";
  std::string resp;
  return http_post("/nfc/write-result", js.str(), resp);
}

bool BambuddyAPIComponent::api_diagnostic_result(
    const std::string &diagnostic, bool success,
    const std::string &output, int exit_code) {
  std::ostringstream js;
  js << "{"
     << "\"diagnostic\":" << json_string(diagnostic) << ","
     << "\"success\":" << bool_str(success) << ","
     << "\"output\":" << json_string(output) << ","
     << "\"exit_code\":" << exit_code
     << "}";
  std::string resp;
  return http_post("/diagnostics/" + device_id_ + "/result", js.str(), resp);
}

bool BambuddyAPIComponent::api_system_command_result(
    const std::string &command, bool success, const std::string &message) {
  std::ostringstream js;
  js << "{"
     << "\"command\":" << json_string(command) << ","
     << "\"success\":" << bool_str(success) << ","
     << "\"message\":" << json_string(message)
     << "}";
  std::string resp;
  return http_post("/devices/" + device_id_ + "/system/command-result",
                   js.str(), resp);
}

// ---------------------------------------------------------------------------
// Command handler (mirrors the Python daemon's heartbeat command handling)
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::handle_command(const std::string &cmd,
                                           const std::string &payload_json,
                                           const std::string & /*response_json*/) {
  ESP_LOGI(TAG, "Received command: %s", cmd.c_str());

  if (cmd == "tare") {
    // Same routing as the UI tare button: push-mode scale gets the command via
    // the next heartbeat response, otherwise capture locally + notify backend.
    request_tare();

  } else if (cmd == "calibrate_with_weight") {
    float ref_g = parse_json_float(payload_json, "reference_weight_grams", 0.0f);
    // Same routing as the UI calibrate button; rejects ref_g <= 0 with a log.
    request_calibration(ref_g);

  } else if (cmd == "write_tag") {
    std::string ndef_hex = parse_json_string(payload_json, "ndef_data_hex");
    int spool_id = parse_json_int(payload_json, "spool_id", 0);
    if (!ndef_hex.empty()) {
      if (last_tag_source_ == TagSource::SCALE) {
        // Tag is on the scale's PN532 — relay the write command via the push
        // response channel so the scale can perform the write locally.
        char spool_buf[32];
        snprintf(spool_buf, sizeof(spool_buf), "\"spool_id\":%d", spool_id);
        lock_state();
        pending_scale_cmd_         = "write_tag";
        pending_scale_cmd_payload_ = std::string(spool_buf) +
                                     ",\"ndef_hex\":\"" + ndef_hex + "\"";
        unlock_state();
        set_status("Write queued for scale tag");
        ESP_LOGI(TAG, "write_tag: routing to scale PN532 (spool %d, %zu ndef bytes)",
                 spool_id, ndef_hex.size() / 2);
      } else {
        // Tag is on the console's PN532 — arm local write as before.
        pending_write_ndef_      = hex_to_bytes(ndef_hex);
        pending_write_spool_id_  = spool_id;
        pending_write_active_.store(true);
        set_status("Waiting for NTAG to write spool " + std::to_string(spool_id));
        ESP_LOGI(TAG, "write_tag: spool_id=%d ndef_len=%zu — waiting for NTAG",
                 spool_id, pending_write_ndef_.size());
      }
    }

  } else if (cmd == "run_nfc_diag") {
    // ESPHome cannot run external scripts; return a mock diagnostic result
    std::string output =
        "ESPHome SpoolBuddy NFC Diagnostic\n"
        "NFC Reader: PN532 via SPI\n"
        "Status: OK (hardware-level diagnostics not available on ESPHome)\n"
        "Note: PN532 SPI diagnostics are performed at startup.\n";
    api_diagnostic_result("nfc", true, output, 0);
    set_status("NFC diagnostic complete (mocked)");

  } else if (cmd == "run_scale_diag") {
    lock_state();
    float w = display_state_.weight_grams;
    bool st = display_state_.weight_stable;
    unlock_state();
    std::string output =
        "ESPHome SpoolBuddy Scale Diagnostic\n"
        "Scale: HX711 via GPIO\n"
        "Status: OK (hardware-level diagnostics not available on ESPHome)\n"
        "Current weight: " + std::to_string(w) + " g\n"
        "Stable: " + (st ? "yes" : "no") + "\n";
    api_diagnostic_result("scale", true, output, 0);
    set_status("Scale diagnostic complete (mocked)");

  } else if (cmd == "run_read_tag_diag") {
    lock_state();
    std::string uid = display_state_.last_tag_uid;
    std::string tt = display_state_.current_filament.tag_type;
    unlock_state();
    std::string output =
        "ESPHome SpoolBuddy Read-Tag Diagnostic\n"
        "Last tag UID: " + uid + "\n"
        "Tag type: " + tt + "\n"
        "Status: OK (hardware-level diagnostics not available on ESPHome)\n";
    api_diagnostic_result("read_tag", true, output, 0);
    set_status("Read-tag diagnostic complete (mocked)");

  } else if (cmd == "apply_system_config") {
    // Parse backend_url and api_key from payload
    std::string new_url = parse_json_string(payload_json, "backend_url");
    std::string new_key = parse_json_string(payload_json, "api_key");
    if (!new_url.empty()) {
      ESP_LOGI(TAG, "Applying new backend_url: %s", new_url.c_str());
      if (http_client_) {
        esp_http_client_cleanup(http_client_);
        http_client_ = nullptr;
      }
      backend_url_ = new_url;
      if (!new_key.empty()) api_key_ = new_key;
      // Note: changes are in-memory only; persisting requires NVS writes
      api_system_command_result(
          "apply_system_config", true,
          "Config updated in-memory (reboot to persist)");
    } else {
      api_system_command_result("apply_system_config", false,
                                "Missing backend_url payload");
    }

  } else if (cmd == "reboot") {
    api_system_command_result("reboot", true, "Rebooting ESPHome device");
    delay(500);
    esp_restart();

  } else if (cmd == "shutdown") {
    // Cannot shut down ESP32; reboot as best-effort substitute
    api_system_command_result("shutdown", true,
                              "Shutdown not supported on ESPHome; rebooting");
    delay(500);
    esp_restart();

  } else if (cmd == "restart_daemon") {
    api_system_command_result("restart_daemon", true,
                              "Restarting ESPHome firmware");
    delay(500);
    esp_restart();

  } else if (cmd == "restart_browser") {
    // No browser on ESPHome; no-op
    api_system_command_result("restart_browser", true,
                              "No browser process on ESPHome (no-op)");

  } else {
    ESP_LOGW(TAG, "Unknown command: %s", cmd.c_str());
  }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

static const char *method_name(esp_http_client_method_t m) {
  switch (m) {
    case HTTP_METHOD_GET:    return "GET";
    case HTTP_METHOD_POST:   return "POST";
    case HTTP_METHOD_PATCH:  return "PATCH";
    case HTTP_METHOD_DELETE: return "DELETE";
    default:                 return "HTTP";
  }
}

// Shared HTTP transaction core for every helper below.  Performs `method` on
// `url` with an optional JSON body, fills response_body, and returns true on
// a 2xx status.
//   timeout_ms:   5 s for backend calls — a connect/read budget large enough
//                 that cold-boot connects (network path still converging after
//                 reboot) complete instead of being cut off.  HTTP runs on a
//                 core-1 task so a long block never stalls LVGL/touch, and the
//                 WDT is fed per-call (30 s budget).  Scale→console pushes use
//                 a shorter 3 s budget.
//   with_api_key: add the X-API-Key header (backend requests only).
//   quiet:        log failures at DEBUG instead of WARN — for the routine
//                 scale→console pushes that fail whenever the console is off.
bool BambuddyAPIComponent::http_request(esp_http_client_method_t method,
                                         const std::string &url,
                                         const std::string &json_body,
                                         std::string &response_body,
                                         int timeout_ms, bool with_api_key,
                                         bool quiet) {
  const char *m = method_name(method);
  HttpContext ctx;

  // All requests rooted at backend_url_ share a persistent TLS connection so
  // the full mbedTLS cert-chain handshake happens once on first connect (or
  // after a dropped connection) rather than on every call.  Direct scale-push
  // calls (http_post_direct) go to a different host and keep per-call cleanup.
  bool is_backend = !backend_url_.empty() && url.rfind(backend_url_, 0) == 0;
  esp_http_client_handle_t client;

  if (is_backend) {
    if (!http_client_) {
      esp_http_client_config_t cfg = {};
      cfg.url                   = url.c_str();
      cfg.event_handler         = http_event_handler;
      cfg.user_data             = nullptr;   // set per-call via set_user_data below
      cfg.timeout_ms            = timeout_ms;
      cfg.disable_auto_redirect = false;
      cfg.crt_bundle_attach     = esp_crt_bundle_attach;
      cfg.transport_type        = HTTP_TRANSPORT_UNKNOWN;
      cfg.keep_alive_enable     = true;
      http_client_ = esp_http_client_init(&cfg);
      if (!http_client_) {
        ESP_LOGE(TAG, "HTTP client init failed for %s %s", m, url.c_str());
        return false;
      }
      ESP_LOGD(TAG, "Created persistent backend HTTP client");
    }
    client = http_client_;
    esp_http_client_set_url(client, url.c_str());
    esp_http_client_set_method(client, method);
    esp_http_client_set_user_data(client, &ctx);
    if (!json_body.empty()) {
      esp_http_client_set_header(client, "Content-Type", "application/json");
      esp_http_client_set_post_field(client, json_body.c_str(), (int)json_body.size());
    } else {
      esp_http_client_delete_header(client, "Content-Type");
      esp_http_client_set_post_field(client, nullptr, 0);
    }
    if (with_api_key && !api_key_.empty())
      esp_http_client_set_header(client, "X-API-Key", api_key_.c_str());
    else
      esp_http_client_delete_header(client, "X-API-Key");
  } else {
    esp_http_client_config_t cfg = {};
    cfg.url                   = url.c_str();
    cfg.method                = method;
    cfg.event_handler         = http_event_handler;
    cfg.user_data             = &ctx;
    cfg.timeout_ms            = timeout_ms;
    cfg.disable_auto_redirect = false;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.transport_type        = HTTP_TRANSPORT_UNKNOWN;
    client = esp_http_client_init(&cfg);
    if (!client) {
      ESP_LOGE(TAG, "HTTP client init failed for %s %s", m, url.c_str());
      return false;
    }
    if (!json_body.empty()) {
      esp_http_client_set_header(client, "Content-Type", "application/json");
      esp_http_client_set_post_field(client, json_body.c_str(), (int)json_body.size());
    }
    if (with_api_key && !api_key_.empty())
      esp_http_client_set_header(client, "X-API-Key", api_key_.c_str());
  }

  ESP_LOGD(TAG, "--> %s %s", m, url.c_str());
  if (!json_body.empty()) ESP_LOGD(TAG, "    body: %s", json_body.c_str());

  // Feed the task WDT immediately before the blocking HTTP call.
  arch_feed_wdt();
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  if (!is_backend) {
    esp_http_client_cleanup(client);
  } else if (err != ESP_OK) {
    // Connection broke — destroy so the next call reconnects fresh.
    esp_http_client_cleanup(http_client_);
    http_client_ = nullptr;
  }

  if (err != ESP_OK) {
    if (quiet)
      ESP_LOGD(TAG, "<-- %s %s failed: %s", m, url.c_str(), esp_err_to_name(err));
    else
      ESP_LOGW(TAG, "<-- %s %s failed: %s", m, url.c_str(), esp_err_to_name(err));
    return false;
  }
  ESP_LOGD(TAG, "<-- %d %s %s", status, m, url.c_str());
  ESP_LOGD(TAG, "    resp: %s", ctx.body.c_str());
  if (status < 200 || status >= 300) {
    if (quiet)
      ESP_LOGD(TAG, "%s %s returned %d", m, url.c_str(), status);
    else
      ESP_LOGW(TAG, "HTTP %s %s returned %d", m, url.c_str(), status);
    return false;
  }
  // Move (not copy) the accumulated body out: a large inventory response would
  // otherwise need a second full-size allocation here, which can exhaust the
  // internal heap and abort (exceptions are compiled out on ESP-IDF).
  response_body = std::move(ctx.body);
  return true;
}

std::string BambuddyAPIComponent::backend_url_for(const char *prefix,
                                                   const std::string &path) const {
  std::string url = backend_url_;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += prefix;
  url += path;
  return url;
}

bool BambuddyAPIComponent::http_post(const std::string &path,
                                      const std::string &json_body,
                                      std::string &response_body) {
  return http_request(HTTP_METHOD_POST,
                      backend_url_for("/api/v1/spoolbuddy", path), json_body,
                      response_body, 5000, true, false);
}

bool BambuddyAPIComponent::http_get_api(const std::string &path,
                                         std::string &response_body) {
  return http_request(HTTP_METHOD_GET, backend_url_for("/api/v1", path), "",
                      response_body, 5000, true, false);
}

bool BambuddyAPIComponent::http_post_api(const std::string &path,
                                          const std::string &json_body,
                                          std::string &response_body) {
  return http_request(HTTP_METHOD_POST, backend_url_for("/api/v1", path),
                      json_body, response_body, 5000, true, false);
}

bool BambuddyAPIComponent::http_patch_api(const std::string &path,
                                           const std::string &json_body,
                                           std::string &response_body) {
  return http_request(HTTP_METHOD_PATCH, backend_url_for("/api/v1", path),
                      json_body, response_body, 5000, true, false);
}

bool BambuddyAPIComponent::http_delete_api(const std::string &path,
                                            std::string &response_body) {
  return http_request(HTTP_METHOD_DELETE, backend_url_for("/api/v1", path), "",
                      response_body, 5000, true, false);
}

bool BambuddyAPIComponent::http_post_direct(const std::string &url,
                                             const std::string &json_body,
                                             std::string &response_body) {
  // Scale→console push: full URL, short timeout, no API key, quiet logging.
  return http_request(HTTP_METHOD_POST, url, json_body, response_body, 3000,
                      false, true);
}

// ---------------------------------------------------------------------------
// Printer & AMS API
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::set_selected_printer(int idx) {
  // Called from the LVGL/main task — guard shared state.
  lock_state();
  if (idx < 0) idx = 0;
  if (!display_state_.printers.empty() &&
      idx >= (int)display_state_.printers.size()) idx = 0;
  // apply_tab re-applies the stored selection on every tab switch, so only react
  // when the printer actually changes — otherwise switching tabs would clear the
  // populated AMS and leave the view empty until the forced re-poll completes.
  bool changed = (idx != display_state_.selected_printer_idx);
  display_state_.selected_printer_idx = idx;
  if (!display_state_.printers.empty())
    display_state_.selected_printer_id = display_state_.printers[idx].id;
  if (changed) {
    // Drop the previous printer's AMS at once so the dynamic view doesn't show
    // stale units until the forced re-poll below completes.
    display_state_.ams_units.clear();
    display_state_.dual_nozzle = false;
  }
  unlock_state();
  if (changed)
    last_printer_poll_ms_ = 0;  // force a printer/AMS refresh on next task tick
}

void BambuddyAPIComponent::api_get_printers() {
  std::string resp;
  // Printer list lives at /api/v1/printers/ — NOT under /api/v1/spoolbuddy
  if (!http_get_api("/printers/", resp)) {
    ESP_LOGW(TAG, "GET /api/v1/printers/ failed");
    return;
  }
  ESP_LOGD(TAG, "Printers raw response: %.300s", resp.c_str());

  std::vector<PrinterInfo> printers;
  parse_printer_list(resp, printers);
  if (printers.empty()) {
    ESP_LOGW(TAG, "Printer list empty or unparseable");
    return;
  }

  lock_state();
  display_state_.printers = printers;
  if (display_state_.selected_printer_idx >= (int)printers.size())
    display_state_.selected_printer_idx = 0;
  display_state_.selected_printer_id =
      printers[display_state_.selected_printer_idx].id;
  std::string sel = display_state_.selected_printer_id;
  unlock_state();
  printers_fetched_ = true;  // first good fetch → switch to the slow poll cadence

  ESP_LOGI(TAG, "Printers: %d (selected: %s)", (int)printers.size(),
           sel.c_str());
  for (const auto &p : printers) {
    ESP_LOGI(TAG, "  Printer id=%s name='%s' online=%s",
             p.id.c_str(), p.name.c_str(), p.online ? "yes" : "no");
  }

  // Eagerly fetch assignments so spool IDs are populated before the first
  // AMS poll completes.  The AMS-change detector inside api_get_ams() will
  // re-fetch again on the first AMS response, but that can take several
  // seconds; doing it here gets the data in front of the user immediately.
  if (!sel.empty()) {
    api_get_assignments(sel);
    api_get_ams_labels(sel);  // custom AMS names; cached and re-applied each AMS poll
  }
}

void BambuddyAPIComponent::api_get_printer_status(const std::string &printer_id) {
  std::string resp;
  std::string path = "/printers/" + printer_id + "/status";
  if (!http_get_api(path.c_str(), resp)) {
    ESP_LOGW(TAG, "GET /api/v1%s failed", path.c_str());
    return;
  }
  bool connected = parse_json_bool(resp, "connected", false);
  lock_state();
  display_state_.printer_connected = connected;
  unlock_state();
  ESP_LOGD(TAG, "Printer %s connected=%s", printer_id.c_str(), connected ? "true" : "false");
}

void BambuddyAPIComponent::api_get_ams() {
  // Copy the selected printer id under lock before doing HTTP.
  lock_state();
  std::string printer_id = display_state_.selected_printer_id;
  unlock_state();
  if (printer_id.empty()) return;

  std::string resp;
  // Printer status (including AMS) lives at /api/v1/printers/{id}/status
  if (!http_get_api("/printers/" + printer_id + "/status", resp)) {
    ESP_LOGW(TAG, "GET /api/v1/printers/%s/status failed", printer_id.c_str());
    return;
  }

  std::vector<AMSUnit> units;
  parse_ams_data(resp, units);

  // Apply ams_extruder_map — top-level dict {"ams_id_str": extruder_id} that tells
  // which nozzle (0=right, 1=left) each AMS unit feeds on dual-nozzle printers.
  // parse_ams_data reads from per-unit JSON objects which don't carry this field;
  // it must be parsed from the top level and stamped onto each unit.
  //
  // AMS-HT global tray ID = unit.id directly (ids 128-135), NOT unit.id*4+slot.
  // Regular AMS:  global_tray_id = unit.id * 4 + slot  (unit ids 0-3)
  // External VT:  global_tray_id = unit.id (254 or 255); nozzle set by id
  //               when ≥2 VT units present: id=254→left(1), id=255→right(0)
  {
    // Extract the ams_extruder_map object so we search only within it (avoids
    // false matches on numeric keys like "0" elsewhere in the response).
    size_t emp = resp.find("\"ams_extruder_map\"");
    if (emp != std::string::npos) {
      size_t ob = resp.find('{', emp);
      if (ob != std::string::npos) {
        size_t cb = ob;
        int dep = 0;
        for (; cb < resp.size(); cb++) {
          if (resp[cb] == '{') dep++;
          else if (resp[cb] == '}' && --dep == 0) { cb++; break; }
        }
        std::string em_json = resp.substr(ob, cb - ob);
        for (auto &u : units) {
          if (u.is_vt) continue;  // VT trays below
          int n = parse_json_int(em_json, std::to_string(u.id), -1);
          if (n == 0 || n == 1) u.nozzle = n;
        }
      }
    }
    // Dual-external (H2D etc.): id=254→left(1), id=255→right(0)
    int vt_count = 0;
    for (const auto &u : units) if (u.is_vt) vt_count++;
    if (vt_count >= 2) {
      for (auto &u : units) {
        if (u.is_vt) u.nozzle = (u.id == 254) ? 1 : 0;
      }
    }
  }

  // Dual-nozzle printers route some AMS to the right nozzle (0) and some to the
  // left (1). If we see both sides assigned, treat the printer as dual-nozzle so
  // the UI can split the AMS view left/right.
  bool has_r = false, has_l = false;
  for (const auto &u : units) {
    if (u.nozzle == 0) has_r = true;
    else if (u.nozzle == 1) has_l = true;
  }
  // Filament track switch ("fila_switch", top-level): when installed,
  // ams_extruder_map is always empty — nozzle routing comes from FTS data.
  //
  // Parse out_extruders to determine whether the FTS itself is dual-nozzle:
  //   All same nozzle  → effectively single-nozzle; suppress dual display.
  //   Both 0 and 1     → FTS dual-nozzle; derive per-unit nozzle from in_slots.
  //
  // in_slots encoding observed on H2C with n3f AMS modules:
  //   in_slots[track] = (ams_id << 8) | slot_within_ams, or -1 = disconnected.
  // This differs from the standard tray_now formula (ams_id*4+slot); this is a
  // firmware-level FTS slot address, not the standard global tray ID.
  bool fila_switch = false;
  bool fts_dual    = false;
  {
    size_t fp = resp.find("\"fila_switch\"");
    if (fp != std::string::npos) {
      size_t ob = resp.find('{', fp);
      if (ob != std::string::npos) {
        int depth = 0; size_t i = ob;
        for (; i < resp.size(); i++) {
          if (resp[i] == '{') depth++;
          else if (resp[i] == '}' && --depth == 0) { i++; break; }
        }
        std::string fs_json = resp.substr(ob, i - ob);
        fila_switch = parse_json_bool(fs_json, "installed", false);

        if (fila_switch) {
          // Inline integer-array parser (no external library).
          std::vector<int> out_ext, in_sl;
          auto parse_int_arr = [&](const std::string &src,
                                   const std::string &key,
                                   std::vector<int> &dst) {
            std::string needle = "\"" + key + "\"";
            size_t kp = src.find(needle);
            if (kp == std::string::npos) return;
            size_t ap = src.find('[', kp);
            if (ap == std::string::npos) return;
            size_t ep = src.find(']', ap);
            if (ep == std::string::npos) return;
            size_t p = ap + 1;
            while (p < ep) {
              while (p < ep && (src[p] == ' ' || src[p] == ',')) p++;
              if (p >= ep) break;
              if (src[p] == '-' || (src[p] >= '0' && src[p] <= '9')) {
                char *end;
                long v = strtol(src.c_str() + p, &end, 10);
                if (end > src.c_str() + p) {
                  dst.push_back((int)v);
                  p = (size_t)(end - src.c_str());
                } else { p++; }
              } else { p++; }
            }
          };
          parse_int_arr(fs_json, "out_extruders", out_ext);
          parse_int_arr(fs_json, "in_slots",      in_sl);

          // Check whether FTS routes to both nozzles.
          bool fts_r = false, fts_l = false;
          for (int e : out_ext) {
            if (e == 0) fts_r = true;
            if (e == 1) fts_l = true;
          }
          fts_dual = fts_r && fts_l;

          if (fts_dual) {
            // Derive per-unit nozzle from FTS track routing.
            // in_slots[t] = (ams_id << 8) | slot_within_ams
            for (int t = 0;
                 t < (int)in_sl.size() && t < (int)out_ext.size(); t++) {
              int slot_enc = in_sl[t];
              if (slot_enc < 0) continue;
              int fts_ams_id = slot_enc >> 8;
              int nozzle = out_ext[t];
              if (nozzle != 0 && nozzle != 1) continue;
              for (auto &u : units)
                if (!u.is_vt && u.id == fts_ams_id) u.nozzle = nozzle;
            }
          }
        }
      }
    }
  }
  // Dual-nozzle: ams_extruder_map maps both sides (non-FTS), OR FTS routes to
  // both nozzles (fts_dual). When FTS is installed but all outputs go to the
  // same nozzle (e.g. H2C/single-nozzle setup), stay in single-nozzle mode.
  bool dual = (has_r && has_l && !fila_switch) || fts_dual;

  // The AMS count is dynamic, so ALWAYS publish the freshly-fetched set. An empty
  // result (printer with no AMS, or every unit removed) must REPLACE the previous
  // units so stale cards disappear — not be skipped. (A transport failure returns
  // earlier, above, and intentionally keeps the last good data through a glitch.)
  // Parse tray_now from the top-level response.
  // 255 = no filament, 254 = external/bypass spool, else ams_id*4+slot_id.
  int tray_now = parse_json_int(resp, "tray_now", 255);

  // Stamp the user's custom AMS names onto the real (non-external) units.
  for (auto &u : units) {
    if (u.is_vt) continue;
    auto it = ams_labels_.find(u.id);
    if (it != ams_labels_.end()) u.custom_name = it->second;
  }

  lock_state();
  display_state_.ams_units  = units;
  display_state_.dual_nozzle = dual;
  display_state_.tray_now   = tray_now;
  // Re-apply cached assignments so spool IDs survive the fresh parse overwrite
  // (parse_ams_data leaves the assignment fields empty); re-stamped on every
  // poll so IDs are stable between api_get_assignments re-fetches.
  apply_cached_assignments_locked();
  unlock_state();

  // Auto-assign: compare new snapshot against previous to find newly loaded slots.
  // prev_ams_units_ is empty on the very first poll — skip that cycle so we
  // establish a clean baseline before reacting to changes.
  if (!prev_ams_units_.empty()) {
    int pending_id = 0;
    uint32_t expiry = 0;
    std::string sel_printer;
    lock_state();
    pending_id = pending_assign_spool_id_;
    expiry = pending_assign_expiry_ms_;
    sel_printer = display_state_.selected_printer_id;
    unlock_state();

    if (pending_id > 0 && millis() < expiry) {
      bool assigned = false;
      for (const auto &unit : units) {
        if (unit.is_vt || assigned) continue;  // skip external bypass trays
        for (const auto &tray : unit.trays) {
          // Only react to slots that are loaded but have no existing assignment.
          if (!tray.present || tray.spool_id != 0 || assigned) continue;
          bool was_present = false;
          for (const auto &punit : prev_ams_units_) {
            if (punit.id != unit.id) continue;
            for (const auto &ptray : punit.trays) {
              if (ptray.slot == tray.slot) {
                was_present = ptray.present;
                break;
              }
            }
            break;
          }
          if (!was_present) {
            ESP_LOGI(TAG, "Newly loaded: AMS%d-T%d — assigning spool %d",
                     unit.id, tray.slot, pending_id);
            assigned = api_assign_spool(pending_id, sel_printer, unit.id, tray.slot);
            if (assigned) {
              lock_state();
              pending_assign_spool_id_ = 0;
              display_state_.spool_assign_expiry_ms = 0;
              // Build slot label e.g. "AMS-B · Slot 2"
              char desc[32];
              snprintf(desc, sizeof(desc), "AMS-%c \xC2\xB7 Slot %d",
                       'A' + (char)(unit.id % 26), tray.slot + 1);
              display_state_.assign_success = true;
              display_state_.assign_slot_desc = std::string(desc);
              unlock_state();
              assign_clear_ms_ = millis() + 4000;  // clear NFC page after 4 s
            }
          }
        }
      }
    }
  }
  // Auto-load on AMS removal: when a known spool (spool_id > 0) transitions
  // from present → absent, show it in the NFC tab for immediate quick-weigh.
  // Only triggers when the UI is idle (no spool selected, no tag resolving)
  // so an in-progress NFC scan is not interrupted.
  if (!prev_ams_units_.empty()) {
    int removed_id = 0;
    FilamentInfo removed_fi{};
    lock_state();
    bool sel      = display_state_.spool_selected;
    bool resolving = display_state_.tag_resolving;
    unlock_state();
    if (!sel && !resolving) {
      for (const auto &punit : prev_ams_units_) {
        if (punit.is_vt || removed_id != 0) continue;
        for (const auto &ptray : punit.trays) {
          if (!ptray.present || ptray.spool_id == 0 || removed_id != 0) continue;
          bool now_gone = true;
          for (const auto &unit : units) {
            if (unit.id != punit.id) continue;
            for (const auto &tray : unit.trays) {
              if (tray.slot == ptray.slot) { now_gone = !tray.present; break; }
            }
            break;
          }
          if (now_gone) {
            removed_id               = ptray.spool_id;
            removed_fi.spool_id      = ptray.spool_id;
            removed_fi.material_type = ptray.material_type;
            removed_fi.color_hex     = ptray.color_hex;
            removed_fi.color_name    = ptray.color_name;
            removed_fi.brand         = ptray.brand;
            removed_fi.subtype       = ptray.subtype;
            removed_fi.label_weight_g= ptray.label_weight_g;
            removed_fi.min_temp      = (float)ptray.nozzle_temp_min;
            removed_fi.max_temp      = (float)ptray.nozzle_temp_max;
          }
        }
      }
    }
    if (removed_id > 0) {
      ESP_LOGI(TAG, "Spool %d removed from AMS — auto-loading for quick weight", removed_id);
      // Build toast description before activate_spool() takes the lock.
      std::string desc;
      if (!removed_fi.material_type.empty()) desc = removed_fi.material_type;
      if (!removed_fi.subtype.empty()) desc += " " + removed_fi.subtype;
      { char buf[16]; snprintf(buf, sizeof(buf), " #%d", removed_id); desc += buf; }
      desc += " removed - ready for weighing";
      // Treat removal exactly like an NFC scan: set all quick-weight state,
      // arm the auto-assign TTL (for re-insertion detection), call api_get_spool().
      activate_spool(removed_fi, "ams:" + std::to_string(removed_id), TagSource::AMS);
      lock_state();
      display_state_.nfc_scan_generation++;        // audio cue (NFC path does this in on_tag_scanned)
      display_state_.ams_removed_spool_desc      = desc;
      display_state_.ams_removed_toast_expiry_ms = millis() + 5000;
      unlock_state();
    }
  }

  // Detect AMS content changes to decide whether to refresh inventory assignments.
  // Always fetch on the very first poll (prev empty); afterwards only when
  // the set of units, loaded slots, or filament types changes.
  bool ams_changed = prev_ams_units_.empty() || (prev_ams_units_.size() != units.size());
  if (!ams_changed) {
    for (size_t i = 0; i < units.size() && !ams_changed; i++) {
      const auto &a = prev_ams_units_[i];
      const auto &b = units[i];
      if (a.id != b.id || a.trays.size() != b.trays.size()) {
        ams_changed = true;
        break;
      }
      for (size_t j = 0; j < a.trays.size() && !ams_changed; j++) {
        if (a.trays[j].present       != b.trays[j].present ||
            a.trays[j].material_type != b.trays[j].material_type ||
            a.trays[j].color_hex     != b.trays[j].color_hex) {
          ams_changed = true;
        }
      }
    }
  }
  if (ams_changed) {
    api_get_assignments(printer_id);
  }

  // Snapshot the assignment-enriched state, not the raw parse, so the removal
  // detection above can compare spool_id > 0 on the next poll.
  lock_state();
  prev_ams_units_ = display_state_.ams_units;
  unlock_state();

  if (units.empty()) {
    ESP_LOGI(TAG, "AMS: none reported for printer '%s' (cleared)",
             printer_id.c_str());
    return;
  }

  ESP_LOGI(TAG, "AMS: %d unit(s) for printer '%s'%s%s", (int)units.size(),
           printer_id.c_str(), dual ? " (dual-nozzle)" : "",
           fila_switch ? " (track switch installed)" : "");
  for (const auto &unit : units) {
    int present = 0;
    for (const auto &t : unit.trays) if (t.present) present++;
    ESP_LOGD(TAG, "  %s (id %d): %d/%d loaded, %.1f°C %d%%, nozzle=%s",
             unit.name.c_str(), unit.id, present, (int)unit.trays.size(),
             unit.temp, unit.humidity,
             unit.nozzle == 0 ? "R" : unit.nozzle == 1 ? "L" : "-");
    for (const auto &t : unit.trays) {
      if (t.present) {
        ESP_LOGD(TAG, "    Slot %d: %s #%s %d-%d",
                 t.slot,
                 t.material_type.empty() ? "?" : t.material_type.c_str(),
                 t.color_hex.empty()     ? "??????" : t.color_hex.c_str(),
                 t.nozzle_temp_min, t.nozzle_temp_max);
      } else {
        ESP_LOGD(TAG, "    Slot %d: empty", t.slot);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Inventory assignments — populate spool_id in AMS tray slots
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::api_get_assignments(const std::string &printer_id) {
  if (printer_id.empty()) return;

  // The printer_id stored in DisplayState is the string form of the numeric id.
  // Build the query string using that value directly.
  std::string path = spoolman_inventory_
      ? "/spoolman/inventory/slot-assignments/all?printer_id=" + printer_id
      : "/inventory/assignments?printer_id=" + printer_id;
  std::string resp;
  if (!http_get_api(path, resp)) {
    ESP_LOGW(TAG, "GET /api/v1%s failed", path.c_str());
    return;
  }

  // Parse the JSON array: each element is expected to contain at minimum
  //   "ams_id": <int>, "tray_id": <int>, "spool_id": <int>
  // We extract every top-level object and look for those three fields.
  // Unknown fields are ignored so the parser stays forward-compatible.
  std::vector<std::string> objs = json_array_objects(resp);
  if (objs.empty()) {
    ESP_LOGD(TAG, "Assignments: empty or unparseable response for printer %s",
             printer_id.c_str());
    return;
  }

  // Parse each assignment object into the member cache.
  // Replace the whole cache so removed assignments clear automatically.
  cached_assignments_.clear();
  cached_assignments_.reserve(objs.size());
  for (const auto &obj : objs) {
    int   ai  = parse_json_int(obj,   "ams_id",       -1);
    int   ti  = parse_json_int(obj,   "tray_id",      -1);
    int   si  = parse_json_int(obj,   "spool_id",      0);
    // Spoolman's slot-assignments response may key the spool id differently
    // than the internal-mode response; fall back if the primary key is absent.
    if (si <= 0 && spoolman_inventory_)
      si = parse_json_int(obj, "spoolman_spool_id", 0);
    // Weight data is nested inside the "spool" sub-object as:
    //   "label_weight": <int grams>   and   "weight_used": <float grams>
    // Our flat extract_value() finds these by key name anywhere in obj.
    float lw  = parse_json_float(obj,  "label_weight",  0.0f);
    float wu  = parse_json_float(obj,  "weight_used",   0.0f);
    std::string mat  = parse_json_string(obj, "material");
    std::string rgba = parse_json_string(obj, "rgba");
    std::string br   = parse_json_string(obj, "brand");
    std::string st   = parse_json_string(obj, "subtype");
    std::string cn   = parse_json_string(obj, "color_name");
    if (ai >= 0 && ti >= 0 && si > 0) {
      SlotAssignment a;
      a.ams_id         = ai;
      a.tray_id        = ti;
      a.spool_id       = si;
      a.label_weight_g = lw;
      a.remaining_g    = (lw > 0.0f) ? std::max(0.0f, lw - wu) : 0.0f;
      a.material_type  = mat;
      a.color_hex      = (rgba.size() >= 6) ? rgba.substr(0, 6) : rgba;
      a.brand          = br;
      a.subtype        = st;
      a.color_name     = cn;
      cached_assignments_.push_back(a);
    }
  }

  ESP_LOGI(TAG, "Assignments: %d known slot(s) for printer %s",
           (int)cached_assignments_.size(), printer_id.c_str());

  // Merge into display_state_.ams_units immediately.  api_get_ams() re-applies
  // the cache after every poll to keep these values alive.
  lock_state();
  apply_cached_assignments_locked();
  unlock_state();
}

void BambuddyAPIComponent::api_get_ams_labels(const std::string &printer_id) {
  // Refetch into a clean map so a removed/renamed label can't linger, and a
  // failed request just falls back to the default AMS-A / AMS-B names.
  ams_labels_.clear();
  if (printer_id.empty()) return;
  std::string resp;
  if (!http_get_api("/printers/" + printer_id + "/ams-labels", resp)) {
    ESP_LOGD(TAG, "GET /api/v1/printers/%s/ams-labels failed (no custom names)",
             printer_id.c_str());
    return;
  }
  // Response is a flat object {"<ams_id>": "<name>", ...}. Walk "key":"value"
  // string pairs, honouring backslash escapes inside the value.
  size_t i = 0;
  while (true) {
    size_t kq = resp.find('"', i);
    if (kq == std::string::npos) break;
    size_t ke = resp.find('"', kq + 1);
    if (ke == std::string::npos) break;
    std::string key = resp.substr(kq + 1, ke - kq - 1);
    size_t colon = resp.find(':', ke + 1);
    if (colon == std::string::npos) break;
    size_t vq = resp.find('"', colon + 1);
    if (vq == std::string::npos) break;
    std::string val;
    size_t p = vq + 1;
    for (; p < resp.size(); ++p) {
      char c = resp[p];
      if (c == '\\' && p + 1 < resp.size()) { val.push_back(resp[p + 1]); ++p; continue; }
      if (c == '"') break;
      val.push_back(c);
    }
    char *endp = nullptr;
    long id = strtol(key.c_str(), &endp, 10);
    if (endp != key.c_str() && !val.empty()) ams_labels_[(int)id] = val;
    i = p + 1;
  }
  if (!ams_labels_.empty())
    ESP_LOGI(TAG, "AMS labels: %d custom name(s) for printer %s",
             (int)ams_labels_.size(), printer_id.c_str());
}

void BambuddyAPIComponent::apply_cached_assignments_locked() {
  for (auto &unit : display_state_.ams_units) {
    for (auto &tray : unit.trays) {
      tray.spool_id       = 0;   // reset first so removed assignments clear too
      tray.label_weight_g = 0.0f;
      tray.remaining_g    = 0.0f;
      tray.brand.clear();
      tray.subtype.clear();
      tray.color_name.clear();
      for (const auto &a : cached_assignments_) {
        if (a.ams_id == unit.id && a.tray_id == tray.slot) {
          tray.spool_id        = a.spool_id;
          tray.label_weight_g  = a.label_weight_g;
          tray.remaining_g     = a.remaining_g;
          if (!a.material_type.empty()) tray.material_type = a.material_type;
          if (!a.color_hex.empty())     tray.color_hex     = a.color_hex;
          tray.brand           = a.brand;
          tray.subtype         = a.subtype;
          tray.color_name      = a.color_name;
          break;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// AMS auto-assign
// ---------------------------------------------------------------------------

bool BambuddyAPIComponent::api_assign_spool(int spool_id,
                                             const std::string &printer_id,
                                             int ams_id, int tray_id) {
  if (printer_id.empty()) {
    ESP_LOGW(TAG, "api_assign_spool: no printer selected");
    return false;
  }
  int pid = (int)strtol(printer_id.c_str(), nullptr, 10);
  if (pid <= 0) {
    ESP_LOGW(TAG, "api_assign_spool: invalid printer_id '%s'", printer_id.c_str());
    return false;
  }
  std::ostringstream js;
  js << "{"
     << (spoolman_inventory_ ? "\"spoolman_spool_id\":" : "\"spool_id\":") << spool_id << ","
     << "\"printer_id\":" << pid << ","
     << "\"ams_id\":" << ams_id << ","
     << "\"tray_id\":" << tray_id
     << "}";
  std::string resp;
  bool ok = http_post_api(spoolman_inventory_ ? "/spoolman/inventory/slot-assignments"
                                               : "/inventory/assignments",
                           js.str(), resp);
  if (ok) {
    ESP_LOGI(TAG, "Auto-assigned spool %d -> printer %d AMS%d-T%d",
             spool_id, pid, ams_id, tray_id);
    set_status("Spool assigned to AMS " + std::to_string(ams_id + 1) +
               " slot " + std::to_string(tray_id + 1));
  } else {
    ESP_LOGW(TAG, "Auto-assign failed: spool %d -> printer %d AMS%d-T%d",
             spool_id, pid, ams_id, tray_id);
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Slot assignment management (called from UI task via enqueue_job)
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::clear_slot_assignment(int ams_id, int tray_slot) {
  // Clear all assignment-derived fields immediately (same set that
  // apply_cached_assignments_locked resets) so the UI renders the empty
  // slot on the very next interval tick without waiting for the HTTP job.
  // Also purge the cache entry so the next AMS poll doesn't re-stamp it.
  // The spool id is captured before clearing because Spoolman's unassign
  // endpoint is keyed by spool id, not by printer/ams/tray.
  int prev_spool_id = 0;
  lock_state();
  for (auto &unit : display_state_.ams_units) {
    if (unit.id != ams_id) continue;
    for (auto &tray : unit.trays) {
      if (tray.slot != tray_slot) continue;
      prev_spool_id        = tray.spool_id;
      tray.spool_id       = 0;
      tray.label_weight_g = 0.0f;
      tray.remaining_g    = 0.0f;
      tray.brand.clear();
      tray.subtype.clear();
      tray.color_name.clear();
      tray.material_type.clear();
      tray.color_hex.clear();
      break;
    }
    break;
  }
  cached_assignments_.erase(
    std::remove_if(cached_assignments_.begin(), cached_assignments_.end(),
      [&](const SlotAssignment &a) {
        return a.ams_id == ams_id && a.tray_id == tray_slot;
      }),
    cached_assignments_.end());
  unlock_state();
  // Async HTTP call — runs on the HTTP task so the UI stays responsive.
  HttpJob job;
  job.kind = HttpJob::CLEAR_ASSIGNMENT;
  job.i1 = ams_id;
  job.i2 = tray_slot;
  job.i3 = prev_spool_id;
  enqueue_job(job);
}

void BambuddyAPIComponent::assign_spool_to_slot(int spool_id, int ams_id,
                                                int tray_slot) {
  if (spool_id <= 0) return;
  set_status("Assigning spool #" + std::to_string(spool_id) + "...");
  HttpJob job;
  job.kind = HttpJob::ASSIGN_SPOOL;
  job.i1 = spool_id;
  job.i2 = (ams_id << 8) | (tray_slot & 0xFF);  // pack ams_id + slot
  enqueue_job(job);
}

bool BambuddyAPIComponent::api_clear_assignment(const std::string &printer_id,
                                                 int ams_id, int tray_slot,
                                                 int spool_id) {
  if (printer_id.empty()) {
    ESP_LOGW(TAG, "api_clear_assignment: no printer selected");
    return false;
  }
  char qpath[128];
  if (spoolman_inventory_) {
    if (spool_id <= 0) {
      ESP_LOGW(TAG, "api_clear_assignment: no spool id for Spoolman unassign "
                    "(printer %s AMS%d-T%d)", printer_id.c_str(), ams_id, tray_slot);
      return false;
    }
    // DELETE /api/v1/spoolman/inventory/slot-assignments/{spool_id}
    snprintf(qpath, sizeof(qpath), "/spoolman/inventory/slot-assignments/%d", spool_id);
  } else {
    // DELETE /api/v1/inventory/assignments/{printer_id}/{ams_id}/{tray_id}
    snprintf(qpath, sizeof(qpath), "/inventory/assignments/%s/%d/%d",
             printer_id.c_str(), ams_id, tray_slot);
  }
  std::string resp;
  bool ok = http_delete_api(qpath, resp);
  if (ok) {
    ESP_LOGI(TAG, "Cleared assignment: printer %s AMS%d-T%d",
             printer_id.c_str(), ams_id, tray_slot);
  } else {
    ESP_LOGW(TAG, "Failed to clear assignment: printer %s AMS%d-T%d",
             printer_id.c_str(), ams_id, tray_slot);
  }
  return ok;
}

// Extract each top-level JSON object from an array: "[{...},{...}]"
std::vector<std::string> BambuddyAPIComponent::json_array_objects(
    const std::string &json) {
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos < json.size() && json[pos] != '[') pos++;
  if (pos >= json.size()) return result;
  pos++;
  while (pos < json.size()) {
    while (pos < json.size() && json[pos] != '{' && json[pos] != ']') pos++;
    if (pos >= json.size() || json[pos] == ']') break;
    int depth = 0;
    size_t start = pos;
    while (pos < json.size()) {
      if (json[pos] == '{') depth++;
      else if (json[pos] == '}') {
        depth--;
        if (depth == 0) {
          result.push_back(json.substr(start, pos - start + 1));
          pos++;
          break;
        }
      }
      pos++;
    }
  }
  return result;
}

void BambuddyAPIComponent::parse_printer_list(const std::string &json,
                                               std::vector<PrinterInfo> &out) {
  out.clear();
  for (const auto &obj : json_array_objects(json)) {
    PrinterInfo p;
    // API returns integer id — store as string for URL construction
    int id_int = parse_json_int(obj, "id", 0);
    if (id_int == 0) continue;
    p.id = std::to_string(id_int);
    p.name = parse_json_string(obj, "name");
    // bambuddy API uses "online"; fall back to "is_active" for older versions
    p.online = parse_json_bool(obj, "online", parse_json_bool(obj, "is_active", false));
    if (!p.name.empty()) out.push_back(p);
  }
}

// ---------------------------------------------------------------------------
// Scale server — HTTP server on the scale device
// ---------------------------------------------------------------------------

esp_err_t BambuddyAPIComponent::scale_http_weight(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  self->lock_state();
  float raw_w       = self->display_state_.weight_grams;  // ESPHome-filtered, pre-tare
  bool  st          = self->display_state_.weight_stable;
  bool  nfc_pres    = (self->display_state_.nfc_state == NFCTagState::PRESENT);
  std::string uid   = self->display_state_.last_tag_uid;
  std::string tuuid = self->display_state_.current_filament.tray_uuid;
  int  sak           = self->display_state_.current_filament.sak;
  std::string ttype  = self->display_state_.current_filament.tag_type;
  // Read tare/cal while holding the lock (same mutex guards local_tare writes).
  float tare   = self->tare_offset_;
  float factor = self->calibration_factor_;
  self->unlock_state();

  // Apply runtime tare offset and calibration factor.
  float w = (raw_w - tare) * factor;

  // Escape any double-quotes that might appear in string fields.
  // (UIDs and UUIDs are hex/dashes, so in practice this is just defensive.)
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"weight_grams\":%.2f,\"stable\":%s,\"hostname\":\"%s\","
           "\"nfc_present\":%s,\"nfc_uid\":\"%s\",\"nfc_tray_uuid\":\"%s\","
           "\"nfc_sak\":%d,\"nfc_tag_type\":\"%s\"}",
           w, st ? "true" : "false", esc(self->hostname_).c_str(),
           nfc_pres ? "true" : "false",
           esc(uid).c_str(), esc(tuuid).c_str(),
           sak, esc(ttype).c_str());
  return send_json(req, buf);
}

esp_err_t BambuddyAPIComponent::scale_http_tare(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  // Read the raw (pre-tare) weight and set it as the new zero-point.
  self->lock_state();
  float raw = self->display_state_.weight_grams;  // raw ESPHome-filtered value
  float factor = self->calibration_factor_;
  self->tare_offset_ = raw;
  self->unlock_state();

  self->save_calibration_nvs();

  // Net weight after tare (should be ~0).
  float net = (raw - self->tare_offset_) * factor;

  char buf[64];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"tare_offset\":%.2f,\"weight_grams\":%.2f}",
           self->tare_offset_, net);
  ESP_LOGI(TAG, "Scale server tare: offset=%.2f", self->tare_offset_);
  return send_json(req, buf);
}

esp_err_t BambuddyAPIComponent::scale_http_calibrate(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  // Read JSON body: {"reference_weight_grams": N}
  std::string json;
  float ref_g = 0.0f;
  if (read_req_body(req, json))
    ref_g = parse_json_float(json, "reference_weight_grams", 0.0f);
  if (ref_g <= 0.0f) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "reference_weight_grams must be > 0");
    return ESP_FAIL;
  }

  // Capture the current tare-adjusted net reading.
  self->lock_state();
  float measured = self->display_state_.weight_grams;
  self->unlock_state();
  float net = measured - self->tare_offset_;

  if (net <= 0.0f) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Scale reading is zero or negative — place weight first");
    return ESP_FAIL;
  }

  // new_factor = reference / net_reading
  float new_factor = ref_g / net;
  self->calibration_factor_ = new_factor;
  self->save_calibration_nvs();

  char buf[80];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"calibration_factor\":%.6f,\"reference_g\":%.1f,\"measured_g\":%.1f}",
           new_factor, ref_g, net);
  ESP_LOGI(TAG, "Scale server calibrate: ref=%.1f g net=%.1f g factor=%.6f",
           ref_g, net, new_factor);
  return send_json(req, buf);
}

// ---------------------------------------------------------------------------
// Scale push mode — scale → console
// ---------------------------------------------------------------------------

bool BambuddyAPIComponent::api_scale_push_weight(float grams, bool stable) {
  if (console_url_.empty()) return false;

  // Apply tare and calibration before pushing so the console receives net grams.
  float net = (grams - tare_offset_) * calibration_factor_;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"weight_grams\":%.2f,\"stable\":%s,\"hostname\":\"%s\","
           "\"tare_offset\":%.2f,\"calibration_factor\":%.6f}",
           net, stable ? "true" : "false", esc(hostname_).c_str(),
           tare_offset_, calibration_factor_);

  std::string resp;
  bool ok = http_post_direct(console_url_ + ":8080/scale/weight", body, resp);
  if (!ok) {
    ESP_LOGD(TAG, "Scale push weight to console failed");
    return false;
  }
  return true;
  // Commands (tare/calibrate/write_tag) are delivered via heartbeat response,
  // not piggybacked here, so weight push stays a pure data channel.
}

void BambuddyAPIComponent::api_scale_push_heartbeat() {
  if (console_url_.empty()) return;

  char body[128];
  snprintf(body, sizeof(body), "{\"hostname\":\"%s\"}", esc(hostname_).c_str());

  std::string resp;
  bool ok = http_post_direct(console_url_ + ":8080/scale/heartbeat", body, resp);
  if (!ok) {
    ESP_LOGD(TAG, "Scale heartbeat to console failed");
    return;
  }
  bool first_success = (last_push_ok_ms_ == 0);
  last_push_ok_ms_ = millis();

  // On first successful heartbeat after boot, force-push the current weight so
  // the console immediately has a reading — even if it is 0 or unchanged (the
  // ESPHome delta filter would otherwise suppress on_scale_reading() callbacks
  // until the weight changes by ≥ 0.3 g).
  if (first_success && !push_weight_pending_.exchange(true)) {
    lock_state();
    float force_g  = display_state_.weight_grams;
    bool  force_st = display_state_.weight_stable;
    unlock_state();
    HttpJob job;
    job.kind = HttpJob::SCALE_PUSH_WEIGHT;
    job.f1   = force_g;
    job.b1   = force_st;
    enqueue_job(job);
    ESP_LOGI(TAG, "First heartbeat OK — force-pushing initial weight %.1f g", force_g);
  }

  // Process any pending command from the console.
  std::string cmd = parse_json_string(resp, "cmd");
  if (cmd == "tare") {
    ESP_LOGI(TAG, "Scale received tare command via heartbeat");
    local_tare();
  } else if (cmd == "calibrate") {
    float ref = parse_json_float(resp, "value", 0.0f);
    if (ref > 0.0f) {
      lock_state();
      float raw_net  = display_state_.weight_grams - tare_offset_;
      float force_g  = display_state_.weight_grams;
      bool  force_st = display_state_.weight_stable;
      unlock_state();
      if (raw_net > 0.0f) {
        calibration_factor_ = ref / raw_net;
        save_calibration_nvs();
        ESP_LOGI(TAG, "Scale calibrate via heartbeat: ref=%.1f g factor=%.6f",
                 ref, calibration_factor_);
        // Force-push so the console immediately reflects the new calibration.
        if (!push_weight_pending_.exchange(true)) {
          HttpJob job;
          job.kind = HttpJob::SCALE_PUSH_WEIGHT;
          job.f1   = force_g;
          job.b1   = force_st;
          enqueue_job(job);
        }
      }
    }
  } else if (cmd == "set_calibration") {
    // Backend-driven calibration relayed by the console: adopt the values,
    // persist them, and force-push so the recalibrated weight is visible
    // immediately.
    static constexpr float kAbsent = -1e9f;
    float t = parse_json_float(resp, "tare",   kAbsent);
    float f = parse_json_float(resp, "factor", 0.0f);
    lock_state();
    if (t != kAbsent) tare_offset_ = t;
    if (f > 0.0f)     calibration_factor_ = f;
    float force_g  = display_state_.weight_grams;
    bool  force_st = display_state_.weight_stable;
    unlock_state();
    save_calibration_nvs();
    ESP_LOGI(TAG, "Scale received set_calibration via heartbeat: tare=%.2f factor=%.6f",
             tare_offset_, calibration_factor_);
    if (!push_weight_pending_.exchange(true)) {
      HttpJob job;
      job.kind = HttpJob::SCALE_PUSH_WEIGHT;
      job.f1   = force_g;
      job.b1   = force_st;
      enqueue_job(job);
    }
  } else if (cmd == "write_tag") {
    std::string ndef_hex = parse_json_string(resp, "ndef_hex");
    int spool_id = parse_json_int(resp, "spool_id", 0);
    if (!ndef_hex.empty()) {
      pending_write_ndef_     = hex_to_bytes(ndef_hex);
      pending_write_spool_id_ = spool_id;
      pending_write_active_.store(true);
      ESP_LOGI(TAG, "Scale received write_tag cmd via heartbeat: spool_id=%d ndef_len=%zu",
               spool_id, pending_write_ndef_.size());
    }
  }
}

void BambuddyAPIComponent::api_scale_push_nfc_scanned(const std::string &uid,
                                                        const std::string &tray_uuid,
                                                        int sak,
                                                        const std::string &tag_type) {
  if (console_url_.empty()) return;

  char body[384];
  snprintf(body, sizeof(body),
           "{\"uid\":\"%s\",\"tray_uuid\":\"%s\",\"sak\":%d,\"tag_type\":\"%s\"}",
           esc(uid).c_str(), esc(tray_uuid).c_str(), sak, esc(tag_type).c_str());

  std::string resp;
  bool ok = http_post_direct(console_url_ + ":8080/scale/nfc/tag-scanned", body, resp);
  if (ok) last_push_ok_ms_ = millis();
  else ESP_LOGW(TAG, "Scale push nfc-scanned to console failed (uid=%s)", uid.c_str());
}

void BambuddyAPIComponent::api_scale_push_nfc_removed(const std::string &uid) {
  if (console_url_.empty()) return;

  char body[128];
  snprintf(body, sizeof(body), "{\"uid\":\"%s\"}", esc(uid).c_str());

  std::string resp;
  bool ok = http_post_direct(console_url_ + ":8080/scale/nfc/tag-removed", body, resp);
  if (ok) last_push_ok_ms_ = millis();
  else ESP_LOGW(TAG, "Scale push nfc-removed to console failed");
}

void BambuddyAPIComponent::api_scale_push_write_result(int spool_id,
                                                         const std::string &uid,
                                                         bool success,
                                                         const std::string &msg) {
  if (console_url_.empty()) return;

  char body[512];
  snprintf(body, sizeof(body),
           "{\"spool_id\":%d,\"uid\":\"%s\",\"success\":%s,\"message\":\"%s\"}",
           spool_id, esc(uid).c_str(), success ? "true" : "false",
           esc(msg).c_str());

  std::string resp;
  bool ok = http_post_direct(console_url_ + ":8080/scale/nfc/write-result", body, resp);
  if (ok) {
    last_push_ok_ms_ = millis();
    ESP_LOGI(TAG, "Scale pushed write-result to console: spool=%d uid=%s success=%d",
             spool_id, uid.c_str(), (int)success);
  } else {
    ESP_LOGW(TAG, "Scale push write-result to console failed");
  }
}

// ---------------------------------------------------------------------------
// Console receive server — accepts pushes from the scale (port CONSOLE_PUSH_PORT)
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::start_console_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = CONSOLE_PUSH_PORT;
  cfg.stack_size       = 6144;
  cfg.max_uri_handlers = 8;

  httpd_handle_t server = nullptr;
  esp_err_t err = httpd_start(&server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "console httpd_start failed: %s", esp_err_to_name(err));
    return;
  }
  console_server_handle_ = server;

  httpd_uri_t weight_uri  = {"/scale/weight",              HTTP_POST,
                              console_http_scale_weight,            this};
  httpd_uri_t hb_uri      = {"/scale/heartbeat",           HTTP_POST,
                              console_http_scale_heartbeat,         this};
  httpd_uri_t scan_uri    = {"/scale/nfc/tag-scanned",     HTTP_POST,
                              console_http_scale_nfc_scanned,       this};
  httpd_uri_t removed_uri = {"/scale/nfc/tag-removed",     HTTP_POST,
                              console_http_scale_nfc_removed,       this};
  httpd_uri_t wrres_uri   = {"/scale/nfc/write-result",    HTTP_POST,
                              console_http_scale_nfc_write_result,  this};

  httpd_register_uri_handler(server, &weight_uri);
  httpd_register_uri_handler(server, &hb_uri);
  httpd_register_uri_handler(server, &scan_uri);
  httpd_register_uri_handler(server, &removed_uri);
  httpd_register_uri_handler(server, &wrres_uri);

  ESP_LOGI(TAG, "Console receive server started on port %u "
           "(POST /scale/weight, /scale/heartbeat, /scale/nfc/tag-scanned, "
           "/scale/nfc/tag-removed, /scale/nfc/write-result)",
           (unsigned)CONSOLE_PUSH_PORT);
}

esp_err_t BambuddyAPIComponent::console_http_scale_weight(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  std::string json;
  if (!read_req_body(req, json)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_OK;
  }
  float grams   = parse_json_float(json, "weight_grams",       0.0f);
  bool  stable  = parse_json_bool (json, "stable",             false);
  // tare_offset and calibration_factor are sent by the scale on every weight
  // push so the console always has the current values.  Use a large sentinel
  // so a genuine 0.0 tare is still detected.
  static constexpr float kAbsent = -1e9f;
  float new_tare = parse_json_float(json, "tare_offset",       kAbsent);
  float new_cal  = parse_json_float(json, "calibration_factor", kAbsent);

  self->lock_state();
  self->last_scale_push_ms_          = millis();  // any scale→console message counts as "alive"
  self->display_state_.weight_grams  = grams;
  self->display_state_.weight_stable = stable;
  self->display_state_.scale_ok      = true;
  // A tare command was delivered to the scale and this push carries its
  // (post-tare) offset — consume the ack flag.
  bool ack_tare = false;
  if (self->tare_ack_pending_ && new_tare != kAbsent) {
    self->tare_ack_pending_ = false;
    ack_tare = true;
  }
  self->unlock_state();

  // Sync tare from the scale.  POST set-tare to the backend when the offset
  // changed — or unconditionally right after a tare command (ack_tare): the
  // Bambuddy UI confirms completion by watching last_calibrated_at advance,
  // so even an unchanged offset (taring an already-empty scale) must be
  // reported or the UI times out after 15 s.
  if (new_tare != kAbsent &&
      (ack_tare || fabsf(new_tare - self->tare_offset_) > 0.5f)) {
    self->tare_offset_ = new_tare;
    HttpJob tj;
    tj.kind = HttpJob::UPDATE_TARE;
    tj.f1   = new_tare;
    self->enqueue_job(tj);
    ESP_LOGI(TAG, "Console: reporting scale tare_offset %.2f to backend%s",
             new_tare, ack_tare ? " (tare command ack)" : "");
  }
  if (new_cal != kAbsent && new_cal > 0.0f &&
      fabsf(new_cal - self->calibration_factor_) >
          self->calibration_factor_ * 1e-4f) {
    self->calibration_factor_ = new_cal;
    self->lock_state();
    self->display_state_.calibration_factor = new_cal;
    self->unlock_state();
    // Report the scale's new factor to the backend so the next heartbeat
    // response doesn't carry the stale value back and revert it on the scale.
    // The backend only has set-factor (known / (raw - tare)); send a synthetic
    // pair that reproduces the factor: net = 10000 / factor.
    HttpJob cj;
    cj.kind = HttpJob::UPDATE_CALIBRATION;
    cj.f1   = 10000.0f;
    cj.f2   = 10000.0f / new_cal;
    self->enqueue_job(cj);
    ESP_LOGI(TAG, "Console: scale calibration_factor changed to %.6f — notifying backend",
             new_cal);
  }

  // Reconstruct the scale's raw (pre-tare, pre-factor) reading so the backend
  // receives a usable raw_adc — its calibration UI computes
  //   factor = known_weight / (raw_adc - tare)
  // and rejects the request when raw_adc is 0 / equals the tare offset.
  //   net = (raw - tare) * factor  →  raw = net / factor + tare
  float eff_tare = (new_tare != kAbsent) ? new_tare : self->tare_offset_;
  float eff_cal  = (new_cal != kAbsent && new_cal > 0.0f)
                       ? new_cal
                       : self->calibration_factor_;
  int raw_adc = (eff_cal > 0.0f)
                    ? (int)lroundf(grams / eff_cal + eff_tare)
                    : 0;

  // Relay weight to BamBuddy via the existing SCALE_READING path.
  HttpJob job;
  job.kind = HttpJob::SCALE_READING;
  job.f1   = grams;
  job.b1   = stable;
  job.i1   = raw_adc;
  self->enqueue_job(job);

  // Weight push is data-only — commands are delivered via /scale/heartbeat.
  return send_json(req, "{\"ok\":true}");
}

esp_err_t BambuddyAPIComponent::console_http_scale_heartbeat(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  std::string json;
  read_req_body(req, json);  // drain the body; contents are unused (hostname ignored)

  self->lock_state();

  // Update scale presence timestamp + scale_ok flag. scale_ok must be set here
  // (not only in console_http_scale_weight) so the UI shows "scale connected"
  // as soon as the first heartbeat arrives, before any weight data is pushed.
  self->last_scale_push_ms_ = millis();
  self->display_state_.scale_ok = true;

  // Read and clear any pending console→scale command.
  std::string cmd        = self->pending_scale_cmd_;
  float       cmdval     = self->pending_scale_cmd_value_;
  std::string cmdpayload = self->pending_scale_cmd_payload_;
  self->pending_scale_cmd_.clear();
  self->pending_scale_cmd_payload_.clear();
  // Tare handed to the scale: ack to the backend on the next weight push,
  // even if the offset value comes back unchanged (see tare_ack_pending_).
  if (cmd == "tare") self->tare_ack_pending_ = true;

  self->unlock_state();

  // Respond — include pending command if any.
  // Use std::string for write_tag so large NDEF hex payloads don't truncate.
  if (cmd == "tare") {
    ESP_LOGI(TAG, "Console: delivering tare command to scale via heartbeat");
    return send_json(req, "{\"ok\":true,\"cmd\":\"tare\"}");
  } else if (cmd == "calibrate") {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"cmd\":\"calibrate\",\"value\":%.2f}", cmdval);
    ESP_LOGI(TAG, "Console: delivering calibrate command to scale via heartbeat (ref=%.1f g)", cmdval);
    return send_json(req, buf);
  } else if (cmd == "set_calibration" && !cmdpayload.empty()) {
    std::string resp_str = std::string("{\"ok\":true,\"cmd\":\"set_calibration\",") + cmdpayload + "}";
    ESP_LOGI(TAG, "Console: delivering set_calibration command to scale via heartbeat (%s)",
             cmdpayload.c_str());
    return send_json(req, resp_str.c_str());
  } else if (cmd == "write_tag" && !cmdpayload.empty()) {
    std::string resp_str = std::string("{\"ok\":true,\"cmd\":\"write_tag\",") + cmdpayload + "}";
    ESP_LOGI(TAG, "Console: delivering write_tag command to scale via heartbeat (%zu payload bytes)",
             cmdpayload.size());
    return send_json(req, resp_str.c_str());
  }
  return send_json(req, "{\"ok\":true}");
}

esp_err_t BambuddyAPIComponent::console_http_scale_nfc_scanned(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  std::string json;
  if (!read_req_body(req, json)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_OK;
  }
  std::string uid      = parse_json_string(json, "uid");
  std::string tray_uuid = parse_json_string(json, "tray_uuid");
  int         sak      = parse_json_int(json, "sak", 0);
  std::string tag_type = parse_json_string(json, "tag_type");

  if (uid.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uid required");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Console received scale NFC tag-scanned: uid=%s", uid.c_str());

  self->lock_state();
  self->last_scale_push_ms_ = millis();
  self->unlock_state();

  // Route through the unified NFC path — identical to a locally-scanned tag.
  self->on_tag_scanned(uid, tray_uuid, sak, tag_type.empty() ? "unknown" : tag_type);
  // Override source AFTER on_tag_scanned() so write_tag commands are routed back
  // to the scale's PN532 rather than waiting on the console's reader.
  self->last_tag_source_ = TagSource::SCALE;

  return send_json(req, "{\"ok\":true}");
}

esp_err_t BambuddyAPIComponent::console_http_scale_nfc_removed(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  std::string json;
  if (!read_req_body(req, json)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_OK;
  }
  std::string uid = parse_json_string(json, "uid");

  ESP_LOGI(TAG, "Console received scale NFC tag-removed: uid=%s", uid.c_str());

  self->lock_state();
  self->last_scale_push_ms_ = millis();
  self->unlock_state();

  self->on_tag_removed(uid);

  return send_json(req, "{\"ok\":true}");
}

esp_err_t BambuddyAPIComponent::console_http_scale_nfc_write_result(httpd_req_t *req) {
  auto *self = static_cast<BambuddyAPIComponent *>(req->user_ctx);

  std::string json;
  if (!read_req_body(req, json)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_OK;
  }
  int         spool_id = parse_json_int(json, "spool_id", 0);
  std::string uid      = parse_json_string(json, "uid");
  bool        success  = parse_json_bool(json, "success", false);
  std::string msg      = parse_json_string(json, "message");

  ESP_LOGI(TAG, "Console received scale write-result: spool=%d uid=%s success=%d msg=%s",
           spool_id, uid.c_str(), (int)success, msg.c_str());

  // Relay to BamBuddy via the existing WRITE_RESULT path on the console's HTTP task.
  HttpJob job;
  job.kind = HttpJob::WRITE_RESULT;
  job.i1   = spool_id;
  job.s1   = uid;
  job.b1   = success;
  job.s2   = msg;
  self->enqueue_job(job);

  return send_json(req, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Scale HTTP server (scale_mode_ == true)
// ---------------------------------------------------------------------------

void BambuddyAPIComponent::start_scale_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port  = 80;
  cfg.stack_size   = 8192;
  cfg.max_uri_handlers = 8;

  httpd_handle_t server = nullptr;
  esp_err_t err = httpd_start(&server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return;
  }
  scale_server_handle_ = server;

  httpd_uri_t weight_uri = {"/weight",    HTTP_GET,  scale_http_weight,    this};
  httpd_uri_t tare_uri   = {"/tare",      HTTP_POST, scale_http_tare,      this};
  httpd_uri_t cal_uri    = {"/calibrate", HTTP_POST, scale_http_calibrate, this};

  httpd_register_uri_handler(server, &weight_uri);
  httpd_register_uri_handler(server, &tare_uri);
  httpd_register_uri_handler(server, &cal_uri);

  ESP_LOGI(TAG, "Scale HTTP server started on port %d (GET /weight, POST /tare, POST /calibrate)",
           cfg.server_port);
}

void BambuddyAPIComponent::restart_scale_server() {
  if (!scale_mode_) return;
  if (scale_server_handle_) {
    ESP_LOGI(TAG, "WiFi reconnect — restarting scale HTTP server");
    httpd_stop(scale_server_handle_);
    scale_server_handle_ = nullptr;
  }
  start_scale_server();
}

// ---------------------------------------------------------------------------
// NVS calibration persistence (scale server mode only)
// ---------------------------------------------------------------------------

static const char *NVS_NS      = "spoolbuddy";
static const char *NVS_TARE    = "tare_f";      // float bits (u32); old "tare_offset" (i32) abandoned
static const char *NVS_CAL     = "cal_factor";

void BambuddyAPIComponent::load_calibration_nvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "NVS namespace '%s' not found — using defaults", NVS_NS);
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
    return;
  }
  uint32_t tare_bits = 0;
  if (nvs_get_u32(h, NVS_TARE, &tare_bits) == ESP_OK) {
    float f; memcpy(&f, &tare_bits, sizeof(f));
    tare_offset_ = f;
  }
  uint32_t cal_bits = 0;
  if (nvs_get_u32(h, NVS_CAL, &cal_bits) == ESP_OK) {
    float f; memcpy(&f, &cal_bits, sizeof(f));
    if (f > 0.0f) calibration_factor_ = f;
  }
  nvs_close(h);
  ESP_LOGI(TAG, "Calibration loaded from NVS: tare=%.2f factor=%.6f",
           tare_offset_, calibration_factor_);
}

void BambuddyAPIComponent::save_calibration_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "NVS open (rw) failed");
    return;
  }
  uint32_t tare_bits; memcpy(&tare_bits, &tare_offset_, sizeof(tare_bits));
  nvs_set_u32(h, NVS_TARE, tare_bits);
  uint32_t cal_bits; memcpy(&cal_bits, &calibration_factor_, sizeof(cal_bits));
  nvs_set_u32(h, NVS_CAL, cal_bits);
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "Calibration saved to NVS: tare=%.2f factor=%.6f",
           tare_offset_, calibration_factor_);
}

void BambuddyAPIComponent::parse_ams_data(const std::string &json,
                                           std::vector<AMSUnit> &out) {
  out.clear();
  // Response from GET /api/v1/printers/{id}/status has a top-level "ams" array.
  // Each unit has an "id", optional "temp"/"humidity", and a "tray" array.
  // Tray fields: id (slot), tray_color (RRGGBBAA 8 chars), tray_type, state
  // (see the state legend at the presence check in the tray loop below).
  // top-level tray_now = currently fed tray global ID (ams_id*4+slot), 254=ext, 255=none
  size_t ap = json.find("\"ams\"");
  if (ap == std::string::npos) return;
  ap = json.find('[', ap);
  if (ap == std::string::npos) return;
  size_t ae = ap;
  int d = 0;
  while (ae < json.size()) {
    if (json[ae] == '[') d++;
    else if (json[ae] == ']' && --d == 0) break;
    ae++;
  }
  std::string ams_json = json.substr(ap, ae - ap + 1);
  for (const auto &uobj : json_array_objects(ams_json)) {
    AMSUnit unit;
    unit.id = parse_json_int(uobj, "id", 0);
    unit.temp = parse_json_float(uobj, "temp", 0.0f);
    unit.humidity = parse_json_int(uobj, "humidity", 0);
    // Nozzle assignment is NOT in the per-unit JSON object; it comes from the
    // top-level ams_extruder_map dict parsed in api_get_ams() after this call.
    // Default to -1 (unknown); api_get_ams() will overwrite it.
    unit.nozzle = -1;
    // Derive a display name. is_ams_ht marks the single-slot HT dryer.
    unit.is_ht = parse_json_bool(uobj, "is_ams_ht", false);
    if (unit.is_ht) {
      unit.name = "AMS HT";
    } else {
      unit.name = "AMS " + std::to_string(unit.id + 1);
    }
    // Find "tray" array within this unit
    size_t tp = uobj.find("\"tray\"");
    if (tp != std::string::npos) {
      tp = uobj.find('[', tp);
      if (tp != std::string::npos) {
        size_t te = tp;
        int td = 0;
        while (te < uobj.size()) {
          if (uobj[te] == '[') td++;
          else if (uobj[te] == ']' && --td == 0) break;
          te++;
        }
        std::string trays_json = uobj.substr(tp, te - tp + 1);
        for (const auto &tobj : json_array_objects(trays_json)) {
          AMSTray tray;
          tray.slot          = parse_json_int(tobj, "id", 0);
          int state          = parse_json_int(tobj, "state", 0);
          tray.material_type = parse_json_string(tobj, "tray_type");
          // tray_color is RRGGBBAA — take first 6 chars as RGB hex
          std::string color8 = parse_json_string(tobj, "tray_color");
          if (color8.size() >= 6)
            tray.color_hex = color8.substr(0, 6);
          tray.nozzle_temp_min = parse_json_int(tobj, "nozzle_temp_min", 0);
          tray.nozzle_temp_max = parse_json_int(tobj, "nozzle_temp_max", 0);
          tray.spool_id        = parse_json_int(tobj, "spool_id", 0);
          // Bambu AMS tray states — rather than maintaining an allowlist of
          // "present" values, exclude the known "no spool" states and treat
          // everything else as occupied:
          //   0  = empty slot
          //   9  = empty slot (alternate empty state observed in firmware)
          //  11  = loaded / RFID recognised (Bambu-brand spool)
          //  27  = present / staged (spool seated but not the active filament,
          //        or third-party spool without full RFID data)
          // Additional transient states (reading, unloading, …) also indicate
          // a spool is physically present. Fall back to checking material data
          // so any undocumented state with populated fields is caught too.
          bool state_present = (state != 0 && state != 9);
          bool data_present  = !tray.material_type.empty() &&
                               !color8.empty() && color8 != "00000000";
          tray.present = state_present || data_present;
          unit.trays.push_back(tray);
        }
      }
    }
    // Pad to the unit's slot count (HT has a single slot) so the UI can index
    // without bounds checks.
    int want_slots = unit.is_ht ? 1 : 4;
    while ((int)unit.trays.size() < want_slots) {
      AMSTray empty;
      empty.slot = (int)unit.trays.size();
      unit.trays.push_back(empty);
    }
    out.push_back(unit);
  }

  // Parse top-level "vt_tray" array — external/bypass spool holders.
  // Each entry is a single spool slot (not a 4-slot AMS), identified by
  // id 254/255 in Bambu firmware. Named "Ext 1", "Ext 2", …
  {
    size_t vp = json.find("\"vt_tray\"");
    if (vp != std::string::npos) {
      vp = json.find('[', vp);
      if (vp != std::string::npos) {
        size_t ve = vp;
        int vd = 0;
        while (ve < json.size()) {
          if (json[ve] == '[') vd++;
          else if (json[ve] == ']' && --vd == 0) break;
          ve++;
        }
        std::string vt_json = json.substr(vp, ve - vp + 1);
        int vt_idx = 0;
        for (const auto &tobj : json_array_objects(vt_json)) {
          AMSUnit unit;
          unit.is_vt  = true;
          unit.id     = parse_json_int(tobj, "id", 254 + vt_idx);
          unit.nozzle = -1;   // no fixed nozzle assignment for external trays
          unit.name   = "Ext " + std::to_string(vt_idx + 1);
          unit.temp   = 0.0f;
          unit.humidity = 0;

          AMSTray tray;
          tray.slot = 0;
          std::string tray_type = parse_json_string(tobj, "tray_type");
          std::string color8    = parse_json_string(tobj, "tray_color");
          tray.material_type    = tray_type;
          if (color8.size() >= 6)
            tray.color_hex = color8.substr(0, 6);
          tray.nozzle_temp_min  = parse_json_int(tobj, "nozzle_temp_min", 0);
          tray.nozzle_temp_max  = parse_json_int(tobj, "nozzle_temp_max", 0);
          tray.spool_id         = parse_json_int(tobj, "spool_id", 0);
          // Consider the slot occupied when a material type is reported and the
          // colour is not the all-zero placeholder "00000000".
          tray.present = !tray_type.empty() &&
                         !color8.empty() && color8 != "00000000";
          unit.trays.push_back(tray);
          out.push_back(unit);
          vt_idx++;
        }
      }
    }
  }

  // Enforce a stable display order regardless of how the printer reports its
  // units: regular AMS first, then the AMS HT dryer, then external/VT spools.
  // The "ams" JSON array can interleave the HT unit among the regular AMS
  // units; this pins it after them.  stable_sort keeps the existing relative
  // order within each category (AMS 1, AMS 2, …; Ext 1, Ext 2, …).
  auto order_key = [](const AMSUnit &u) -> int {
    if (u.is_vt) return 2;  // external / bypass spools last
    if (u.is_ht) return 1;  // AMS HT dryer in the middle
    return 0;               // regular AMS units first
  };
  std::stable_sort(out.begin(), out.end(),
                   [&](const AMSUnit &a, const AMSUnit &b) {
                     return order_key(a) < order_key(b);
                   });
}

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

bool BambuddyAPIComponent::is_network_ready() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return false;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;
  return ip_info.ip.addr != 0;
}

void BambuddyAPIComponent::ensure_device_id() {
  if (!device_id_.empty()) return;
  // Derive from WiFi MAC address
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  char id[32];
  snprintf(id, sizeof(id), "sb-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  device_id_ = id;
  ESP_LOGI(TAG, "Device ID derived from MAC: %s", device_id_.c_str());
}

std::string BambuddyAPIComponent::get_ip_address() {
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    return std::string(buf);
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external library dependency)
// ---------------------------------------------------------------------------

std::string BambuddyAPIComponent::json_string(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
          out += esc;
        } else {
          out += c;
        }
    }
  }
  out += "\"";
  return out;
}

// Flat key-based JSON value extractor: finds the first occurrence of "key"
// anywhere in the input (including nested objects) and returns its raw value.
static std::string extract_value(const std::string &json,
                                 const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";
  pos += needle.size();
  // skip whitespace and colon
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    pos++;
  if (pos >= json.size()) return "";

  if (json[pos] == '"') {
    // string value
    pos++;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\') pos++;  // skip escape prefix
      if (pos < json.size()) val += json[pos++];
    }
    return val;
  } else if (json[pos] == 't' || json[pos] == 'f') {
    // boolean
    return (json[pos] == 't') ? "true" : "false";
  } else if (json[pos] == 'n') {
    return "null";
  } else {
    // number
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != ' ')
      end++;
    return json.substr(pos, end - pos);
  }
}

std::string BambuddyAPIComponent::parse_json_string(const std::string &json,
                                                     const std::string &key) {
  std::string val = extract_value(json, key);
  if (val == "null") return "";
  return val;
}

bool BambuddyAPIComponent::parse_json_bool(const std::string &json,
                                            const std::string &key,
                                            bool default_val) {
  std::string val = extract_value(json, key);
  if (val == "true") return true;
  if (val == "false") return false;
  return default_val;
}

float BambuddyAPIComponent::parse_json_float(const std::string &json,
                                              const std::string &key,
                                              float default_val) {
  std::string val = extract_value(json, key);
  if (val.empty() || val == "null") return default_val;
  return strtof(val.c_str(), nullptr);
}

int BambuddyAPIComponent::parse_json_int(const std::string &json,
                                          const std::string &key,
                                          int default_val) {
  std::string val = extract_value(json, key);
  if (val.empty() || val == "null") return default_val;
  return (int)strtol(val.c_str(), nullptr, 10);
}

// ---------------------------------------------------------------------------
// NFC picker — fetch recent spools, link/unlink tag, create spool from tag
// ---------------------------------------------------------------------------

// The inventory list can be large, so stream the response instead of buffering
// it whole: read the body in chunks, brace-match one top-level spool object at a
// time, and keep only the 9 highest ids. Peak memory is one object plus the 9
// kept summaries, regardless of how many spools the backend returns.
void BambuddyAPIComponent::api_get_recent_spools() {
  auto finish = [&](std::vector<SpoolSummary> &&result) {
    std::sort(result.begin(), result.end(),
              [](const SpoolSummary &a, const SpoolSummary &b) { return a.id > b.id; });
    ESP_LOGI(TAG, "api_get_recent_spools: %d spool(s) (streamed)", (int)result.size());
    lock_state();
    display_state_.recent_spools         = std::move(result);
    display_state_.recent_spools_loading = false;
    display_state_.recent_spools_generation++;
    unlock_state();
  };

  std::string url = backend_url_for("/api/v1",
      spoolman_inventory_ ? "/spoolman/inventory/spools" : "/inventory/spools");
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 8000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.transport_type = HTTP_TRANSPORT_UNKNOWN;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) { finish({}); return; }
  if (!api_key_.empty())
    esp_http_client_set_header(client, "X-API-Key", api_key_.c_str());

  arch_feed_wdt();
  esp_err_t err = esp_http_client_open(client, 0);  // 0 = no request body
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "api_get_recent_spools: open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    finish({});
    return;
  }
  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "api_get_recent_spools: HTTP %d", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    finish({});
    return;
  }

  // Insert into the kept set, replacing the lowest id once we hold 9.
  std::vector<SpoolSummary> top;
  auto consider = [&](const std::string &obj) {
    int id = parse_json_int(obj, "id", 0);
    if (id <= 0) return;
    if (!parse_json_string(obj, "tag_uid").empty()) return;  // skip tagged spools
    SpoolSummary ss;
    ss.id       = id;
    ss.material = parse_json_string(obj, "material");
    ss.brand    = parse_json_string(obj, "brand");
    std::string rgba = parse_json_string(obj, "rgba");
    ss.color_hex = (rgba.size() >= 6) ? rgba.substr(0, 6) : "";
    if (top.size() < 9) { top.push_back(std::move(ss)); return; }
    size_t lo = 0;
    for (size_t k = 1; k < top.size(); k++)
      if (top[k].id < top[lo].id) lo = k;
    if (ss.id > top[lo].id) top[lo] = std::move(ss);
  };

  // Streaming brace matcher: capture each top-level {...} object, ignoring the
  // enclosing array and any braces inside JSON strings.
  std::string obj;
  int  depth = 0;
  bool in_str = false, esc = false, capturing = false;
  char buf[512];
  int r;
  while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
    arch_feed_wdt();
    for (int i = 0; i < r; i++) {
      char c = buf[i];
      if (in_str) {
        if (capturing) obj.push_back(c);
        if (esc)            esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"')  in_str = false;
        continue;
      }
      if (c == '"') { in_str = true; if (capturing) obj.push_back(c); continue; }
      if (c == '{') {
        if (depth == 0) { capturing = true; obj.clear(); }
        depth++; obj.push_back(c); continue;
      }
      if (c == '}') {
        if (depth > 0) depth--;
        obj.push_back(c);
        if (depth == 0 && capturing) { capturing = false; consider(obj); obj.clear(); }
        continue;
      }
      if (capturing) obj.push_back(c);
    }
    // Guard against a malformed (never-closing) object growing without bound.
    if (capturing && obj.size() > 8192) {
      capturing = false; obj.clear(); depth = 0; in_str = false; esc = false;
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  finish(std::move(top));
}

void BambuddyAPIComponent::record_scale_weight(int spool_id, float total_grams) {
  if (spool_id <= 0) {
    ESP_LOGW(TAG, "record_scale_weight: invalid spool_id %d", spool_id);
    return;
  }
  // Optimistic local display update using the same formula as the PATCH.
  lock_state();
  if (display_state_.current_filament.spool_id == spool_id) {
    auto &fi = display_state_.current_filament;
    float remaining = total_grams - fi.core_weight_g;
    float used = (fi.label_weight_g > 0.0f)
                 ? std::max(0.0f, fi.label_weight_g - remaining)
                 : 0.0f;
    fi.weight_used_g = used;
  }
  unlock_state();
  HttpJob job;
  job.kind = HttpJob::UPDATE_SPOOL_WEIGHT;
  job.i1 = spool_id;
  job.f1 = total_grams;
  enqueue_job(job);
}

void BambuddyAPIComponent::api_update_spool_weight(int spool_id, float total_grams) {
  // POST /scale/update-spool-weight: send the raw scale reading (filament + spool
  // body). BamBuddy subtracts core_weight from its database and routes the update
  // to the correct backend (local DB or Spoolman) based on site configuration.
  int weight_int = (int)lroundf(total_grams);
  char body[64];
  snprintf(body, sizeof(body),
           "{\"spool_id\":%d,\"weight_grams\":%d}", spool_id, weight_int);
  std::string resp;
  bool ok = http_post("/scale/update-spool-weight", body, resp);
  if (ok) {
    ESP_LOGI(TAG, "api_update_spool_weight spool %d: %d g OK", spool_id, weight_int);
    set_status("Weight saved");
    // Refresh spool detail; check_empty=true triggers archive proposal if 0g left.
    // Optimistic update in record_scale_weight handles the display until this returns.
    api_get_spool(spool_id, /* check_empty= */ true);
  } else {
    ESP_LOGW(TAG, "api_update_spool_weight spool %d: POST failed", spool_id);
    set_status("Weight save failed");
  }
}

void BambuddyAPIComponent::api_link_tag(int spool_id, const std::string &uid,
                                        const std::string &tray_uuid,
                                        const std::string &tag_type) {
  // uid="" means unlink; otherwise link the tag.
  bool is_unlink = uid.empty();
  std::ostringstream js;
  char path[64];
  if (spoolman_inventory_) {
    if (is_unlink) {
      // The dedicated .../tag endpoint requires tag_uid length >= 8, so it
      // cannot express "clear the tag". Use the general spool PATCH instead.
      js << "{\"tag_uid\":null}";
      snprintf(path, sizeof(path), "/spoolman/inventory/spools/%d", spool_id);
    } else {
      js << "{\"tag_uid\":"   << json_string(uid)
         << ",\"tray_uuid\":" << json_string(tray_uuid) << "}";
      snprintf(path, sizeof(path), "/spoolman/inventory/spools/%d/tag", spool_id);
    }
  } else {
    js << "{\"tag_uid\":"    << json_string(uid)
       << ",\"tray_uuid\":"  << json_string(tray_uuid)
       << ",\"tag_type\":"   << json_string(tag_type)
       << ",\"data_origin\":\"espoolbuddy\"}";
    snprintf(path, sizeof(path), "/inventory/spools/%d/link-tag", spool_id);
  }
  std::string resp;
  bool ok = http_patch_api(path, js.str(), resp);
  if (!ok) {
    ESP_LOGW(TAG, "api_link_tag spool %d %s: failed", spool_id,
             is_unlink ? "unlink" : "link");
    set_status(is_unlink ? "Unlink failed" : "Tag link failed");
    return;
  }
  ESP_LOGI(TAG, "api_link_tag spool %d %s: ok", spool_id,
           is_unlink ? "unlink" : "link");
  if (is_unlink) {
    // Clear everything and go back to idle.
    lock_state();
    display_state_.spool_selected   = false;
    display_state_.current_filament = FilamentInfo{};
    display_state_.status_message   = "Tag unlinked";
    display_state_.nfc_state        = NFCTagState::ABSENT;
    display_state_.spool_assign_expiry_ms = 0;
    unlinked_tag_expiry_ms_ = 0;
    display_state_.unlinked_tag_expiry_ms = 0;
    pending_assign_spool_id_ = 0;
    unlock_state();
  } else {
    // Arm the pending-assign TTL for AMS slot auto-assignment.
    // Seed the spool immediately so the NFC tab stays on the spool view;
    // api_get_spool (called below) fills in the full inventory detail.
    lock_state();
    pending_assign_spool_id_  = spool_id;
    pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
    display_state_.spool_assign_expiry_ms = pending_assign_expiry_ms_;
    unlinked_tag_expiry_ms_   = 0;
    display_state_.unlinked_tag_expiry_ms = 0;
    FilamentInfo seed{};
    seed.spool_id = spool_id;
    display_state_.current_filament = seed;
    display_state_.spool_selected   = true;
    display_state_.status_message   = "Tag linked to spool #" + std::to_string(spool_id);
    unlock_state();
    api_get_spool(spool_id);
  }
}

void BambuddyAPIComponent::api_create_spool_from_tag(const std::string &uid) {
  std::string resp;
  bool ok;
  if (spoolman_inventory_) {
    // Spoolman's create-spool body has no tag field, so create first, then
    // PATCH .../tag on the new spool to link it (two sequential blocking
    // calls on the HTTP task — same pattern used elsewhere for chained jobs).
    std::string create_js = "{\"material\":\"PLA\",\"label_weight\":1000,"
                             "\"note\":\"Created by ESPoolBuddy\"}";
    ok = http_post_api("/spoolman/inventory/spools", create_js, resp);
  } else {
    std::string js = "{\"material\":\"PLA\",\"label_weight\":1000,"
                     "\"tag_uid\":" + json_string(uid) + ","
                     "\"note\":\"Created by ESPoolBuddy\","
                     "\"data_origin\":\"spoolbuddy\"}";
    ok = http_post_api("/inventory/spools", js, resp);
  }
  if (!ok) {
    ESP_LOGW(TAG, "api_create_spool_from_tag: failed");
    set_status("Create spool failed");
    return;
  }
  int new_id = parse_json_int(resp, "id", 0);
  if (new_id <= 0) {
    ESP_LOGW(TAG, "api_create_spool_from_tag: no id in response");
    set_status("Create spool failed (no id)");
    return;
  }
  if (spoolman_inventory_) {
    char tag_path[64];
    snprintf(tag_path, sizeof(tag_path), "/spoolman/inventory/spools/%d/tag", new_id);
    std::string tag_js = "{\"tag_uid\":" + json_string(uid) + "}";
    std::string tag_resp;
    if (!http_patch_api(tag_path, tag_js, tag_resp)) {
      ESP_LOGW(TAG, "api_create_spool_from_tag: created #%d but tag link failed", new_id);
      set_status("Spool created, tag link failed");
    }
  }
  ESP_LOGI(TAG, "api_create_spool_from_tag: created spool #%d", new_id);

  // Arm the pending-assign TTL for AMS slot auto-assignment, then clear the
  // view so the user returns to "Waiting for tag…".
  lock_state();
  pending_assign_spool_id_  = new_id;
  pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
  display_state_.spool_assign_expiry_ms = pending_assign_expiry_ms_;
  unlinked_tag_expiry_ms_   = 0;
  display_state_.unlinked_tag_expiry_ms = 0;
  display_state_.nfc_state  = NFCTagState::ABSENT;
  display_state_.spool_selected   = false;
  display_state_.current_filament = FilamentInfo{};
  display_state_.status_message   = "Created spool #" + std::to_string(new_id);
  unlock_state();
}

// ---- Public method implementations ----

void BambuddyAPIComponent::request_recent_spools() {
  lock_state();
  display_state_.recent_spools_loading = true;
  display_state_.recent_spools.clear();
  unlock_state();
  HttpJob job;
  job.kind = HttpJob::FETCH_RECENT_SPOOLS;
  enqueue_job(job);
}

void BambuddyAPIComponent::link_tag_to_spool(int spool_id) {
  std::string uid, tray_uuid, tag_type;
  lock_state();
  uid        = display_state_.last_tag_uid;
  tray_uuid  = display_state_.current_filament.tray_uuid;
  tag_type   = display_state_.current_filament.tag_type;
  unlock_state();
  HttpJob job;
  job.kind = HttpJob::LINK_TAG_TO_SPOOL;
  job.i1   = spool_id;
  job.s1   = uid;        // non-empty = link
  job.s2   = tray_uuid;
  job.s3   = tag_type;
  enqueue_job(job);
}

void BambuddyAPIComponent::unlink_current_tag() {
  int spool_id;
  lock_state();
  spool_id = display_state_.current_filament.spool_id;
  unlock_state();
  if (spool_id <= 0) return;
  HttpJob job;
  job.kind = HttpJob::LINK_TAG_TO_SPOOL;
  job.i1   = spool_id;
  job.s1   = "";    // empty = unlink
  enqueue_job(job);
}

void BambuddyAPIComponent::create_spool_from_tag() {
  std::string uid;
  lock_state();
  uid = display_state_.last_tag_uid;
  unlock_state();
  HttpJob job;
  job.kind = HttpJob::CREATE_SPOOL_FROM_TAG;
  job.s1   = uid;
  enqueue_job(job);
}

void BambuddyAPIComponent::archive_spool(int spool_id) {
  if (spool_id <= 0) return;
  lock_state();
  display_state_.propose_archive = false;
  unlock_state();
  HttpJob job;
  job.kind = HttpJob::ARCHIVE_SPOOL;
  job.i1   = spool_id;
  enqueue_job(job);
}

void BambuddyAPIComponent::api_archive_spool(int spool_id) {
  char path[64];
  snprintf(path, sizeof(path),
           spoolman_inventory_ ? "/spoolman/inventory/spools/%d/archive"
                                : "/inventory/spools/%d/archive",
           spool_id);
  std::string resp;
  bool ok = http_post_api(path, "", resp);
  if (ok) {
    ESP_LOGI(TAG, "api_archive_spool %d: ok", spool_id);
    set_status("Spool archived");
    lock_state();
    display_state_.spool_selected   = false;
    display_state_.current_filament = FilamentInfo{};
    display_state_.nfc_state        = NFCTagState::ABSENT;
    display_state_.unlinked_tag_expiry_ms = 0;
    unlinked_tag_expiry_ms_ = 0;
    unlock_state();
  } else {
    ESP_LOGW(TAG, "api_archive_spool %d: failed", spool_id);
    set_status("Archive failed");
  }
}

}  // namespace bambuddy_api
}  // namespace esphome
