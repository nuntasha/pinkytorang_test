#include "pinkytorang/traffic_manager.hpp"

#include <algorithm>
#include <ctime>

#include "pinkytorang/log.hpp"

namespace pinky {

static bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0) s += ", ";
    s += v[i];
  }
  return s;
}

TrafficManager::TrafficManager(const Config& cfg, const MapManager& map) : cfg_(cfg), map_(map) {
  // 서버를 다시 켜도 예전 cmd_id와 겹치지 않게 현재 시각에서 시작
  next_cmd_id_ = static_cast<uint32_t>((std::time(nullptr) % 1000000) * 100 + 1);
}

void TrafficManager::addRobot(const std::string& id) {
  Robot r;
  r.id = id;
  robots_[id] = r;
}

std::string TrafficManager::lastSent(const Robot& r) const {
  if (r.committed.empty()) return r.at;
  return r.committed.back();
}

// ---------------------------------------------------------------- 위치

bool TrafficManager::localize(const std::string& id, const std::string& last_node, double x, double y,
                              std::string& reason) {
  Robot& r = robots_[id];
  if (r.localized) return true;

  std::string n;
  if (!last_node.empty() && map_.node(last_node)) n = last_node;
  else n = map_.nearestNode(x, y, cfg_.localize_radius_m);
  if (n.empty()) {
    reason = "위치를 노드에 맞출 수 없음 (last_node='" + last_node + "')";
    return false;
  }
  // 운영 규칙: 로봇은 도킹 위에서 켠다. 처음 위치가 도킹이 아니면 인정하지 않는다
  if (cfg_.require_dock_start && map_.node(n)->stop_kind != "DOCK") {
    reason = n + " 는 도킹이 아님. 로봇을 도킹에 올리고 다시 켜 주세요";
    return false;
  }
  if (map_.node(n)->lock) {
    if (lock_.count(n) && lock_[n] != id) {
      reason = n + " 를 " + lock_[n] + " 가 이미 잡고 있음";
      return false;
    }
    lock_[n] = id;
  }
  r.at = n;
  r.localized = true;
  logLine("TRAFFIC", id + " 위치 확인: " + n);
  return true;
}

bool TrafficManager::isLocalized(const std::string& id) const {
  auto it = robots_.find(id);
  return it != robots_.end() && it->second.localized;
}

std::string TrafficManager::currentNode(const std::string& id) const {
  auto it = robots_.find(id);
  if (it == robots_.end()) return "";
  return it->second.at;
}

std::string TrafficManager::lastSentNode(const std::string& id) const {
  auto it = robots_.find(id);
  if (it == robots_.end()) return "";
  return lastSent(it->second);
}

// ---------------------------------------------------------------- 목적지

bool TrafficManager::setGoal(const std::string& id, const std::string& node) {
  Robot& r = robots_[id];
  if (!r.localized) return false;
  if (map_.shortestPath(lastSent(r), node).empty()) {
    logLine("TRAFFIC", id + " 경로 없음: " + lastSent(r) + " -> " + node);
    return false;
  }
  r.goal = node;
  r.plan.clear();
  logLine("TRAFFIC", id + " 목적지 " + node);
  checkArrival(r);              // 이미 거기 있으면 바로 도착
  if (!r.goal.empty()) tryExtend(r);
  return true;
}

void TrafficManager::clearGoal(const std::string& id) {
  Robot& r = robots_[id];
  r.goal = "";
  r.plan.clear();
}

bool TrafficManager::isStationary(const std::string& id) const {
  auto it = robots_.find(id);
  if (it == robots_.end()) return false;
  return it->second.committed.empty() && it->second.goal.empty();
}

// 보낸 노드가 없고 지금 노드가 목적지면 도착
void TrafficManager::checkArrival(Robot& r) {
  if (r.goal.empty() || !r.committed.empty() || r.cmd_active) return;
  if (r.at != r.goal) return;
  Arrival a;
  a.robot_id = r.id;
  a.node = r.at;
  arrivals_.push_back(a);
  logLine("TRAFFIC", r.id + " 도착 " + r.at);
  r.goal = "";
  r.plan.clear();
}

std::vector<Arrival> TrafficManager::takeArrivals() {
  std::vector<Arrival> out = arrivals_;
  arrivals_.clear();
  return out;
}

// ---------------------------------------------------------------- 로봇 이벤트

void TrafficManager::releaseLock(const std::string& node, const std::string& id) {
  if (lock_.count(node) && lock_[node] == id) lock_.erase(node);
}

void TrafficManager::onNodeReached(const std::string& id, const std::string& node) {
  Robot& r = robots_[id];
  if (!contains(r.committed, node)) return;  // 보낸 적 없는 노드 (중복 등)는 무시

  // node에 닿을 때까지 앞에서부터 하나씩 도착 처리
  while (!r.committed.empty()) {
    std::string prev = r.at;
    r.at = r.committed.front();
    r.committed.erase(r.committed.begin());
    // 규칙 2: 다음 노드에 도착했으니 이전 노드 lock 해제 (같은 노드를 또 지나갈 예정이면 유지)
    if (prev != r.at && !contains(r.committed, prev)) releaseLock(prev, id);
    if (r.at == node) break;
  }
  tick();  // 풀린 lock을 기다리던 로봇이 있을 수 있다
}

void TrafficManager::onCmdDone(const std::string& id, uint32_t cmd_id, const std::string& node) {
  Robot& r = robots_[id];
  if (cmd_id != r.cmd_id) return;  // 예전 명령의 CMD_DONE
  r.cmd_active = false;            // 이 뒤의 새 이동은 REPLACE
  onNodeReached(id, node);

  // 로봇은 끝났다는데 서버는 이미 더 붙여 보낸 경우 -> 남은 노드를 새 명령으로 다시 보냄
  if (!r.committed.empty() && !r.cmd_active) {
    resend(id);
    return;
  }
  checkArrival(r);
  if (!r.goal.empty() && !r.cmd_active) tryExtend(r);
}

void TrafficManager::onCmdFailed(const std::string& id) {
  robots_[id].cmd_active = false;
}

// ---------------------------------------------------------------- 멈춤 · 재전송

void TrafficManager::setHold(const std::string& id, bool hold) {
  Robot& r = robots_[id];
  bool was = r.hold;
  r.hold = hold;
  if (was && !hold) tryExtend(r);
}

void TrafficManager::resend(const std::string& id) {
  Robot& r = robots_[id];
  if (!r.localized) return;
  if (r.committed.empty()) {
    r.cmd_active = false;
    if (!r.goal.empty()) tryExtend(r);
    return;
  }
  const MapNode* here = map_.node(r.at);
  bool reverse = here->pocket && contains(here->next, r.committed.front());
  r.cmd_active = false;
  publish(r, r.committed, false, reverse);
}

// ---------------------------------------------------------------- 경로 늘리기

void TrafficManager::tick() {
  // 한 로봇이 lock을 잡으면 다른 로봇 상황도 바뀌니 몇 번 돌린다
  for (int round = 0; round < 3; ++round) {
    bool changed = false;
    for (auto& pair : robots_) {
      if (tryExtend(pair.second)) changed = true;
    }
    if (!changed) break;
  }
}

int TrafficManager::committedLockCount(const Robot& r) const {
  int n = 0;
  for (const std::string& id : r.committed) {
    if (map_.node(id)->lock) n++;
  }
  return n;
}

// plan 앞에서 이번에 한꺼번에 잡아야 할 노드 묶음을 만든다
std::vector<std::string> TrafficManager::buildSegment(const Robot& r) const {
  std::vector<std::string> seg;
  size_t i = 0;

  // 다음 lock 노드까지 담는다 (코너는 같이 담김)
  auto takeToLock = [&]() {
    while (i < r.plan.size()) {
      std::string n = r.plan[i];
      i++;
      seg.push_back(n);
      if (map_.node(n)->lock) return true;
    }
    return false;
  };

  if (!takeToLock()) return std::vector<std::string>();

  while (true) {
    std::string last = seg.back();
    std::string prev = (seg.size() >= 2) ? seg[seg.size() - 2] : lastSent(r);
    bool has_next = i < r.plan.size();
    bool next_zone_edge = has_next && map_.isZoneEdge(last, r.plan[i]);
    bool end_only = map_.isZoneEndOnly(last);

    // 규칙 3: 양방향 구간을 따라가는 중이면 끝 노드(또는 빠져나간 노드)까지 계속
    bool in_zone_run = map_.isZoneEdge(prev, last) || (map_.inZone(last) && (!end_only || next_zone_edge));
    if (in_zone_run && has_next && (next_zone_edge || !end_only)) {
      if (!takeToLock()) return std::vector<std::string>();
      continue;
    }
    // 규칙 4: no_wait 노드면 다음 lock 노드까지 함께
    if (map_.node(last)->no_wait && has_next) {
      if (!takeToLock()) return std::vector<std::string>();
      continue;
    }
    break;
  }
  return seg;
}

bool TrafficManager::canLock(const std::vector<std::string>& seg, const std::string& id, std::string& blocker) const {
  for (const std::string& n : seg) {
    if (!map_.node(n)->lock) continue;
    auto it = lock_.find(n);
    if (it != lock_.end() && it->second != id) {
      blocker = n + "(" + it->second + ")";
      return false;
    }
  }
  return true;
}

bool TrafficManager::tryExtend(Robot& r) {
  if (!r.localized || r.hold || r.goal.empty()) return false;

  bool was_empty = r.committed.empty();
  std::vector<std::string> batch;

  while (committedLockCount(r) < cfg_.lock_lookahead) {
    // 보낸 경로 끝에서 목적지까지 다시 계산 (늦게 결정)
    std::vector<std::string> path = map_.shortestPath(lastSent(r), r.goal);
    if (path.size() <= 1) break;
    r.plan.assign(path.begin() + 1, path.end());

    std::vector<std::string> seg = buildSegment(r);
    if (seg.empty()) break;

    std::string blocker;
    if (!canLock(seg, r.id, blocker)) {
      std::string msg = "[" + join(seg) + "] 대기, " + blocker + " 사용 중";
      if (msg != r.last_wait_log) {  // 같은 로그 반복 방지
        logLine("TRAFFIC", r.id + " " + msg);
        r.last_wait_log = msg;
      }
      break;
    }
    // 전부 비었으니 한 번에 잡는다
    for (const std::string& n : seg) {
      if (map_.node(n)->lock) lock_[n] = r.id;
      r.committed.push_back(n);
      batch.push_back(n);
    }
    r.plan.erase(r.plan.begin(), r.plan.begin() + static_cast<long>(seg.size()));
    r.last_wait_log = "";
  }
  if (batch.empty()) return false;

  // 규칙 5, 6
  bool leaving_pocket = was_empty && map_.node(r.at)->pocket;
  bool append = r.cmd_active && !leaving_pocket;
  publish(r, batch, append, leaving_pocket);
  return true;
}

// 노드에서 바라볼 방향: STOP 노드는 yaml 값, PASS 노드는 다음 노드 쪽
double TrafficManager::yawFor(const std::vector<std::string>& seq, size_t i, const std::string& before) const {
  const MapNode* n = map_.node(seq[i]);
  if (n->has_yaw) return n->yaw;
  if (i + 1 < seq.size()) return map_.heading(seq[i], seq[i + 1]);
  std::string prev = (i > 0) ? seq[i - 1] : before;
  if (!prev.empty() && prev != seq[i]) return map_.heading(prev, seq[i]);
  return 0.0;
}

void TrafficManager::publish(Robot& r, const std::vector<std::string>& nodes, bool append, bool reverse_first) {
  if (!append) r.cmd_id = next_cmd_id_++;

  MoveCmd cmd;
  cmd.cmd_id = r.cmd_id;
  cmd.append = append;

  std::vector<std::string> seq = nodes;           // yaw 계산용: 보낼 노드 + 그 다음 계획 노드
  if (!r.plan.empty()) seq.push_back(r.plan.front());

  std::string text;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const MapNode* n = map_.node(nodes[i]);
    WaypointCmd w;
    w.node_id = n->id;
    w.x = n->x;
    w.y = n->y;
    w.yaw = yawFor(seq, i, (i == 0) ? r.at : nodes[i - 1]);
    w.reverse = reverse_first && i == 0;
    cmd.waypoints.push_back(w);
    if (i > 0) text += ", ";
    text += n->id + (w.reverse ? "(rev)" : "");
  }
  r.cmd_active = true;

  logLine("TRAFFIC", r.id + " Move(cmd " + std::to_string(cmd.cmd_id) + ", " + (append ? "APPEND" : "REPLACE") +
                         ", [" + text + "])");
  if (fleet_) fleet_->sendMove(r.id, cmd);
}

// ---------------------------------------------------------------- 조회

std::vector<LockView> TrafficManager::locks() const {
  std::vector<LockView> out;
  for (const auto& pair : lock_) {
    LockView l;
    l.node = pair.first;
    l.robot_id = pair.second;
    out.push_back(l);
  }
  return out;
}

bool TrafficManager::isDockFree(const std::string& dock, const std::string& for_robot) const {
  auto it = lock_.find(dock);
  if (it != lock_.end() && it->second != for_robot) return false;  // 누가 서 있음
  for (const auto& pair : robots_) {
    if (pair.first != for_robot && pair.second.goal == dock) return false;  // 누가 가는 중
  }
  return true;
}

double TrafficManager::pathCost(const std::string& id, const std::string& target) const {
  auto it = robots_.find(id);
  if (it == robots_.end() || !it->second.localized) return -1;
  std::vector<std::string> path = map_.shortestPath(lastSent(it->second), target);
  if (path.empty()) return -1;
  return map_.pathLength(path);
}

}  // namespace pinky
