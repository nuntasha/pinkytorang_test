#include "pinkytorang/types.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace pinky {

std::string toString(TaskKind v) {
  if (v == TaskKind::SERVE) return "SERVE";
  return "GUIDE";
}

std::string toString(Stage v) {
  switch (v) {
    case Stage::COOKING: return "COOKING";
    case Stage::WAITING: return "WAITING";
    case Stage::ASSIGNED: return "ASSIGNED";
    case Stage::TO_PICKUP: return "TO_PICKUP";
    case Stage::AT_PICKUP: return "AT_PICKUP";
    case Stage::TO_TABLE: return "TO_TABLE";
    case Stage::AT_TABLE: return "AT_TABLE";
    case Stage::TO_DOCK: return "TO_DOCK";
    case Stage::DONE: return "DONE";
    case Stage::FAILED: return "FAILED";
    case Stage::CANCELED: return "CANCELED";
  }
  return "?";
}

std::string toString(Confirm v) {
  switch (v) {
    case Confirm::NONE: return "NONE";
    case Confirm::LOADED: return "LOADED";
    case Confirm::RECEIVED: return "RECEIVED";
    case Confirm::SEATED: return "SEATED";
  }
  return "?";
}

std::string toString(ControlCmd v) {
  if (v == ControlCmd::PAUSE) return "PAUSE";
  if (v == ControlCmd::RESUME) return "RESUME";
  return "CANCEL";
}

std::string toString(TableState v) {
  if (v == TableState::FREE) return "FREE";
  if (v == TableState::RESERVED) return "RESERVED";
  return "OCCUPIED";
}

std::string robotStateName(uint8_t state) {
  switch (state) {
    case 0: return "IDLE";
    case 1: return "MOVING";
    case 2: return "PAUSED";
    case 3: return "BLOCKED";
    default: return "ERROR";
  }
}

bool parseConfirm(const std::string& s, Confirm& out) {
  if (s == "LOADED") { out = Confirm::LOADED; return true; }
  if (s == "RECEIVED") { out = Confirm::RECEIVED; return true; }
  if (s == "SEATED") { out = Confirm::SEATED; return true; }
  return false;
}

bool parseControlCmd(const std::string& s, ControlCmd& out) {
  if (s == "PAUSE") { out = ControlCmd::PAUSE; return true; }
  if (s == "RESUME") { out = ControlCmd::RESUME; return true; }
  if (s == "CANCEL") { out = ControlCmd::CANCEL; return true; }
  return false;
}

bool isEnded(Stage s) {
  return s == Stage::DONE || s == Stage::FAILED || s == Stage::CANCELED;
}

std::string nowIsoUtc() {
  using namespace std::chrono;
  system_clock::time_point t = system_clock::now();
  long long ms = duration_cast<milliseconds>(t.time_since_epoch()).count() % 1000;
  std::time_t tt = system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
  return os.str();
}

std::string nowLocalHms() {
  std::time_t tt = std::time(nullptr);
  std::tm tm{};
  localtime_r(&tt, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%H:%M:%S");
  return os.str();
}

}  // namespace pinky
