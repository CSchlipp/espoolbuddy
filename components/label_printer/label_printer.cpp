#include "label_printer.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>

namespace esphome::label_printer {

static const char *const TAG = "label_printer";

LabelPrinter *global_label_printer = nullptr;  // NOLINT

static const uint32_t FETCH_TIMEOUT_MS = 15000;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;  // from the start of the print
static const uint32_t DONE_TIMEOUT_MS = 15000;
// Sending: a base plus 1 ms a byte, several times what Bluetooth needs (about
// 3 KB/s), so a stalled link ends the print instead of keeping it busy.
static const uint32_t SEND_TIMEOUT_BASE_MS = 10000;
static const uint32_t RESULT_SHOWN_MS = 6000;
static const size_t DECODE_SLICE = 1024;  // PNG bytes fed per loop

void LabelPrinter::setup() {
  global_label_printer = this;
  if (this->api_ == nullptr || this->driver_ == nullptr || this->transport_ == nullptr) {
    ESP_LOGE(TAG, "Not wired to an API, driver and transport");
    this->mark_failed();
    return;
  }
  this->transport_->set_driver(this->driver_);
  this->transport_->set_reply_handler(this);
}

void LabelPrinter::dump_config() {
  const PrinterInfo &info = this->driver_->info();
  // "right" and "left" are offsets past any head, clamped to the wall.
  char offset[24];
  if (this->x_offset_ >= info.head_dots) {
    snprintf(offset, sizeof(offset), "right wall");
  } else if (this->x_offset_ <= -(int) info.head_dots) {
    snprintf(offset, sizeof(offset), "left wall");
  } else {
    snprintf(offset, sizeof(offset), "%d dots", this->x_offset_);
  }
  ESP_LOGCONFIG(TAG,
                "Label printer:\n"
                "  Model:    %s (%u dots at %u dpi)\n"
                "  Template: %s\n"
                "  Placed:   %s\n"
                "  Fields:   %s",
                info.name, info.head_dots, info.dpi, this->template_.c_str(), offset,
                this->fields_.empty() ? "(Bambuddy's default)" : this->fields_.c_str());
}

bool LabelPrinter::print_spool(int spool_id) {
  if (this->busy() || spool_id <= 0 || this->is_failed()) return false;
  ESP_LOGI(TAG, "Printing the label of spool %d", spool_id);
  this->spool_id_ = spool_id;
  this->result_ = Result::NONE;
  this->png_.clear();
  this->raster_.clear();
  this->api_->request_label_png(spool_id, this->template_, this->fields_, this->driver_->info().dpi);
  this->transport_->open();
  this->state_ = State::FETCH;
  this->state_ms_ = millis();
  return true;
}

const char *LabelPrinter::status_text(int spool_id) const {
  if (spool_id != this->spool_id_) return this->busy() ? "Printer busy" : "";
  switch (this->state_) {
    case State::FETCH:
    case State::DECODE:
      return "Preparing label";
    case State::CONNECT:
      return this->transport_->searching() ? "Looking for printer" : "Connecting to printer";
    case State::SEND:
    case State::WAIT_DONE:
      return "Printing";
    case State::IDLE:
      break;
  }
  if (this->result_ == Result::NONE || millis() - this->result_ms_ > RESULT_SHOWN_MS) return "";
  return result_button_text(this->result_);
}

void LabelPrinter::finish_(Result result) {
  if (result == Result::PRINTED || result == Result::SENT) {
    ESP_LOGI(TAG, "Spool %d: %s (%u bytes)", this->spool_id_, result_log_text(result),
             (unsigned) this->driver_->size());
  } else {
    ESP_LOGW(TAG, "Spool %d: %s", this->spool_id_, result_log_text(result));
  }
  this->driver_->end();
  this->rasterizer_.end();
  // Hand the buffers back; they're only needed while printing.
  std::string().swap(this->png_);
  this->raster_.clear();
  this->state_ = State::IDLE;
  this->result_ = result;
  this->result_ms_ = millis();
  this->transport_->close(result == Result::NOT_FOUND);
}

void LabelPrinter::on_reply(const uint8_t *data, size_t len) {
  if (this->state_ != State::SEND && this->state_ != State::WAIT_DONE) return;
  Result r = this->driver_->on_reply(data, len);
  if (r != Result::NONE) this->finish_(r);
}

void LabelPrinter::loop() {
  const uint32_t now = millis();
  switch (this->state_) {
    case State::IDLE:
      return;

    case State::FETCH: {
      auto got = this->api_->poll_label_png(this->png_);
      if (got == bambuddy_api::BambuddyAPIComponent::LABEL_READY) {
        const PrinterInfo &info = this->driver_->info();
        if (!this->rasterizer_.begin(&this->raster_, info.head_dots, info.max_rows, this->x_offset_)) {
          this->finish_(Result::BAD_LABEL);
          return;
        }
        this->png_pos_ = 0;
        this->state_ = State::DECODE;
      } else if (got == bambuddy_api::BambuddyAPIComponent::LABEL_FAILED) {
        this->finish_(Result::NO_LABEL);
      } else if (now - this->state_ms_ > FETCH_TIMEOUT_MS) {
        this->api_->cancel_label_png();
        this->finish_(Result::NO_LABEL);
      }
      return;
    }

    case State::DECODE: {
      // A slice per loop keeps the UI responsive while it decodes.
      size_t n = std::min(DECODE_SLICE, this->png_.size() - this->png_pos_);
      if (n > 0) {
        int eaten = this->rasterizer_.feed(reinterpret_cast<const uint8_t *>(this->png_.data()) + this->png_pos_, n);
        if (eaten <= 0) {
          ESP_LOGW(TAG, "PNG decode failed: %s", eaten < 0 ? this->rasterizer_.error() : "stalled");
          this->finish_(Result::BAD_LABEL);
          return;
        }
        this->png_pos_ += eaten;
        if (this->png_pos_ < this->png_.size()) return;
      }
      if (!this->rasterizer_.ok()) {
        this->finish_(Result::BAD_LABEL);
        return;
      }
      this->rasterizer_.end();
      std::string().swap(this->png_);
      if (!this->driver_->begin(this->raster_)) {
        this->finish_(Result::BAD_LABEL);
        return;
      }
      this->state_ = State::CONNECT;  // keeps the fetch's start time for its timeout
      return;
    }

    case State::CONNECT:
      if (this->transport_->ready()) {
        ESP_LOGD(TAG, "Connected, sending %u bytes", (unsigned) this->driver_->size());
        this->transport_->send();
        this->state_ = State::SEND;
        this->state_ms_ = now;
      } else if (now - this->state_ms_ > CONNECT_TIMEOUT_MS) {
        this->finish_(Result::NOT_FOUND);
      }
      return;

    case State::SEND:
      switch (this->transport_->send_state()) {
        case LabelTransport::SendState::DONE:
          ESP_LOGD(TAG, "All %u bytes sent, waiting for the printer", (unsigned) this->driver_->size());
          if (!this->driver_->info().confirms || !this->transport_->has_replies()) {
            this->finish_(Result::SENT);
            return;
          }
          this->state_ = State::WAIT_DONE;
          this->state_ms_ = now;
          return;
        case LabelTransport::SendState::FAILED:
          this->finish_(Result::WRITE_FAILED);
          return;
        case LabelTransport::SendState::LOST:
          this->finish_(Result::LOST);
          return;
        default:
          if (now - this->state_ms_ > SEND_TIMEOUT_BASE_MS + this->driver_->size()) {
            ESP_LOGW(TAG, "Sending stalled");
            this->finish_(Result::WRITE_FAILED);
          }
          return;
      }

    case State::WAIT_DONE:
      // Disconnecting right after the last bytes can be the printer's way of
      // saying it's done.
      if (!this->transport_->ready() || now - this->state_ms_ > DONE_TIMEOUT_MS) this->finish_(Result::SENT);
      return;
  }
}

}  // namespace esphome::label_printer
