// The tag chip's state (update_tag_state_locked()). Pure logic on
// display_state_, kept out of bambuddy_api.cpp so the desktop simulator, which
// builds host/bambuddy_api_mock.cpp instead, shows the same states.
#include "bambuddy_api.h"
#include <cctype>

namespace esphome {
namespace bambuddy_api {

namespace {

// The tag holds the spool's value cut to its field, so compare against the
// spool's value cut the same way (on a UTF-8 character boundary, as the
// encoder cuts), ignoring case and surrounding spaces. A field the spool
// doesn't have is not a difference: a rewrite would only blank it on the tag.
bool same_field(const std::string &on_tag, const std::string &in_spool, size_t field) {
  if (in_spool.empty()) return true;
  auto norm = [](std::string v, size_t n) {
    const size_t a = v.find_first_not_of(' ');
    v = a == std::string::npos ? std::string() : v.substr(a);
    if (v.size() > n) {
      while (n > 0 && ((unsigned char) v[n] & 0xC0) == 0x80) n--;
      v.resize(n);
    }
    while (!v.empty() && v.back() == ' ') v.pop_back();
    for (char &c : v) c = (char) tolower((unsigned char) c);
    return v;
  };
  return norm(on_tag, field) == norm(in_spool, field);
}

}  // namespace

void BambuddyAPIComponent::update_tag_state_locked() {
  auto &ds = display_state_;
  const auto &fi = ds.current_filament;
  const auto &tf = ds.tag_filament;
  ds.tag_state = TagState::NONE;
  ds.tag_writable = fi.spool_id > 0 && fi.tag_type == "ntag" && !ds.last_tag_uid.empty() &&
                    last_tag_source_ != TagSource::AMS;
  if (!ds.tag_writable) return;
  if (tag_written_scan_gen_ == ds.nfc_scan_generation && tag_written_spool_id_ == fi.spool_id &&
      fi.tag_format == "open_tag_3d") {
    ds.tag_state = TagState::MATCHES;  // written from here since this scan
    return;
  }
  if (!tf.valid || tf.format != "open_tag_3d") return;
  // OpenTag3D field lengths: material 5, brand 16 (the same in v1 and v2).
  const bool differs = !same_field(tf.material, fi.material_type, 5) ||
                       !same_field(tf.brand, fi.brand, 16) ||
                       !same_field(tf.color_hex, fi.color_hex, 6);
  ds.tag_state = differs ? TagState::DIFFERS : TagState::MATCHES;
}

}  // namespace bambuddy_api
}  // namespace esphome
