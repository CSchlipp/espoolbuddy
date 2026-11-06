#pragma once

#include "driver.h"

namespace esphome::label_printer {

// Where the core hands what the printer says.
class ReplyHandler {
 public:
  virtual void on_reply(const uint8_t *data, size_t len) = 0;
};

// Gets a driver's bytes to the printer: Bluetooth today, USB or a file later.
// The transport owns flow control (how many bytes per write, when the next
// may go), pulling the job from the driver with next().
class LabelTransport {
 public:
  enum class SendState : uint8_t { IDLE, SENDING, DONE, FAILED, LOST };

  virtual ~LabelTransport() = default;
  void set_driver(LabelDriver *driver) { this->driver_ = driver; }
  void set_reply_handler(ReplyHandler *handler) { this->reply_ = handler; }

  // Starts connecting, and finding the printer first when that's needed.
  virtual void open() = 0;
  // Connected and able to send.
  virtual bool ready() const = 0;
  // Still looking for the printer (for the button: "Looking for printer").
  virtual bool searching() const { return false; }
  // The printer can talk back on this connection.
  virtual bool has_replies() const = 0;
  // Sends the driver's whole job; follow it with send_state().
  virtual void send() = 0;
  virtual SendState send_state() const = 0;
  // Ends the job and lets go of the printer. unreachable: it never
  // answered, so a transport that found it may forget it.
  virtual void close(bool unreachable) = 0;

 protected:
  LabelDriver *driver_{nullptr};
  ReplyHandler *reply_{nullptr};
};

}  // namespace esphome::label_printer
