// Client Link Manager: 고객 UI · 매장 UI 와의 HTTP + WebSocket (Crow)
// - HTTP 요청이 오면 Task Manager 함수를 불러 결과를 JSON으로 돌려준다 (판단하지 않음)
// - Task Manager가 pushXxx를 부르면 WebSocket으로 UI에 보낸다
// - web/ 폴더의 화면 파일을 브라우저에 보내 준다 (/c?table=T3, /store, /kiosk)
#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "crow_all.h"

#include "pinkytorang/config.hpp"
#include "pinkytorang/ports.hpp"
#include "pinkytorang/task_manager.hpp"

namespace pinky {

class ClientLinkManager : public IClientOut {
 public:
  ClientLinkManager(const Config& cfg, TaskManager& tasks);
  ~ClientLinkManager();

  void start();  // HTTP 서버를 별도 스레드에서 시작
  void stop();

  // IClientOut (Task Manager가 부름)
  void pushSnapshot(const SnapshotView& snap) override;
  void pushTaskUpdate(const TaskView& task) override;
  void pushConfirmNeeded(const ConfirmView& c) override;
  void pushConfirmClosed(const ConfirmView& c, const std::string& by) override;
  void pushAlert(const std::string& level, const std::string& robot_id, const std::string& message) override;

 private:
  void setupRoutes();
  crow::response serveFile(const std::string& rel);
  void sendToStore(const std::string& text);
  void sendToTable(const std::string& table, const std::string& text);

  const Config& cfg_;
  TaskManager& tasks_;
  crow::SimpleApp app_;
  std::thread thread_;

  // 보내는 중에 연결이 끊겨 onclose가 같은 스레드에서 불려도 막히지 않게 recursive_mutex
  std::recursive_mutex mutex_;
  std::set<crow::websocket::connection*> store_conns_;
  std::map<crow::websocket::connection*, std::string> customer_conns_;  // 연결 -> 테이블
  std::string last_snapshot_;                       // 새로 붙은 매장 UI에 바로 보냄

  struct OpenConfirm {
    std::string json;
    std::string table;
    bool guest = false;  // 손님 확인(RECEIVED, SEATED)이면 고객 UI에도 보냄
  };
  std::map<int, OpenConfirm> open_confirms_;        // 새로 붙은 화면에 다시 띄울 확인 버튼
};

}  // namespace pinky
