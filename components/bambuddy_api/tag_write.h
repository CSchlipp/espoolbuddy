#pragma once
#include <cstdint>
#include <vector>
#include "esphome/core/helpers.h"

namespace esphome {
namespace bambuddy_api {

// How long an armed tag write waits for a tag before it fails. Bambuddy has no
// limit of its own: without this, a write queued and forgotten would overwrite
// whatever NTAG is presented next, maybe hours later.
static constexpr uint32_t WRITE_ARM_TTL_MS = 60000;

// Bambuddy's OpenTag3D payload starts with a Capability Container (E1 xx xx
// xx), which belongs in page 3, followed by the NDEF TLV for page 4. Returns
// the payload without it, ready to write from page 4; anything else as is.
std::vector<uint8_t> strip_capability_container(std::vector<uint8_t> payload);

/**
 * Bookkeeping for the NTAG writes Bambuddy requests, shared by the HTTP, NFC
 * and main tasks. Pure logic: no I/O and no clock (callers pass now_ms and
 * carry out what the methods report), guarded by its own mutex.
 *
 * Every device has one local write: IDLE → ARMED (requested) → WRITING
 * (claimed by whoever ends it, the NFC task or the timeout) → IDLE. Cancel
 * only acts on ARMED: a write that has started runs to the end and reports.
 *
 * A console also tracks the request itself. Bambuddy repeats write_tag in
 * every heartbeat until it has a result, so:
 *  - a request goes to one reader, the console's or the scale's, decided
 *    when it first arrives; its repeats follow;
 *  - a heartbeat without it means it was cancelled or replaced;
 *  - repeats that arrive while its result is on the way are stale;
 *  - a result is sent only while Bambuddy still waits for it.
 */
class TagWriteCoordinator {
 public:
  // A scale has no Bambuddy connection; it takes its requests from a console.
  void set_scale(bool is_scale) { is_scale_ = is_scale; }

  // ---- The local write ----
  enum class Arm {
    ARMED,         // waiting for a tag now
    REPEAT,        // this spool's write is already armed or running
    BUSY,          // another spool's write is running; its repeat comes again
    JUST_WRITTEN,  // scale: a stale copy for the spool it just wrote
  };
  // ndef is the payload as Bambuddy sends it (see strip_capability_container).
  Arm arm(int spool_id, std::vector<uint8_t> ndef, uint32_t now_ms);
  bool armed();
  // ARMED → WRITING, so this caller alone reports the outcome. Copies the
  // payload and spool id when the pointers are non-null.
  bool claim(std::vector<uint8_t> *ndef = nullptr, int *spool_id = nullptr);
  // claim() for a write armed WRITE_ARM_TTL_MS or longer.
  bool claim_expired(uint32_t now_ms);
  // ARMED → IDLE without a result. False if nothing was armed.
  bool disarm(int *spool_id);
  // Ends the write (ARMED or WRITING → IDLE). False if there was none.
  bool finish(bool success, uint32_t now_ms, int *spool_id);

  // ---- Console: Bambuddy's request ----
  struct Request {
    bool stale{false};          // its result is on the way: ignore
    bool fresh{false};          // first heartbeat with this request
    bool to_scale{false};       // goes to the scale's reader, not the console's
    bool dropped_local{false};  // an earlier local write was disarmed for it
  };
  // A heartbeat carrying write_tag for spool_id. tag_on_scale: the last tag
  // was seen on the scale's reader.
  Request on_requested(int spool_id, bool tag_on_scale);
  // A heartbeat without write_tag. Returns the spool whose request ended (0 if
  // none was pending) and disarms its local write, if any (*dropped_local).
  int on_not_requested(bool *dropped_local);
  // An answer to the request for spool_id is on its way to Bambuddy: a result
  // (a local write ended, or the scale sent one) or the console's cancel.
  // Repeats of the request are stale until result_sent().
  void result_ready(int spool_id);
  // Whether Bambuddy still waits for this result.
  bool result_wanted(int spool_id);
  // The send attempt is over; delivered = Bambuddy answered.
  void result_sent(int spool_id, bool delivered);
  // Whether the scale's next heartbeat response must tell it to cancel.
  // Reading it clears it.
  bool take_scale_cancel();

  // ---- Scale: the console's cancel ----
  // Disarms an armed write (returns its spool, 0 if none) and forgets the
  // spool just written.
  int scale_cancelled();

 private:
  enum class State : uint8_t { IDLE, ARMED, WRITING };
  bool claim_locked(std::vector<uint8_t> *ndef, int *spool_id);

  Mutex mutex_;
  bool is_scale_{false};

  State state_{State::IDLE};
  int spool_id_{0};
  std::vector<uint8_t> ndef_;
  uint32_t armed_ms_{0};

  // Scale: the spool it last wrote successfully. A copy of write_tag the
  // console sent before Bambuddy had that result is ignored, until the
  // console's cancel arrives or WRITE_ARM_TTL_MS passes.
  int just_written_spool_{0};
  uint32_t just_written_ms_{0};

  // Console: the spool Bambuddy is asking to write (0 = none), where that
  // request went, and the spool whose result is on its way.
  int requested_spool_{0};
  bool requested_to_scale_{false};
  int result_inflight_spool_{0};
  // Something went to the scale since it was last told to cancel. The cancel
  // travels as its own flag, never in the single console→scale command slot,
  // where a tare or calibration sync could overwrite it.
  bool scale_may_be_armed_{false};
  bool scale_cancel_pending_{false};
};

}  // namespace bambuddy_api
}  // namespace esphome
