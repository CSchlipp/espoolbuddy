#include "tag_write.h"

namespace esphome {
namespace bambuddy_api {

static constexpr uint8_t CC_MAGIC = 0xE1;
static constexpr uint8_t TLV_NDEF_MESSAGE = 0x03;

std::vector<uint8_t> strip_capability_container(std::vector<uint8_t> payload) {
  if (payload.size() >= 6 && payload[0] == CC_MAGIC && payload[4] == TLV_NDEF_MESSAGE)
    payload.erase(payload.begin(), payload.begin() + 4);
  return payload;
}

bool is_ndef_message_tlv(const std::vector<uint8_t> &data) {
  if (data.size() < 2 || data[0] != TLV_NDEF_MESSAGE) return false;
  size_t len = data[1], header = 2;
  if (len == 0xFF) {
    if (data.size() < 4) return false;
    len = (size_t) data[2] << 8 | data[3];
    header = 4;
  }
  return len > 0 && header + len <= data.size();
}

TagWriteCoordinator::Arm TagWriteCoordinator::arm(int spool_id, std::vector<uint8_t> ndef,
                                                  std::vector<uint8_t> ndef_v2,
                                                  uint32_t now_ms) {
  LockGuard guard(mutex_);
  if (state_ != State::IDLE && spool_id_ == spool_id) return Arm::REPEAT;
  // A different spool while a write is on the tag: Bambuddy holds one command
  // per device, so the new request repeats once this write has finished.
  if (state_ == State::WRITING) return Arm::BUSY;
  if (is_scale_ && spool_id == just_written_spool_ &&
      now_ms - just_written_ms_ < WRITE_ARM_TTL_MS)
    return Arm::JUST_WRITTEN;
  if (spool_id != just_written_spool_) just_written_spool_ = 0;
  ndef_ = strip_capability_container(std::move(ndef));
  ndef_v2_ = std::move(ndef_v2);  // only sent by releases that have no CC copy
  spool_id_ = spool_id;
  armed_ms_ = now_ms;
  state_ = State::ARMED;
  return Arm::ARMED;
}

bool TagWriteCoordinator::armed() {
  LockGuard guard(mutex_);
  return state_ == State::ARMED;
}

bool TagWriteCoordinator::claim_locked(std::vector<uint8_t> *ndef, std::vector<uint8_t> *ndef_v2,
                                       int *spool_id) {
  if (state_ != State::ARMED) return false;
  state_ = State::WRITING;
  if (ndef != nullptr) *ndef = ndef_;
  if (ndef_v2 != nullptr) *ndef_v2 = ndef_v2_;
  if (spool_id != nullptr) *spool_id = spool_id_;
  return true;
}

bool TagWriteCoordinator::claim(std::vector<uint8_t> *ndef, std::vector<uint8_t> *ndef_v2,
                                int *spool_id) {
  LockGuard guard(mutex_);
  return claim_locked(ndef, ndef_v2, spool_id);
}

bool TagWriteCoordinator::claim_expired(uint32_t now_ms) {
  LockGuard guard(mutex_);
  if (state_ != State::ARMED || now_ms - armed_ms_ < WRITE_ARM_TTL_MS) return false;
  return claim_locked(nullptr, nullptr, nullptr);
}

bool TagWriteCoordinator::disarm(int *spool_id) {
  LockGuard guard(mutex_);
  if (state_ != State::ARMED) return false;
  state_ = State::IDLE;
  ndef_.clear();
  ndef_v2_.clear();
  *spool_id = spool_id_;
  return true;
}

bool TagWriteCoordinator::finish(bool success, uint32_t now_ms, int *spool_id) {
  LockGuard guard(mutex_);
  if (state_ == State::IDLE) return false;
  state_ = State::IDLE;
  ndef_.clear();
  ndef_v2_.clear();
  *spool_id = spool_id_;
  if (!is_scale_) {
    result_inflight_spool_ = spool_id_;
  } else if (success) {
    just_written_spool_ = spool_id_;
    just_written_ms_ = now_ms;
  }
  return true;
}

TagWriteCoordinator::Request TagWriteCoordinator::on_requested(int spool_id, bool tag_on_scale) {
  LockGuard guard(mutex_);
  Request r;
  if (spool_id == result_inflight_spool_) {
    r.stale = true;
    return r;
  }
  r.fresh = spool_id != requested_spool_;
  if (r.fresh) {
    // A new request, possibly replacing another spool's.
    if (requested_spool_ != 0 && scale_may_be_armed_) scale_cancel_pending_ = true;
    requested_spool_ = spool_id;
    requested_to_scale_ = tag_on_scale;
    if (tag_on_scale && state_ == State::ARMED) {
      state_ = State::IDLE;
      ndef_.clear();
      ndef_v2_.clear();
      r.dropped_local = true;
    }
  }
  r.to_scale = requested_to_scale_;
  if (r.to_scale) {
    scale_may_be_armed_ = true;
    scale_cancel_pending_ = false;  // this request supersedes any cancel
  }
  return r;
}

int TagWriteCoordinator::on_not_requested(bool *dropped_local) {
  LockGuard guard(mutex_);
  *dropped_local = false;
  const int spool_id = requested_spool_;
  if (spool_id == 0) return 0;
  requested_spool_ = 0;
  if (!requested_to_scale_ && state_ == State::ARMED) {
    state_ = State::IDLE;
    ndef_.clear();
    ndef_v2_.clear();
    *dropped_local = true;
  }
  if (scale_may_be_armed_) scale_cancel_pending_ = true;
  return spool_id;
}

void TagWriteCoordinator::result_ready(int spool_id) {
  LockGuard guard(mutex_);
  result_inflight_spool_ = spool_id;
}

bool TagWriteCoordinator::result_wanted(int spool_id) {
  LockGuard guard(mutex_);
  return requested_spool_ == spool_id;
}

void TagWriteCoordinator::result_sent(int spool_id, bool delivered) {
  LockGuard guard(mutex_);
  if (result_inflight_spool_ == spool_id) result_inflight_spool_ = 0;
  if (delivered && requested_spool_ == spool_id) requested_spool_ = 0;
  // The scale is done with this write: leave it idle and clear its guard.
  if (scale_may_be_armed_) scale_cancel_pending_ = true;
}

bool TagWriteCoordinator::take_scale_cancel() {
  LockGuard guard(mutex_);
  if (!scale_cancel_pending_) return false;
  scale_cancel_pending_ = false;
  scale_may_be_armed_ = false;
  return true;
}

int TagWriteCoordinator::scale_cancelled() {
  LockGuard guard(mutex_);
  just_written_spool_ = 0;
  if (state_ != State::ARMED) return 0;
  state_ = State::IDLE;
  ndef_.clear();
  ndef_v2_.clear();
  return spool_id_;
}

}  // namespace bambuddy_api
}  // namespace esphome
