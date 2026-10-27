#include "pinkytorang/sim_fleet.hpp"

#include <algorithm>
#include <cmath>

#include "pinkytorang/log.hpp"

namespace pinky {

SimFleet::SimFleet(const Config& cfg, const MapManager& map, double speed_mps, double time_scale)
    : cfg_(cfg), map_(map), speed_(speed_mps), scale_(time_scale) {
  // 도킹 노드에 차례로 세워 둔다
  std::vector<std::string> docks = map_.nodesOfKind("DOCK");
  const double start_battery[] = {90.0, 75.0, 12.0};  // 세 번째 로봇은 충전부터 하게
  for (size_t i = 0; i < cfg_.robots.size(); ++i) {
    const MapNode* n = map_.node(docks[i % docks.size()]);
    Bot b;
    b.id = cfg_.robots[i];
    b.x = n->x;
    b.y = n->y;
    b.yaw = n->yaw;
    b.last_node = n->id;
    b.battery = start_battery[i % 3];
    bots_[b.id] = b;
  }
}

SimFleet::~SimFleet() { stop(); }

void SimFleet::start() {
  running_ = true;
  thread_ = std::thread([this]() { loop(); });
  logLine("SIM", "가상 로봇 " + std::to_string(bots_.size()) + "대 시작");
}

void SimFleet::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void SimFleet::sendMove(const std::string& robot_id, const MoveCmd& cmd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!bots_.count(robot_id)) return;
  Bot& b = bots_[robot_id];
  bool rev = (cmd.drive == Drive::REVERSE);
  if (cmd.append) {
    if (cmd.cmd_id != b.cmd_id) return;  // 다른 명령에 붙이려는 APPEND는 무시
  } else {
    b.path.clear();  // REPLACE: 경로 교체
    b.cmd_id = cmd.cmd_id;
    b.dock_wait = 0.0;
  }
  for (const WaypointCmd& w : cmd.waypoints) b.path.push_back({w, rev});
  b.running = true;
}

void SimFleet::sendControl(const std::string& robot_id, ControlCmd cmd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!bots_.count(robot_id)) return;
  Bot& b = bots_[robot_id];
  if (cmd == ControlCmd::PAUSE) b.paused = true;
  if (cmd == ControlCmd::RESUME) b.paused = false;
}

void SimFleet::sendDisplay(const std::string& robot_id, const DisplayCmd& cmd) {
  std::string text = robot_id + " \"" + cmd.text + "\"";
  if (cmd.countdown_sec > 0) text += " (" + std::to_string(cmd.countdown_sec) + "초)";
  logLine("LCD", text);
}

// 한 로봇을 dt초만큼 움직인다. 생긴 이벤트는 out에 담는다
void SimFleet::step(Bot& b, double dt, std::vector<PendingEvent>& out) {
  // 배터리: 도킹에 서 있으면 초당 2% 충전, 움직이면 초당 0.4% 소모
  const MapNode* at = map_.node(b.last_node);
  bool docked = !b.running && at && at->stop_kind == "DOCK";
  if (docked) b.battery = std::min(100.0, b.battery + 2.0 * dt);
  else if (b.running && !b.paused) b.battery = std::max(0.0, b.battery - 0.4 * dt);

  if (b.paused || !b.running) return;

  if (b.dock_wait > 0.0) {  // 정밀 도킹 중
    b.dock_wait -= dt;
    if (b.dock_wait <= 0.0) {
      b.running = false;
      out.push_back({b.id, EVENT_CMD_DONE, b.cmd_id, b.last_node});
    }
    return;
  }
  if (b.path.empty()) {
    b.running = false;
    out.push_back({b.id, EVENT_CMD_DONE, b.cmd_id, b.last_node});
    return;
  }

  double move = speed_ * dt;  // 이번에 갈 수 있는 거리
  while (move > 0.0 && !b.path.empty()) {
    WaypointCmd w = b.path.front().w;
    bool rev = b.path.front().reverse;
    double dx = w.x - b.x;
    double dy = w.y - b.y;
    double d = std::hypot(dx, dy);
    if (d > move) {  // 아직 못 닿음
      b.x += dx / d * move;
      b.y += dy / d * move;
      b.yaw = rev ? std::atan2(-dy, -dx) : std::atan2(dy, dx);
      return;
    }
    // 노드 도착
    b.x = w.x;
    b.y = w.y;
    move -= d;
    b.last_node = w.node_id;
    b.path.erase(b.path.begin());
    out.push_back({b.id, EVENT_NODE_REACHED, b.cmd_id, w.node_id});

    if (b.path.empty()) {  // 명령의 마지막 노드
      b.yaw = w.yaw;
      const MapNode* n = map_.node(w.node_id);
      if (n && (n->stop_kind == "DOCK" || n->stop_kind == "PICKUP")) {  // 정밀 정차 (마커 / 테이프)
        b.dock_wait = 1.0;
      } else {
        b.running = false;
        out.push_back({b.id, EVENT_CMD_DONE, b.cmd_id, w.node_id});
      }
      return;
    }
  }
}

void SimFleet::loop() {
  auto last = std::chrono::steady_clock::now();
  auto last_state = last;
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last).count() * scale_;
    last = now;

    // 1) 자물쇠 안에서 로봇을 움직이고 이벤트를 모은다
    std::vector<PendingEvent> events;
    std::vector<Bot> states;
    bool send_state = now - last_state >= std::chrono::milliseconds(200);  // 상태 5 Hz
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& pair : bots_) step(pair.second, dt, events);
      if (send_state) {
        for (auto& pair : bots_) states.push_back(pair.second);
      }
    }
    if (send_state) last_state = now;

    // 2) 자물쇠를 푼 뒤에 Task Manager에 알린다
    //    (Task Manager는 안에서 sendMove를 부르고, sendMove는 이 자물쇠를 잡는다.
    //     자물쇠를 쥔 채로 부르면 서로 기다리다 멈춘다 = 교착)
    if (!tasks_) continue;
    for (const PendingEvent& e : events) tasks_->onRobotEvent(e.robot_id, e.type, e.cmd_id, e.node, "");
    for (const Bot& b : states) {
      uint8_t st = b.paused ? 2 : (b.running ? 1 : 0);
      tasks_->onRobotState(b.id, b.x, b.y, b.yaw, st, b.last_node, static_cast<float>(b.battery));
    }
  }
}

}  // namespace pinky
