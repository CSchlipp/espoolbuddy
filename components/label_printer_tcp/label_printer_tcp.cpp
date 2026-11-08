#include "label_printer_tcp.h"

#include "esphome/core/log.h"

namespace esphome::label_printer_tcp {

static const char *const TAG = "label_printer_tcp";

// A failed connect is tried again after this, within the core's 20 s.
static const uint32_t RECONNECT_MS = 3000;
// Writes per loop at most, so a fast link doesn't hold up the UI.
static const uint8_t WRITES_PER_LOOP = 8;

void LabelPrinterTCP::setup() {
  this->link_.set_reconnect_interval(RECONNECT_MS);
  this->link_.begin(TAG);
}

void LabelPrinterTCP::dump_config() {
  ESP_LOGCONFIG(TAG, "Label printer on the network:\n  Address: %s:%u", this->link_.host(), this->link_.port());
}

void LabelPrinterTCP::open() {
  this->link_.close();
  // The first attempt goes now, not after an interval.
  this->link_.begin(TAG);
  this->opened_ = true;
  this->job_out_ = false;
  this->send_state_ = SendState::IDLE;
}

void LabelPrinterTCP::send() {
  this->job_out_ = false;
  this->send_state_ = SendState::SENDING;
}

void LabelPrinterTCP::close(bool unreachable) {
  this->link_.close();
  this->opened_ = false;
  this->job_out_ = false;
  this->send_state_ = SendState::IDLE;
}

void LabelPrinterTCP::loop() {
  if (!this->opened_) return;
  // Connecting, or retrying after a failed attempt.
  this->link_.poll();
  if (!this->link_.connected()) {
    if (this->send_state_ == SendState::SENDING) this->send_state_ = SendState::LOST;
    return;
  }
  this->read_replies_();
  if (!this->opened_) return;  // a reply ended the print
  if (this->send_state_ == SendState::SENDING) this->write_job_();
}

void LabelPrinterTCP::read_replies_() {
  if (!this->link_.ready()) return;
  // Drain it: ready() only reports new data once the socket is empty.
  uint8_t buf[64];
  while (true) {
    const ssize_t n = this->link_.read(buf, sizeof(buf));
    if (n == 0) return;
    if (n < 0) {
      // Dropped. While sending that's a lost print; after it, ready() says
      // so and the core takes it as sent.
      if (this->send_state_ == SendState::SENDING) this->send_state_ = SendState::LOST;
      return;
    }
    if (this->reply_ != nullptr) this->reply_->on_reply(buf, n);
    if (!this->opened_) return;
  }
}

void LabelPrinterTCP::write_job_() {
  for (uint8_t i = 0; i < WRITES_PER_LOOP; i++) {
    // Top up the link's buffer straight from the driver.
    if (!this->job_out_) {
      const size_t room = this->link_.tx_free();
      if (room > 0) {
        const size_t n = this->driver_->next(this->link_.tx_tail(), room);
        if (n == 0) {
          this->job_out_ = true;
        } else {
          this->link_.tx_commit(n);
        }
      }
    }
    const bool empty = this->link_.flush_tx();
    if (!this->link_.connected()) {
      this->send_state_ = SendState::LOST;
      return;
    }
    if (empty && this->job_out_) {
      this->send_state_ = SendState::DONE;
      return;
    }
    if (!empty) return;  // the socket is full: more next loop
  }
}

}  // namespace esphome::label_printer_tcp
