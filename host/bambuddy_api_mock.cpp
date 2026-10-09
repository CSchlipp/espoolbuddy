#include "esphome/components/bambuddy_api/bambuddy_api.h"

// Mock implementation of bambuddy_api for the ESPHome host platform. Not part
// of the component itself: host/espoolbuddy_console_host.yaml pulls it into
// the build via `esphome: includes:`, while the component's own
// bambuddy_api.cpp compiles to nothing off ESP32. There is no backend, no NFC reader and no
// scale: setup() fills display_state_ with demo printers/AMS/spools so the
// LVGL UI can be exercised in an SDL window on the desktop, and the UI-facing
// actions update that state locally the way a successful backend round trip
// would. Everything runs on the main loop, so no locking is needed
// (state_mutex_ stays null and lock_state()/unlock_state() are no-ops).
#ifdef USE_HOST
#include "esphome/core/hal.h"

namespace esphome {
namespace bambuddy_api {

namespace {

struct DemoSpool {
  int id;
  const char *material;
  const char *subtype;
  const char *brand;
  const char *color_hex;
  const char *color_name;
  float label_weight_g;
  float remaining_g;
};

const DemoSpool DEMO_SPOOLS[] = {
    {42, "PLA", "Matte", "Bambu Lab", "1A1A1A", "Charcoal", 1000, 640},
    {41, "PETG", "HF", "Bambu Lab", "F5F5F5", "White", 1000, 910},
    {40, "PLA", "Basic", "Bambu Lab", "C12E1F", "Red", 1000, 120},
    {39, "PLA", "Silk", "eSUN", "D4AF37", "Gold", 1000, 455},
    {38, "ABS", "", "Polymaker", "0A4F9E", "Blue", 1000, 280},
    {37, "TPU", "95A", "Bambu Lab", "FF8C00", "Orange", 500, 300},
    {36, "PLA", "Plus", "eSUN", "F0E6D2", "Bone White", 1000, 1000},
    {35, "ASA", "", "Bambu Lab", "4B5320", "Army Green", 1000, 760},
    {34, "PETG", "Translucent", "Sunlu", "7FDBFF", "Ice Blue", 1000, 50},
};

const DemoSpool *find_demo_spool(int id) {
  for (const auto &s : DEMO_SPOOLS)
    if (s.id == id) return &s;
  return nullptr;
}

AMSTray demo_tray(int slot, const DemoSpool *s) {
  AMSTray t;
  t.slot = slot;
  if (s == nullptr) return t;  // empty slot
  t.present = true;
  t.material_type = s->material;
  t.color_hex = s->color_hex;
  t.nozzle_temp_min = 190;
  t.nozzle_temp_max = 240;
  t.spool_id = s->id;
  t.label_weight_g = s->label_weight_g;
  t.remaining_g = s->remaining_g;
  t.brand = s->brand;
  t.subtype = s->subtype;
  t.color_name = s->color_name;
  return t;
}

AMSUnit demo_unit(int id, const char *name, int humidity, float temp, int nozzle) {
  AMSUnit u;
  u.id = id;
  u.name = name;
  u.humidity = humidity;
  u.temp = temp;
  u.nozzle = nozzle;
  return u;
}

// AMS layout per demo printer: printer 0 is a dual-nozzle printer with two
// AMS, an AMS HT and the external slot; printer 1 a single AMS; printer 2 is
// offline with nothing loaded.
void fill_demo_ams(int printer_idx, DisplayState &ds) {
  ds.ams_units.clear();
  ds.dual_nozzle = false;
  ds.tray_now = 255;
  if (printer_idx == 0) {
    ds.dual_nozzle = true;
    ds.tray_now = 1;
    AMSUnit a = demo_unit(0, "AMS A", 3, 26.5f, 0);
    a.custom_name = "Daily";
    a.trays = {demo_tray(0, find_demo_spool(42)), demo_tray(1, find_demo_spool(41)),
               demo_tray(2, find_demo_spool(40)), demo_tray(3, nullptr)};
    AMSUnit b = demo_unit(1, "AMS B", 2, 25.0f, 1);
    b.trays = {demo_tray(0, find_demo_spool(39)), demo_tray(1, find_demo_spool(38)),
               demo_tray(2, nullptr), demo_tray(3, find_demo_spool(36))};
    AMSUnit ht = demo_unit(128, "AMS HT", 1, 55.0f, 1);
    ht.is_ht = true;
    ht.trays = {demo_tray(0, find_demo_spool(35))};
    AMSUnit vt = demo_unit(255, "Ext", 0, 0.0f, 0);
    vt.is_vt = true;
    vt.trays = {demo_tray(0, find_demo_spool(37))};
    ds.ams_units = {a, b, ht, vt};
  } else if (printer_idx == 1) {
    ds.tray_now = 2;
    AMSUnit a = demo_unit(0, "AMS A", 4, 24.0f, -1);
    a.trays = {demo_tray(0, find_demo_spool(36)), demo_tray(1, nullptr),
               demo_tray(2, find_demo_spool(34)), demo_tray(3, find_demo_spool(42))};
    ds.ams_units = {a};
  }
}

void fill_demo_plugs(int printer_idx, DisplayState &ds) {
  ds.plugs.clear();
  ds.power_plug_id = 0;
  if (printer_idx == 0) {
    ds.plugs.push_back({1, "X1C Power", true});
    ds.power_plug_id = 1;
  } else if (printer_idx == 1) {
    ds.plugs.push_back({2, "P1S Power", false});
    ds.power_plug_id = 2;
  }
  ds.plugs.push_back({3, "Enclosure Light", false});  // not assigned to a printer
  ds.plugs_generation++;
}

FilamentInfo filament_from_spool(const DemoSpool &s) {
  FilamentInfo fi;
  fi.spool_id = s.id;
  fi.material_type = s.material;
  fi.color_hex = s.color_hex;
  fi.spool_name = std::string(s.brand) + " " + s.material;
  fi.brand = s.brand;
  fi.subtype = s.subtype;
  fi.color_name = s.color_name;
  fi.label_weight_g = s.label_weight_g;
  fi.weight_used_g = s.label_weight_g - s.remaining_g;
  fi.core_weight_g = 250;
  fi.min_temp = 190;
  fi.max_temp = 240;
  return fi;
}

// Tag UID -> demo spool id. Tags not listed here resolve as unlinked.
std::map<std::string, int> &tag_links() {
  static std::map<std::string, int> links = {{"04A1B2C3D4E5F6", 39}};
  return links;
}

}  // namespace

void BambuddyAPIComponent::setup() {
  ESP_LOGW(TAG, "Host build: using mock backend with demo data (backend_url ignored)");
  start_ms_ = millis();
  auto &ds = display_state_;
  ds.backend_state = BackendState::REGISTERED;
  ds.ip_address = "127.0.0.1";
  ds.nfc_ok = has_nfc();
  ds.scale_ok = true;
  ds.weight_grams = 812.4f;
  ds.weight_stable = true;
  ds.printers = {{"1", "X1C Workshop", true}, {"2", "P1S Office", true}, {"3", "A1 mini", false}};
  ds.selected_printer_id = ds.printers[0].id;
  ds.printer_connected = true;
  fill_demo_ams(0, ds);
  fill_demo_plugs(0, ds);
  for (const auto &s : DEMO_SPOOLS)
    ds.recent_spools.push_back({s.id, s.material, s.brand, s.color_hex});
  ds.recent_spools_generation++;
  ds.storage_locations = {{1, "Shelf A", "04AABBCCDDEE01", 4},
                          {2, "Dry Box", "", 2},
                          {3, "Drawer 3", "", 0}};
  ds.storage_locations_generation++;
}

void BambuddyAPIComponent::loop() {
  const uint32_t now = millis();
  auto &ds = display_state_;
  if (assign_clear_ms_ != 0 && (int32_t)(now - assign_clear_ms_) >= 0) {
    assign_clear_ms_ = 0;
    ds.assign_success = false;
    ds.assign_slot_desc.clear();
    ds.spool_selected = false;
    ds.current_filament = FilamentInfo{};
    ds.nfc_state = NFCTagState::ABSENT;
  }
  if (pending_assign_spool_id_ != 0 && (int32_t)(now - pending_assign_expiry_ms_) >= 0) {
    pending_assign_spool_id_ = 0;
    ds.spool_assign_expiry_ms = 0;
  }
  if (unlinked_tag_expiry_ms_ != 0 && (int32_t)(now - unlinked_tag_expiry_ms_) >= 0) {
    unlinked_tag_expiry_ms_ = 0;
    ds.unlinked_tag_expiry_ms = 0;
    if (!ds.spool_selected) ds.nfc_state = NFCTagState::ABSENT;
  }
}

DisplayState BambuddyAPIComponent::snapshot() {
  DisplayState copy = display_state_;
  copy.uptime_s = (millis() - start_ms_) / 1000;
  return copy;
}

// ---- Optional hardware (only present if the host YAML wires something in) ----
void BambuddyAPIComponent::set_nfc_scan_enabled(bool enabled) {
  if (nfc_) nfc_->set_scan_enabled(enabled);
}
void BambuddyAPIComponent::set_nfc_low_power(bool low_power) {
  if (nfc_) nfc_->set_low_power(low_power);
}
void BambuddyAPIComponent::play_chime(const std::string &rtttl) {
  if (speaker_) speaker_->play(rtttl);
}
bool BambuddyAPIComponent::chime_playing() const { return speaker_ != nullptr && speaker_->is_playing(); }

void BambuddyAPIComponent::set_low_power(bool enable) {
  ESP_LOGI(TAG, "Mock: low power %s", enable ? "on" : "off");
  low_power_ = enable;
}
void BambuddyAPIComponent::configure_pm(bool light_sleep) {}
void BambuddyAPIComponent::restart_scale_server() {}
void BambuddyAPIComponent::save_calibration_nvs() {}

// ---- Printer / AMS ----
void BambuddyAPIComponent::set_selected_printer(int idx) {
  auto &ds = display_state_;
  if (idx < 0 || idx >= (int)ds.printers.size()) idx = 0;
  if (idx == ds.selected_printer_idx && !ds.selected_printer_id.empty()) return;
  ds.selected_printer_idx = idx;
  ds.selected_printer_id = ds.printers[idx].id;
  ds.printer_connected = ds.printers[idx].online;
  fill_demo_ams(idx, ds);
  fill_demo_plugs(idx, ds);
  ESP_LOGI(TAG, "Mock: selected printer %s", ds.printers[idx].name.c_str());
}

void BambuddyAPIComponent::clear_slot_assignment(int ams_id, int tray_slot) {
  for (auto &u : display_state_.ams_units) {
    if (u.id != ams_id) continue;
    for (auto &t : u.trays) {
      if (t.slot != tray_slot) continue;
      t.spool_id = 0;
      t.label_weight_g = 0;
      t.remaining_g = 0;
      t.brand.clear();
      t.subtype.clear();
      t.color_name.clear();
    }
  }
  set_status("Assignment cleared");
}

void BambuddyAPIComponent::assign_spool_to_slot(int spool_id, int ams_id, int tray_slot) {
  const DemoSpool *s = find_demo_spool(spool_id);
  if (s == nullptr) {
    set_status("Spool #" + std::to_string(spool_id) + " not found");
    return;
  }
  for (auto &u : display_state_.ams_units) {
    if (u.id != ams_id) continue;
    for (auto &t : u.trays)
      if (t.slot == tray_slot) t = demo_tray(tray_slot, s);
  }
  set_status("Spool #" + std::to_string(spool_id) + " assigned");
}

void BambuddyAPIComponent::confirm_plate_cleared() {
  display_state_.awaiting_plate_clear = false;
  set_status("Plate cleared");
}

// ---- Spools ----
void BambuddyAPIComponent::request_recent_spools() { display_state_.recent_spools_generation++; }

void BambuddyAPIComponent::link_tag_to_spool(int spool_id) {
  auto &ds = display_state_;
  const DemoSpool *s = find_demo_spool(spool_id);
  if (s == nullptr || ds.last_tag_uid.empty()) return;
  if (!find_conflicting_location(ds.last_tag_uid).empty()) {
    set_status("Tag is linked to a storage location");
    return;
  }
  tag_links()[ds.last_tag_uid] = spool_id;
  FilamentInfo fi = filament_from_spool(*s);
  fi.tag_type = ds.current_filament.tag_type;
  fi.tag_format = ds.current_filament.tag_format;
  fi.sak = ds.current_filament.sak;
  ds.current_filament = fi;
  ds.spool_selected = true;
  ds.unlinked_tag_expiry_ms = 0;
  unlinked_tag_expiry_ms_ = 0;
  ds.propose_nfc_write = fi.tag_type == "ntag";
  ds.status_message = "Tag linked to spool #" + std::to_string(spool_id);
}

void BambuddyAPIComponent::unlink_current_tag() {
  auto &ds = display_state_;
  tag_links().erase(ds.last_tag_uid);
  ds.spool_selected = false;
  ds.current_filament.spool_id = 0;
  ds.current_filament.material_type.clear();
  ds.status_message = "Tag unlinked";
}

void BambuddyAPIComponent::create_spool_from_tag() {
  auto &ds = display_state_;
  if (ds.last_tag_uid.empty()) return;
  // Reuse the first demo spool as the "new" entry — enough to drive the UI.
  link_tag_to_spool(DEMO_SPOOLS[0].id);
  ds.status_message = "Spool created";
}

void BambuddyAPIComponent::record_scale_weight(int spool_id, float total_grams) {
  auto &fi = display_state_.current_filament;
  if (fi.spool_id == spool_id) {
    float remaining = total_grams - fi.core_weight_g;
    if (remaining < 0) remaining = 0;
    fi.weight_used_g = fi.label_weight_g - remaining;
    display_state_.propose_archive = remaining <= 0;
  }
  set_status("Weight saved");
}

void BambuddyAPIComponent::archive_spool(int spool_id) {
  display_state_.propose_archive = false;
  set_status("Spool #" + std::to_string(spool_id) + " archived");
}

// ---- Storage locations ----
void BambuddyAPIComponent::request_storage_locations() { display_state_.storage_locations_generation++; }

void BambuddyAPIComponent::begin_link_location_tag(int location_id) {
  display_state_.location_link_pending_id = location_id;
  display_state_.location_link_conflict_msg.clear();
}

void BambuddyAPIComponent::cancel_link_location_tag() {
  display_state_.location_link_pending_id = 0;
  display_state_.location_link_conflict_msg.clear();
}

void BambuddyAPIComponent::unlink_location_tag(int location_id) {
  for (auto &l : display_state_.storage_locations)
    if (l.id == location_id) l.identifier.clear();
  display_state_.storage_locations_generation++;
}

std::string BambuddyAPIComponent::find_conflicting_location(const std::string &uid, int exclude_location_id) {
  for (const auto &l : display_state_.storage_locations)
    if (l.id != exclude_location_id && !uid.empty() && l.identifier == uid) return l.name;
  return "";
}

// ---- Smart plugs ----
void BambuddyAPIComponent::request_power_plug() {}

void BambuddyAPIComponent::toggle_plug(int plug_id) {
  for (auto &p : display_state_.plugs)
    if (p.id == plug_id) p.on = !p.on;
}

void BambuddyAPIComponent::toggle_power_plug() { toggle_plug(display_state_.power_plug_id); }

// ---- NFC (driven by the host YAML's "Simulate tag" buttons, or bambuddy_nfc) ----
void BambuddyAPIComponent::on_tag_scanned(const std::string &uid, const std::string &tray_uuid, int sak,
                                           const std::string &tag_type, const TagFilamentInfo *filament) {
  ESP_LOGI(TAG, "Mock: tag scanned uid=%s type=%s", uid.c_str(), tag_type.c_str());
  auto &ds = display_state_;
  ds.scan_chime_generation++;

  if (ds.location_link_pending_id > 0) {
    std::string conflict = find_conflicting_location(uid, ds.location_link_pending_id);
    int loc_id = ds.location_link_pending_id;
    ds.location_link_pending_id = 0;
    if (!conflict.empty()) {
      ds.location_link_conflict_msg =
          "This tag is already linked to \"" + conflict + "\". Unlink it there before linking it here.";
      return;
    }
    for (auto &l : ds.storage_locations)
      if (l.id == loc_id) l.identifier = uid;
    ds.storage_locations_generation++;
    return;
  }

  ds.nfc_scan_generation++;
  ds.nfc_state = NFCTagState::PRESENT;
  ds.last_tag_uid = uid;
  ds.tag_resolving = false;
  ds.unlinked_tag_expiry_ms = 0;
  unlinked_tag_expiry_ms_ = 0;
  ds.tag_filament = filament != nullptr ? *filament : TagFilamentInfo{};

  auto it = tag_links().find(uid);
  const DemoSpool *s = it != tag_links().end() ? find_demo_spool(it->second) : nullptr;
  if (s == nullptr) {
    FilamentInfo fi;
    fi.tray_uuid = tray_uuid;
    fi.sak = sak;
    fi.tag_type = tag_type;
    fi.tag_format = (filament != nullptr && filament->valid && !filament->format.empty())
                        ? filament->format
                        : tag_type == "mifare_classic" ? "bambu_lab" : "ndef";
    ds.current_filament = fi;
    ds.spool_selected = false;
    return;
  }
  FilamentInfo fi = filament_from_spool(*s);
  fi.tray_uuid = tray_uuid;
  fi.sak = sak;
  fi.tag_type = tag_type;
  fi.tag_format = tag_type == "mifare_classic" ? "bambu_lab" : "ndef";
  ds.current_filament = fi;
  ds.spool_selected = true;
  ds.propose_archive = false;
  ds.assign_success = false;
  ds.assign_slot_desc.clear();
  ds.status_message = "Spool: " + fi.spool_name;
  pending_assign_spool_id_ = s->id;
  pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
  ds.spool_assign_expiry_ms = pending_assign_expiry_ms_;
  assign_clear_ms_ = 0;
}

void BambuddyAPIComponent::on_tag_removed(const std::string &uid) {
  ESP_LOGI(TAG, "Mock: tag removed uid=%s", uid.c_str());
  auto &ds = display_state_;
  ds.tag_resolving = false;
  if (ds.spool_selected) {
    ds.nfc_state = NFCTagState::ABSENT;
  } else {
    unlinked_tag_expiry_ms_ = millis() + ASSIGN_TTL_MS;
    ds.unlinked_tag_expiry_ms = unlinked_tag_expiry_ms_;
  }
}

void BambuddyAPIComponent::on_write_tag_result(const std::string &uid, bool success, const std::string &msg) {
  pending_write_active_ = false;
  set_status(success ? "Tag written" : "Tag write failed: " + msg);
}

// ---- Scale ----
void BambuddyAPIComponent::on_scale_reading(float grams, bool stable, int raw_adc) {
  display_state_.weight_grams = grams - tare_offset_;
  display_state_.weight_stable = stable;
  display_state_.scale_ok = true;
}

void BambuddyAPIComponent::request_tare() {
  tare_offset_ += display_state_.weight_grams;
  display_state_.weight_grams = 0.0f;
  set_status("Scale tared");
}

void BambuddyAPIComponent::request_calibration(float reference_weight_g) {
  if (reference_weight_g <= 0) return;
  display_state_.weight_grams = reference_weight_g;
  set_status("Scale calibrated");
}

}  // namespace bambuddy_api
}  // namespace esphome

#endif  // USE_HOST
