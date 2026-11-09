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

#include <cstdio>
#include <cstdlib>
#include <deque>

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

// Spools created from tags ("Add to Inventory"), appended after the demo set.
// std::deque keeps element addresses stable, so the DemoSpool pointers and
// the interned strings they point into stay valid as more are added.
std::deque<DemoSpool> &created_spools() {
  static std::deque<DemoSpool> spools;
  return spools;
}
const char *intern(const std::string &s) {
  static std::deque<std::string> pool;
  pool.push_back(s);
  return pool.back().c_str();
}

const DemoSpool *find_demo_spool(int id) {
  for (const auto &s : created_spools())
    if (s.id == id) return &s;
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

// A spool the AMS reports (material/colour/temps from its RFID tag or the
// slicer) that isn't in the Bambuddy inventory: present, but spool_id 0 and
// none of the inventory-enriched fields.
AMSTray demo_untracked_tray(int slot, const char *material, const char *color_hex) {
  AMSTray t;
  t.slot = slot;
  t.present = true;
  t.material_type = material;
  t.color_hex = color_hex;
  t.nozzle_temp_min = 190;
  t.nozzle_temp_max = 230;
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
// AMS, an AMS HT and two external spools; printer 1 a single AMS; printer 2 is
// offline with nothing loaded.
void fill_demo_ams(int printer_idx, DisplayState &ds) {
  ds.ams_units.clear();
  ds.dual_nozzle = false;
  ds.tray_now = 255;
  if (printer_idx == 0) {
    ds.dual_nozzle = true;
    ds.tray_now = 1;
    AMSUnit a = demo_unit(0, "AMS A", 3, 26.5f, 0);
    a.custom_name = "Drawer";
    a.trays = {demo_tray(0, find_demo_spool(42)), demo_tray(1, find_demo_spool(41)),
               demo_tray(2, find_demo_spool(40)), demo_tray(3, nullptr)};
    AMSUnit b = demo_unit(1, "AMS B", 2, 45.0f, 1);
    b.drying = true;  // shows the drying tint on the icon, name and temp/humidity
    b.trays = {demo_tray(0, find_demo_spool(39)), demo_tray(1, find_demo_spool(38)),
               demo_untracked_tray(2, "PLA", "00AE42"), demo_tray(3, find_demo_spool(36))};
    AMSUnit ht = demo_unit(128, "AMS HT", 1, 27.0f, 1);
    ht.is_ht = true;
    ht.trays = {demo_tray(0, find_demo_spool(35))};
    AMSUnit vl = demo_unit(254, "Ext", 0, 0.0f, 1);
    vl.is_vt = true;
    vl.trays = {demo_tray(0, find_demo_spool(37))};
    AMSUnit vr = demo_unit(255, "Ext", 0, 0.0f, 0);
    vr.is_vt = true;
    vr.trays = {demo_tray(0, find_demo_spool(34))};
    ds.ams_units = {a, b, ht, vl, vr};
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
  fi.spool_name = s.brand[0] ? std::string(s.brand) + " " + s.material : std::string(s.material);
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
    // Same as the real component's TTL expiry (bambuddy_api.cpp maintenance block).
    pending_assign_spool_id_ = 0;
    ds.spool_assign_expiry_ms = 0;
    ds.spool_selected = false;
    ds.current_filament = FilamentInfo{};
    ds.status_message = "Timed out — rescan to assign";
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
  ds.status_message = "Tag linked to spool #" + std::to_string(spool_id);
  propose_tag_write(ds.last_tag_uid);
}

// Same end state as a successful unlink in the real api_link_tag(): back to
// idle, with the pending auto-assignment dropped.
void BambuddyAPIComponent::unlink_current_tag() {
  auto &ds = display_state_;
  tag_links().erase(ds.last_tag_uid);
  ds.spool_selected = false;
  ds.current_filament = FilamentInfo{};
  ds.status_message = "Tag unlinked";
  ds.nfc_state = NFCTagState::ABSENT;
  ds.spool_assign_expiry_ms = 0;
  unlinked_tag_expiry_ms_ = 0;
  ds.unlinked_tag_expiry_ms = 0;
  pending_assign_spool_id_ = 0;
}

// Same as the real api_create_spool_from_tag(): a new spool is created from
// the tag's decoded Bambu Lab / OpenTag3D data (a generic 1000 g PLA for a
// plain tag), the tag is linked to it, and it is shown like a scanned spool —
// spool details, auto-assign window, Quick Weight.
void BambuddyAPIComponent::create_spool_from_tag() {
  auto &ds = display_state_;
  if (ds.last_tag_uid.empty()) return;
  const TagFilamentInfo &tf = ds.tag_filament;
  const bool decoded = tf.valid && !tf.material.empty();
  const float label = decoded && tf.spool_weight > 0 && tf.spool_weight <= 5000 ? tf.spool_weight : 1000;
  std::string brand = !decoded ? "" : !tf.brand.empty() ? tf.brand
                    : tf.format != "open_tag_3d" ? "Bambu Lab" : "";
  DemoSpool ns{};
  ns.id = DEMO_SPOOLS[0].id + 1 + (int) created_spools().size();
  ns.material = intern(decoded ? tf.material : "PLA");
  ns.subtype = intern(decoded ? tf.subtype : "");
  ns.brand = intern(brand);
  ns.color_hex = intern(decoded ? tf.color_hex.substr(0, 6) : "");
  ns.color_name = intern(decoded ? tf.color_name : "");
  ns.label_weight_g = label;
  ns.remaining_g = label;
  created_spools().push_back(ns);
  const DemoSpool &s = created_spools().back();
  tag_links()[ds.last_tag_uid] = s.id;
  ds.recent_spools.insert(ds.recent_spools.begin(), {s.id, s.material, s.brand, s.color_hex});
  ds.recent_spools_generation++;

  FilamentInfo fi = filament_from_spool(s);
  fi.tray_uuid = ds.current_filament.tray_uuid;
  fi.sak = ds.current_filament.sak;
  fi.tag_type = ds.current_filament.tag_type;
  fi.tag_format = ds.current_filament.tag_format;
  ds.current_filament = fi;
  ds.spool_selected = true;
  ds.nfc_state = NFCTagState::PRESENT;
  ds.propose_archive = false;
  ds.assign_success = false;
  ds.assign_slot_desc.clear();
  unlinked_tag_expiry_ms_ = 0;
  ds.unlinked_tag_expiry_ms = 0;
  pending_assign_spool_id_ = s.id;
  pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
  ds.spool_assign_expiry_ms = pending_assign_expiry_ms_;
  assign_clear_ms_ = 0;
  ds.status_message = "Created spool #" + std::to_string(s.id);
}

void BambuddyAPIComponent::record_scale_weight(int spool_id, float total_grams) {
  auto &fi = display_state_.current_filament;
  if (fi.spool_id == spool_id) {
    float remaining = total_grams - fi.core_weight_g;
    if (remaining < 0) remaining = 0;
    fi.weight_used_g = fi.label_weight_g - remaining;
    display_state_.propose_archive = remaining <= 0;
  }
  // As in the real record_scale_weight(): weighing restarts the auto-assign
  // window of the spool waiting for the AMS.
  if (pending_assign_spool_id_ > 0 && pending_assign_spool_id_ == spool_id) {
    pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
    display_state_.spool_assign_expiry_ms = pending_assign_expiry_ms_;
  }
  set_status("Weight saved");
}

// Labels (host/espoolbuddy_console_host_labels.yaml): there's no Bambuddy to
// draw one, so every spool gets the PNG named by ESPOOLBUDDY_LABEL_PNG, e.g. a
// label saved from Bambuddy's label dialog. Without it a request fails.
void BambuddyAPIComponent::request_label_png(int spool_id, const std::string &tmpl, const std::string &fields,
                                             int dpi) {
  label_png_.clear();
  label_png_state_ = LABEL_FAILED;
  const char *path = getenv("ESPOOLBUDDY_LABEL_PNG");
  if (path == nullptr || *path == '\0') {
    ESP_LOGW(TAG, "Label for spool %d: set ESPOOLBUDDY_LABEL_PNG to a PNG to print one", spool_id);
    return;
  }
  FILE *f = fopen(path, "rb");
  if (f == nullptr) {
    ESP_LOGW(TAG, "Label for spool %d: can't read %s", spool_id, path);
    return;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) label_png_.append(buf, n);
  fclose(f);
  ESP_LOGI(TAG, "Label for spool %d (%s, %s, %d dpi): %s (%u bytes)", spool_id, tmpl.c_str(),
           fields.empty() ? "default fields" : fields.c_str(), dpi, path, (unsigned) label_png_.size());
  label_png_state_ = LABEL_READY;
}
void BambuddyAPIComponent::cancel_label_png() {
  label_png_.clear();
  label_png_state_ = LABEL_NONE;
}
BambuddyAPIComponent::LabelFetch BambuddyAPIComponent::poll_label_png(std::string &png) {
  LabelFetch state = label_png_state_;
  if (state == LABEL_READY) png = std::move(label_png_);
  label_png_.clear();
  label_png_state_ = LABEL_NONE;
  return state;
}

void BambuddyAPIComponent::archive_spool(int spool_id) {
  display_state_.propose_archive = false;
  set_status("Spool archived");  // same message as the real component
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

  // A storage-location tag: assign the staged spool to it, as the real
  // on_tag_scanned() does, without disturbing the staged spool.
  for (const auto &l : ds.storage_locations) {
    if (l.identifier.empty() || l.identifier != uid) continue;
    if (ds.spool_selected && ds.current_filament.spool_id > 0) {
      ds.current_filament.storage_location = l.name;
      set_status("Assigned to " + l.name);
    } else {
      set_status("Location tag: " + l.name);
    }
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
  int spool_id = 0;
  tag_write_.finish(success, 0, &spool_id);
  set_status(success ? "Tag written" : "Tag write failed: " + msg);
}

// Tag writes: no Bambuddy to queue them, so a request goes straight to
// waiting for a tag; sim_tag_write_result() finishes it.
void BambuddyAPIComponent::request_tag_write() {
  display_state_.propose_nfc_write = false;
  set_tag_write_stage(TagWriteStage::WAITING, display_state_.current_filament.spool_id);
}

void BambuddyAPIComponent::cancel_tag_write() {
  set_tag_write_stage(TagWriteStage::NONE);
  set_status("Tag write cancelled");
}

void BambuddyAPIComponent::sim_tag_write_result(bool success) {
  if (success) display_state_.current_filament.tag_format = "open_tag_3d";
  set_tag_write_stage(success ? TagWriteStage::DONE : TagWriteStage::FAILED,
                      display_state_.tag_write_spool_id,
                      success ? "" : "Tag too small: needs 129 bytes, holds 48");
  set_status(success ? "Tag written successfully" : "Tag write failed");
}

// ---- Scale ----
void BambuddyAPIComponent::on_scale_reading(float gross, bool stable) {
  display_state_.weight_grams = gross - tare_offset_;
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

// ---- Simulator-only hooks ----
void BambuddyAPIComponent::sim_scale_reading(float gross, bool stable) { on_scale_reading(gross, stable); }

// What api_get_ams()'s auto-assign block does on a device when the AMS reports
// a newly loaded, unassigned tray while a spool is pending.
void BambuddyAPIComponent::sim_ams_insert() {
  auto &ds = display_state_;
  const DemoSpool *s = find_demo_spool(pending_assign_spool_id_);
  if (pending_assign_spool_id_ == 0 || s == nullptr) {
    set_status("No spool waiting to be assigned");
    return;
  }
  for (auto &u : ds.ams_units) {
    if (u.is_vt) continue;  // the real auto-assign skips external bypass trays
    for (auto &t : u.trays) {
      if (t.present) continue;
      t = demo_tray(t.slot, s);
      char desc[32];
      snprintf(desc, sizeof(desc), "AMS-%c \xC2\xB7 Slot %d", 'A' + (char) (u.id % 26), t.slot + 1);
      pending_assign_spool_id_ = 0;
      ds.spool_assign_expiry_ms = 0;
      ds.assign_success = true;
      ds.assign_slot_desc = desc;
      assign_clear_ms_ = millis() + 4000;
      ESP_LOGI(TAG, "Mock: spool %d loaded into %s", s->id, desc);
      return;
    }
  }
  set_status("No empty AMS slot");
}

void BambuddyAPIComponent::sim_assign_timeout() {
  if (pending_assign_spool_id_ == 0) return;
  pending_assign_expiry_ms_ = millis();
  display_state_.spool_assign_expiry_ms = pending_assign_expiry_ms_;
}

void BambuddyAPIComponent::sim_plate_clear() { display_state_.awaiting_plate_clear = true; }

// What api_get_ams() does when a known spool disappears from its slot: the
// spool is staged like a fresh scan (card, auto-assign window, scan
// generation) and a "removed" note is shown for 5 s.
void BambuddyAPIComponent::sim_ams_remove() {
  auto &ds = display_state_;
  for (auto &u : ds.ams_units) {
    if (u.is_vt) continue;
    for (auto &t : u.trays) {
      if (!t.present || t.spool_id == 0) continue;
      const DemoSpool *s = find_demo_spool(t.spool_id);
      if (s == nullptr) continue;
      t = demo_tray(t.slot, nullptr);
      FilamentInfo fi = filament_from_spool(*s);
      ds.current_filament = fi;
      ds.spool_selected = true;
      ds.nfc_state = NFCTagState::PRESENT;
      ds.last_tag_uid = "ams:" + std::to_string(s->id);
      ds.assign_success = false;
      ds.assign_slot_desc.clear();
      ds.propose_archive = false;
      pending_assign_spool_id_ = s->id;
      pending_assign_expiry_ms_ = millis() + ASSIGN_TTL_MS;
      ds.spool_assign_expiry_ms = pending_assign_expiry_ms_;
      assign_clear_ms_ = 0;
      ds.nfc_scan_generation++;
      ds.ams_removed_spool_desc = std::string(s->material) + " " + s->subtype + " #" +
                                  std::to_string(s->id) + " removed - ready for weighing";
      ds.ams_removed_toast_expiry_ms = millis() + 5000;
      ESP_LOGI(TAG, "Mock: spool %d removed from %s slot %d", s->id, u.name.c_str(), t.slot + 1);
      return;
    }
  }
  set_status("No assigned spool in the AMS");
}

void BambuddyAPIComponent::sim_scale_offline() {
  display_state_.scale_ok = false;
  display_state_.weight_stable = false;
  display_state_.weight_grams = 0.0f;
}

}  // namespace bambuddy_api
}  // namespace esphome

#endif  // USE_HOST
