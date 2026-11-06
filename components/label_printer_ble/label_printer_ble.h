#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/label_printer/transport.h"

#include <vector>

namespace esphome::label_printer_ble {

// A label printer over Bluetooth LE (GATT), for any driver with a
// BleProfile. Bluetooth is switched on for a print and off after it, because
// the stack needs about 58 KB of internal RAM while it runs.
//
// Without a configured address (the ble_client's mac_address left at
// 00:00:00:00:00:00) the first print looks for the printer by the driver's
// advertisement rule. Once a connection shows the printer's
// characteristics, the address is kept in flash; it's forgotten again if the
// printer can't be reached, so a replaced printer is found on the print after.
class LabelPrinterBLE : public Component,
                        public ble_client::BLEClientNode,
                        public esp32_ble_tracker::ESPBTDeviceListener,
                        public label_printer::LabelTransport {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;

  void open() override;
  bool ready() const override { return this->ready_; }
  bool searching() const override { return this->discovering_; }
  bool has_replies() const override { return this->status_handle_ != 0; }
  void send() override;
  SendState send_state() const override { return this->send_state_; }
  void close(bool unreachable) override;

 protected:
  void bluetooth_off_();
  void remember_(uint64_t address);
  void forget_();
  void write_next_();

  bool opened_{false};
  bool we_enabled_ble_{false};
  uint32_t ble_off_at_{0};

  // Finding the printer.
  bool configured_{false};    // mac_address set in the config: never search
  bool discovering_{false};   // waiting for a matching advertisement
  bool found_new_{false};     // connecting to an address found this print, not yet confirmed
  uint64_t rejected_{0};      // matched but had no printer characteristics
  ESPPreferenceObject pref_;

  // Connection.
  bool ready_{false};  // characteristics found and notifications registered
  uint16_t write_handle_{0};
  uint16_t status_handle_{0};
  uint16_t mtu_{23};

  // Sending.
  SendState send_state_{SendState::IDLE};
  std::vector<uint8_t> chunk_;  // the write being sent (kept for retries)
  size_t chunk_len_{0};
  bool in_flight_{false};       // the write awaits its event
  uint32_t in_flight_ms_{0};
  uint32_t last_write_ms_{0};
  bool congested_{false};
  bool write_events_{true};     // the stack reports each no-response write
  uint16_t retries_{0};
};

}  // namespace esphome::label_printer_ble

#endif  // USE_ESP32
