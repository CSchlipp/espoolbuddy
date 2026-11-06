#include "label_printer_ble.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble/ble.h"

#include <esp_gattc_api.h>

#include <algorithm>

namespace esphome::label_printer_ble {

static const char *const TAG = "label_printer_ble";

using SendState = label_printer::LabelTransport::SendState;

static const uint16_t WRITE_RETRIES = 40;
static const uint32_t WRITE_EVENT_WAIT_MS = 250;
static const uint32_t BLE_OFF_DELAY_MS = 500;

void LabelPrinterBLE::setup() {
  if (esp32_ble::global_ble == nullptr) {
    ESP_LOGE(TAG, "esp32_ble is missing");
    this->mark_failed();
    return;
  }
  this->configured_ = this->parent()->get_address() != 0;
  if (this->configured_) return;
  this->pref_ = global_preferences->make_preference<uint64_t>(fnv1_hash("label_printer_ble_address"));
  uint64_t saved = 0;
  if (this->pref_.load(&saved) && saved != 0) this->parent()->set_address(saved);
}

void LabelPrinterBLE::dump_config() {
  ESP_LOGCONFIG(TAG, "Label printer over Bluetooth:\n  Printer: %s%s",
                this->parent()->get_address() != 0 ? this->parent()->address_str() : "not found yet",
                this->configured_ ? "" : " (found automatically)");
}

void LabelPrinterBLE::remember_(uint64_t address) {
  if (this->configured_) return;
  this->pref_.save(&address);
  global_preferences->sync();
}

void LabelPrinterBLE::forget_() {
  this->parent()->set_enabled(false);
  this->parent()->set_address(0);
  this->remember_(0);
}

bool LabelPrinterBLE::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (!this->discovering_ || this->driver_ == nullptr) return false;
  const label_printer::BleProfile *profile = this->driver_->ble_profile();
  if (profile == nullptr || profile->advertised_service == 0) return false;
  const uint64_t addr = device.address_uint64();
  if (addr == this->rejected_) return false;
  bool has_uuid = false;
  for (const auto &u : device.get_service_uuids()) {
    if (u == ble_device_base::ESPBTUUID::from_uint16(profile->advertised_service)) has_uuid = true;
  }
  if (!has_uuid) return false;
  if (profile->address_in_manufacturer_data) {
    bool own_address = false;
    for (const auto &m : device.get_manufacturer_datas()) {
      if (m.data.size() != 6) continue;
      own_address = true;
      for (int i = 0; i < 6; i++) {
        if (m.data[i] != (uint8_t) (addr >> (40 - 8 * i))) own_address = false;
      }
      if (own_address) break;
    }
    if (!own_address) return false;
  }
  char ab[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  auto name = device.get_name();
  ESP_LOGI(TAG, "Found a printer at %s (RSSI %d%s%.*s)", device.address_str_to(ab), device.get_rssi(),
           name.empty() ? "" : ", ", (int) name.size(), name.empty() ? "" : name.c_str());
  this->discovering_ = false;
  this->found_new_ = true;
  this->parent()->set_address(addr);
  this->parent()->set_enabled(true);
  return false;
}

void LabelPrinterBLE::open() {
  this->opened_ = true;
  this->found_new_ = false;
  this->rejected_ = 0;
  this->send_state_ = SendState::IDLE;
  if (this->parent()->get_address() != 0) {
    this->discovering_ = false;
    this->parent()->set_enabled(true);
  } else {
    ESP_LOGI(TAG, "Looking for the printer");
    this->discovering_ = true;  // parse_device() enables the client once it's found
  }
  this->ble_off_at_ = 0;
  if (!esp32_ble::global_ble->is_active()) {
    esp32_ble::global_ble->enable();
    this->we_enabled_ble_ = true;
  }
}

void LabelPrinterBLE::close(bool unreachable) {
  // A found or remembered address that didn't answer is forgotten, so the
  // next print looks again (a replaced printer, or a wrong match).
  if (unreachable && !this->configured_ && this->parent()->get_address() != 0) {
    ESP_LOGW(TAG, "No answer from %s; will look for the printer next time", this->parent()->address_str());
    this->forget_();
  }
  this->opened_ = false;
  this->discovering_ = false;
  this->found_new_ = false;
  this->send_state_ = SendState::IDLE;
  this->in_flight_ = false;
  std::vector<uint8_t>().swap(this->chunk_);
  this->chunk_len_ = 0;
  // Start the next print from a clean connection, even if this one's close
  // event is lost when Bluetooth is switched off.
  this->ready_ = false;
  this->write_handle_ = 0;
  this->status_handle_ = 0;
  this->mtu_ = 23;
  this->congested_ = false;
  this->write_events_ = true;
  this->bluetooth_off_();
}

void LabelPrinterBLE::bluetooth_off_() {
  // Stop the client reconnecting to the printer, which would otherwise hold
  // it (and the phone app off it) until Bluetooth is switched off.
  this->parent()->set_enabled(false);
  // Leave Bluetooth alone if something else turned it on.
  if (!this->we_enabled_ble_) return;
  this->ble_off_at_ = millis() + BLE_OFF_DELAY_MS;
}

void LabelPrinterBLE::send() {
  const label_printer::PrinterInfo &info = this->driver_->info();
  this->chunk_.assign(info.max_write, 0);
  this->chunk_len_ = 0;
  this->in_flight_ = false;
  this->retries_ = 0;
  this->congested_ = false;
  this->last_write_ms_ = 0;
  this->send_state_ = SendState::SENDING;
  ESP_LOGD(TAG, "MTU %u", this->mtu_);
}

void LabelPrinterBLE::write_next_() {
  if (this->chunk_len_ == 0) {
    const size_t room = std::min<size_t>(this->chunk_.size(), this->mtu_ > 3 ? this->mtu_ - 3 : 20);
    this->chunk_len_ = this->driver_->next(this->chunk_.data(), room);
    if (this->chunk_len_ == 0) {
      this->send_state_ = SendState::DONE;
      return;
    }
  }
  esp_err_t err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                           this->write_handle_, this->chunk_len_, this->chunk_.data(),
                                           ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  this->last_write_ms_ = millis();
  if (err != ESP_OK) {
    // The queue is full: try the same bytes again after the gap.
    if (++this->retries_ > WRITE_RETRIES) this->send_state_ = SendState::FAILED;
    return;
  }
  if (this->write_events_) {
    this->in_flight_ = true;
    this->in_flight_ms_ = this->last_write_ms_;
  } else {
    this->chunk_len_ = 0;
    this->retries_ = 0;
  }
}

void LabelPrinterBLE::loop() {
  const uint32_t now = millis();
  if (this->ble_off_at_ != 0 && (int32_t) (now - this->ble_off_at_) >= 0) {
    this->ble_off_at_ = 0;
    if (!this->opened_ && this->we_enabled_ble_) {
      esp32_ble::global_ble->disable();
      this->we_enabled_ble_ = false;
    }
  }
  if (this->send_state_ != SendState::SENDING) return;
  if (this->in_flight_) {
    if (now - this->in_flight_ms_ < WRITE_EVENT_WAIT_MS) return;
    // No event for a no-response write: this stack doesn't send them.
    ESP_LOGD(TAG, "No write events; pacing by time only");
    this->write_events_ = false;
    this->in_flight_ = false;
    this->chunk_len_ = 0;
    this->retries_ = 0;
  }
  if (this->congested_ || now - this->last_write_ms_ < this->driver_->info().write_gap_ms) return;
  this->write_next_();
}

void LabelPrinterBLE::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) this->mtu_ = param->cfg_mtu.mtu;
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      const label_printer::BleProfile *profile = this->driver_ != nullptr ? this->driver_->ble_profile() : nullptr;
      auto *wr = profile != nullptr ? this->parent()->get_characteristic(profile->service, profile->write_char)
                                    : nullptr;
      if (wr == nullptr) {
        ESP_LOGW(TAG, "%s doesn't have the printer's characteristics", this->parent()->address_str());
        if (!this->configured_ && this->opened_ && this->send_state_ == SendState::IDLE) {
          // Not the printer after all: forget it and keep looking.
          this->rejected_ = this->parent()->get_address();
          this->found_new_ = false;
          this->forget_();
          this->discovering_ = true;
        } else {
          this->parent()->disconnect();
        }
        break;
      }
      this->write_handle_ = wr->handle;
      auto *st = profile->notify_char != 0 ? this->parent()->get_characteristic(profile->service, profile->notify_char)
                                           : nullptr;
      this->status_handle_ = st != nullptr ? st->handle : 0;
      if (st != nullptr && this->parent()->register_for_notify(st->handle) == ESP_OK) break;
      // No status channel: print without the "printed" confirmation.
      this->status_handle_ = 0;
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      this->ready_ = true;
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle == this->status_handle_) {
        if (param->reg_for_notify.status != ESP_GATT_OK) this->status_handle_ = 0;
        this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
        this->ready_ = true;
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (this->send_state_ != SendState::SENDING || !this->in_flight_) break;
      this->in_flight_ = false;
      if (param->write.status == ESP_GATT_OK) {
        this->chunk_len_ = 0;
        this->retries_ = 0;  // the limit is per write, not per label
      } else if (++this->retries_ > WRITE_RETRIES) {
        this->send_state_ = SendState::FAILED;
      }
      break;

    case ESP_GATTC_CONGEST_EVT:
      this->congested_ = param->congest.congested;
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle != this->status_handle_ || this->reply_ == nullptr) break;
      this->reply_->on_reply(param->notify.value, param->notify.value_len);
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->ready_ = false;
      this->write_handle_ = 0;
      this->congested_ = false;
      this->in_flight_ = false;
      this->mtu_ = 23;
      if (this->send_state_ == SendState::SENDING) this->send_state_ = SendState::LOST;
      break;

    default:
      break;
  }
  if (this->ready_ && this->found_new_) {
    ESP_LOGI(TAG, "Remembering the printer at %s", this->parent()->address_str());
    this->remember_(this->parent()->get_address());
    this->found_new_ = false;
  }
}

}  // namespace esphome::label_printer_ble

#endif  // USE_ESP32
