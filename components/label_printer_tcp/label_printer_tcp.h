#pragma once

#include "esphome/core/component.h"
#include "esphome/components/label_printer/transport.h"
#include "esphome/components/socket/tcp_client_link.h"

#include <cstdint>

namespace esphome::label_printer_tcp {

// A label printer on the network, reached over a raw TCP stream (port 9100,
// what printers call "raw" or "JetDirect" printing). Connects for a print
// and closes after it; TCP does the flow control, and the printer's replies
// come back on the same connection.
class LabelPrinterTCP : public Component, public label_printer::LabelTransport {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_host(const char *host) { this->link_.set_host(host); }
  void set_port(uint16_t port) { this->link_.set_port(port); }

  void open() override;
  bool ready() const override { return this->opened_ && this->link_.connected(); }
  bool has_replies() const override { return true; }
  void send() override;
  SendState send_state() const override { return this->send_state_; }
  void close(bool unreachable) override;

 protected:
  void read_replies_();
  void write_job_();

  socket::TcpClientLink link_;
  bool opened_{false};
  bool job_out_{false};  // the driver has handed over its last byte
  SendState send_state_{SendState::IDLE};
};

}  // namespace esphome::label_printer_tcp
