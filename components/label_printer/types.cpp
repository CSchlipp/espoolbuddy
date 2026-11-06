#include "types.h"

namespace esphome::label_printer {

const char *result_log_text(Result result) {
  switch (result) {
    case Result::PRINTED:
      return "printed";
    case Result::SENT:
      return "sent, no confirmation";
    case Result::NO_LABEL:
      return "no label from Bambuddy";
    case Result::BAD_LABEL:
      return "label doesn't fit";
    case Result::NOT_FOUND:
      return "printer not found";
    case Result::LOST:
      return "printer disconnected";
    case Result::WRITE_FAILED:
      return "write failed";
    case Result::COVER_OPEN:
      return "cover open";
    case Result::NO_PAPER:
      return "out of labels";
    case Result::PRINTER_ERROR:
      return "printer error";
    default:
      return "none";
  }
}

const char *result_button_text(Result result) {
  switch (result) {
    case Result::PRINTED:
      return "Printed";
    case Result::SENT:
      return "Sent to printer";
    case Result::NO_LABEL:
      return "No label from Bambuddy";
    case Result::BAD_LABEL:
      return "Label didn't fit";
    case Result::NOT_FOUND:
      return "Printer not found";
    case Result::LOST:
      return "Printer disconnected";
    case Result::WRITE_FAILED:
      return "Printing failed";
    case Result::COVER_OPEN:
      return "Printer cover open";
    case Result::NO_PAPER:
      return "Out of labels";
    case Result::PRINTER_ERROR:
      return "Printer error";
    default:
      return "";
  }
}

}  // namespace esphome::label_printer
