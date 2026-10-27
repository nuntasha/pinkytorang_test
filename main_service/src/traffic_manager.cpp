#include "pinkytorang/traffic_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

std::string TrafficManager::edgeKey(const std::string& a, const std::string& b) {
  return (a < b) ? a + "|" + b : b + "|" + a;  // 양방향이라 방향 없이 하나로
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
  r.route_start = lastSent(r);  // 포켓 출차 방향 판단용 (경로의 출발지)
  r.plan.clear();
  r.via = "";
  r.avoid.clear();
  r.waiting = false;
  logLine("TRAFFIC", id + " 목적지 " + node);
  checkArrival(r);              // 이미 거기 있으면 바로 도착
  if (!r.goal.empty()) tryExtend(r);
  return true;
}

void TrafficManager::clearGoal(const std::string& id) {
  Robot& r = robots_[id];
  r.goal = "";
  r.plan.clear();
  r.via = "";
  r.waiting = false;
}

bool TrafficManager::isStationary(const std::string& id) const {
  auto it = robots_.find(id);
  if (it == robots_.end()) return false;
  return it->second.committed.empty() && it->second.goal.empty();
}

// 보낸 노드가 없고 지금 노드가 목적지면 도착
std::string TrafficManager::goalOf(const std::string& id) const {
  auto it = robots_.find(id);
  if (it == robots_.end()) return "";
  return it->second.goal;
}

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
  r.via = "";
  r.waiting = false;
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
    // 규칙 7: 지나온 길(엣지)도 해제
    auto e = edge_lock_.find(edgeKey(prev, r.at));
    if (e != edge_lock_.end() && e->second == id) edge_lock_.erase(e);
    if (r.at == r.via) r.via = "";  // 비켜 준 칸에 도착
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
  r.cmd_active = false;
  publish(r, r.committed, false);
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

// seg의 노드와, 거기로 가는 길(엣지)이 전부 비었는지. 막혔으면 막은 노드 · 로봇을 알려 준다
bool TrafficManager::canLock(const Robot& r, const std::vector<std::string>& seg, std::string& blocker,
                             std::string& blocker_robot, std::string& blocked_node) const {
  std::string prev = lastSent(r);
  for (const std::string& n : seg) {
    auto e = edge_lock_.find(edgeKey(prev, n));
    if (e != edge_lock_.end() && e->second != r.id) {
      blocker = prev + "-" + n + " 길(" + e->second + ")";
      blocker_robot = e->second;
      blocked_node = n;
      return false;
    }
    if (map_.node(n)->lock) {
      auto it = lock_.find(n);
      if (it != lock_.end() && it->second != r.id) {
        blocker = n + "(" + it->second + ")";
        blocker_robot = it->second;
        blocked_node = n;
        return false;
      }
    }
    prev = n;
  }
  return true;
}

// 보낸 경로 끝에서 목적지까지. 우회 중이면 막힌 노드를 빼고, 비켜 주는 중이면 옆 칸을 먼저 들른다
std::vector<std::string> TrafficManager::planPath(const Robot& r) const {
  std::string from = lastSent(r);
  std::set<std::string> ban;
  if (!r.avoid.empty() && Clock::now() < r.avoid_until) ban = r.avoid;

  if (!r.via.empty() && r.via != from) {
    std::vector<std::string> a = map_.shortestPathAvoid(from, r.via, ban);
    std::vector<std::string> b = map_.shortestPathAvoid(r.via, r.goal, ban);
    if (b.empty()) b = map_.shortestPath(r.via, r.goal);
    if (!a.empty() && !b.empty()) {
      a.insert(a.end(), b.begin() + 1, b.end());
      return a;
    }
  }
  std::vector<std::string> path = map_.shortestPathAvoid(from, r.goal, ban);
  if (path.empty()) path = map_.shortestPath(from, r.goal);  // 우회 길이 사라졌으면 원래 길
  return path;
}

// 서로 기다리는 고리가 있나 (A가 B를, B가 A를 기다림 또는 더 긴 고리)
bool TrafficManager::inWaitCycle(const std::string& start) const {
  std::string cur = start;
  for (size_t step = 0; step <= robots_.size(); ++step) {
    auto it = robots_.find(cur);
    if (it == robots_.end() || !it->second.waiting || it->second.waiting_for.empty()) return false;
    cur = it->second.waiting_for;
    if (cur == start) return true;
  }
  return false;
}

// 교착인데 우회 길도 없으면: 지금 서 있는 노드 옆의 빈 칸으로 비켜 준다 (상대가 지나갈 노드는 피함)
bool TrafficManager::sideStep(Robot& r, const Robot& other) {
  if (!r.committed.empty() || r.cmd_active) return false;  // 서 있을 때만
  const MapNode* here = map_.node(r.at);
  for (const std::string& n : here->next) {
    if (map_.node(n)->stop_kind == "DOCK") continue;
    if (lock_.count(n) && lock_[n] != r.id) continue;
    if (edge_lock_.count(edgeKey(r.at, n))) continue;
    if (contains(other.committed, n) || contains(other.plan, n) || n == other.goal) continue;
    r.via = n;
    logLine("TRAFFIC", r.id + " 교착 -> " + n + " 로 비켜 줌 (" + other.id + " 먼저)");
    return true;
  }
  return false;
}

// 막혔을 때 우회할지 기다릴지 정한다. 우회(또는 비켜 주기)를 정했으면 true
bool TrafficManager::resolveBlock(Robot& r, const std::string& blocker_robot, const std::string& blocked_node,
                                  const std::vector<std::string>& path) {
  Clock::time_point now = Clock::now();
  if (!r.waiting || r.waiting_for != blocker_robot) {
    r.waiting = true;
    r.waiting_for = blocker_robot;
    r.wait_since = now;
  }
  const Robot& b = robots_[blocker_robot];
  bool parked = b.goal.empty() || b.hold;   // 앞 로봇이 일하는 중이거나 멈춤 -> 금방 안 비킴
  bool cycle = inWaitCycle(r.id);
  double waited = std::chrono::duration<double>(now - r.wait_since).count();
  std::string target = r.via.empty() ? r.goal : r.via;

  // 다른 로봇이 잡고 있는 노드를 전부 빼고 다시 찾아 본다 (막힌 노드가 목적지 자체면 우회 불가)
  if (blocked_node != target) {
    std::set<std::string> ban;
    for (const auto& pair : lock_) {
      if (pair.second != r.id) ban.insert(pair.first);
    }
    ban.insert(blocked_node);
    std::vector<std::string> alt = map_.shortestPathAvoid(lastSent(r), target, ban);
    bool alt_ok = alt.size() >= 2 && !contains(alt, blocked_node);
    if (alt_ok) {
      double extra = map_.pathLength(alt) - map_.pathLength(path);
      bool go = parked || cycle || extra <= cfg_.detour_max_m || waited >= cfg_.detour_wait_sec;
      if (go) {
        r.avoid = ban;
        r.avoid_until = now + std::chrono::milliseconds(static_cast<int>(cfg_.avoid_sec * 1000));
        r.waiting = false;
        char buf[160];
        snprintf(buf, sizeof(buf), " 우회 (+%.2f m, %s 때문, %s)", extra, blocker_robot.c_str(),
                 parked ? "상대가 서 있음" : cycle ? "교착" : waited >= cfg_.detour_wait_sec ? "오래 기다림" : "짧은 우회");
        logLine("TRAFFIC", r.id + buf);
        return true;
      }
    }
  }
  // 교착인데 우회 길도 없으면 한 대가 비켜 준다 (이름이 뒤인 로봇 먼저, 못 비키면 상대가)
  if (cycle && r.via.empty()) {
    if (r.id > blocker_robot || b.via.empty()) {
      if (sideStep(r, b)) {
        r.waiting = false;
        return true;
      }
    }
  }
  return false;  // 기다림
}

bool TrafficManager::tryExtend(Robot& r) {
  if (!r.localized || r.hold || r.goal.empty()) return false;

  bool was_empty = r.committed.empty();
  std::vector<std::string> batch;

  int resolves = 0;
  while (committedLockCount(r) < cfg_.lock_lookahead) {
    // 보낸 경로 끝에서 목적지까지 다시 계산 (늦게 결정)
    std::vector<std::string> path = planPath(r);
    if (path.size() <= 1) break;
    r.plan.assign(path.begin() + 1, path.end());

    std::vector<std::string> seg = buildSegment(r);
    if (seg.empty()) break;

    std::string blocker, blocker_robot, blocked_node;
    if (!canLock(r, seg, blocker, blocker_robot, blocked_node)) {
      // 규칙 8: 우회할지 기다릴지 (한 번 호출에 두 번까지만 다시 계산)
      if (resolves < 2 && resolveBlock(r, blocker_robot, blocked_node, path)) {
        resolves++;
        continue;
      }
      std::string msg = "[" + join(seg) + "] 대기, " + blocker + " 사용 중";
      if (msg != r.last_wait_log) {  // 같은 로그 반복 방지
        logLine("TRAFFIC", r.id + " " + msg);
        r.last_wait_log = msg;
      }
      break;
    }
    // 전부 비었으니 한 번에 잡는다 (노드 + 그 노드로 가는 길)
    std::string prev = lastSent(r);
    for (const std::string& n : seg) {
      if (map_.node(n)->lock) lock_[n] = r.id;
      edge_lock_[edgeKey(prev, n)] = r.id;
      prev = n;
      r.committed.push_back(n);
      batch.push_back(n);
    }
    r.waiting = false;
    r.waiting_for = "";
    r.plan.erase(r.plan.begin(), r.plan.begin() + static_cast<long>(seg.size()));
    r.last_wait_log = "";
  }
  if (batch.empty()) return false;

  // 규칙 5, 6
  bool leaving_pocket = was_empty && map_.node(r.at)->pocket;
  bool append = r.cmd_active && !leaving_pocket;
  publish(r, batch, append);
  return true;
}

// 규칙 5: Move 하나에는 방향 하나 (fleet_msgs Move.drive, README 4.5 · 4.9)
//   길마다 후진인지 보고(MapManager::driveReverse), 방향이 바뀌는 곳과 포켓에 드나드는 곳에서 Move를 나눈다
//   나뉜 뒤쪽 Move는 같은 cmd_id로 APPEND. 로봇은 방향이 바뀌는 노드에서 멈추고 돌아서 이어 간다
//   yaw = 로봇 앞이 보는 방향: 다음 길 쪽 (후진이면 +180도). 목적지가 yaw 정해진 정차점이면 그 값
void TrafficManager::publish(Robot& r, const std::vector<std::string>& nodes, bool append) {
  if (nodes.empty()) return;

  // nodes는 항상 committed의 뒤쪽이다. 첫 노드 바로 앞 = 앞서 보낸 마지막 노드 (없으면 지금 노드)
  std::string before = r.at;
  if (r.committed.size() > nodes.size()) before = r.committed[r.committed.size() - nodes.size() - 1];

  std::vector<std::string> seq = nodes;  // 보낼 노드 + 그 다음 계획 노드 (yaw 계산용)
  if (!r.plan.empty()) seq.push_back(r.plan.front());
  auto from = [&](size_t k) { return (k == 0) ? before : seq[k - 1]; };  // k번째 길 = from(k) -> seq[k]
  std::vector<bool> rev(seq.size(), false);
  for (size_t k = 0; k < seq.size(); ++k) rev[k] = map_.driveReverse(from(k), seq[k], r.route_start, r.goal);

  auto yawAt = [&](size_t i) {
    const MapNode* n = map_.node(seq[i]);
    if (n->has_yaw && i + 1 >= seq.size() && seq[i] == r.goal) return n->yaw;
    double h = 0.0;
    bool back = false;
    if (i + 1 < seq.size()) {
      h = map_.heading(seq[i], seq[i + 1]);
      back = rev[i + 1];
    } else if (from(i) != seq[i]) {
      h = map_.heading(from(i), seq[i]);
      back = rev[i];
    }
    if (back) h += (h > 0.0) ? -M_PI : M_PI;
    return h;
  };

  size_t start = 0;
  while (start < nodes.size()) {
    size_t end = start;
    while (end + 1 < nodes.size() && rev[end + 1] == rev[start] &&
           !map_.isPocketEdge(from(end + 1), seq[end + 1], r.route_start, r.goal))
      end++;

    bool app = (start == 0) ? append : true;
    if (!app) r.cmd_id = next_cmd_id_++;

    MoveCmd cmd;
    cmd.cmd_id = r.cmd_id;
    cmd.append = app;
    cmd.drive = rev[start] ? Drive::REVERSE : Drive::FORWARD;

    std::string text;
    for (size_t i = start; i <= end; ++i) {
      const MapNode* n = map_.node(nodes[i]);
      WaypointCmd w;
      w.node_id = n->id;
      w.x = n->x;
      w.y = n->y;
      w.yaw = yawAt(i);
      cmd.waypoints.push_back(w);
      if (i > start) text += ", ";
      text += n->id;
    }
    logLine("TRAFFIC", r.id + " Move(cmd " + std::to_string(cmd.cmd_id) + ", " + (app ? "APPEND" : "REPLACE") +
                           ", " + (rev[start] ? "REVERSE" : "FORWARD") + ", [" + text + "])");
    if (fleet_) fleet_->sendMove(r.id, cmd);
    start = end + 1;
  }
  r.cmd_active = true;
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
