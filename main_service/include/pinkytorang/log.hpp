// 터미널 로그. 여러 스레드가 동시에 찍어도 줄이 섞이지 않게 자물쇠를 건다
#pragma once

#include <iostream>
#include <mutex>
#include <string>

#include "pinkytorang/types.hpp"

namespace pinky {

inline void logLine(const std::string& tag, const std::string& msg) {
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  std::cout << nowLocalHms() << " [" << tag << "] " << msg << std::endl;
}

}  // namespace pinky
