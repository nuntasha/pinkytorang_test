#include "pinkytorang/map_manager.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <yaml-cpp/yaml.h>

namespace pinky {

static bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// node_graph.yaml 형식 (두 가지 모두 읽음, 섞어 써도 됨)
//   fleet_msgs 형식 (fleet_msgs/docs/node_graph.yaml, 팀 기준 문서)
//     nodes: {id, type: PASS/STOP, stop_kind, table, lock, pocket, entry: FORWARD/REVERSE, x, y, yaw, next: [..]}
//       next = 갈 수 있는 다음 노드 (양방향은 양쪽에, 일방통행은 한쪽에만)
//       pocket + entry: 포켓 진입 방향. FORWARD(기본) = 전진 진입 · 후진 출차, REVERSE = 후진 진입 · 전진 출차
//       (드나드는 길의 방향은 경로의 출발 · 목적지로 정한다: driveReverse)
//   짧은 형식
//     nodes : {id, x, y}  + 정차점이면 kind(QUEUE/PICKUP/SERVE/DOCK), lock 기본 true
//     edges : 양방향 길. [A, B, C] = A-B, B-C 를 양쪽으로
//     oneway: 일방통행 길. [A, B, C] = A->B->C
//   공통
//     yaw: 방향이 정해진 정차점 (도킹 필수). 그 노드가 목적지일 때만 맞춘다
//     SERVE 노드의 테이블 이름은 table (없으면 id)
//     reverse (선택): 후진 구간. [A, B, C] = A->B, B->C 를 후진으로. 첫 노드 · 끝 노드는 lock 노드
//     two_way_zones (선택): 들어갈 때 한 번에 잡는 1차선 구간
bool MapManager::load(const std::string& path, std::string& error) {
  nodes_.clear();
  index_.clear();
  zones_.clear();
  reverse_edges_.clear();
  std::vector<std::vector<std::string>> reverse_chains;
  std::vector<std::pair<std::string, std::vector<std::string>>> next_lists;  // fleet_msgs 형식의 next
  std::vector<std::string> problems;

  // a -> b 길 추가 (같은 길 두 번은 무시)
  auto addNext = [&](const std::string& a, const std::string& b) {
    if (!index_.count(a) || !index_.count(b)) {
      problems.push_back("길에 없는 노드: " + a + " - " + b);
      return;
    }
    std::vector<std::string>& next = nodes_[index_[a]].next;
    if (std::find(next.begin(), next.end(), b) == next.end()) next.push_back(b);
  };

  try {
    YAML::Node root = YAML::LoadFile(path);
    for (const YAML::Node& n : root["nodes"]) {
      MapNode m;
      m.id = n["id"].as<std::string>();
      if (n["kind"]) m.stop_kind = n["kind"].as<std::string>();
      else if (n["stop_kind"]) m.stop_kind = n["stop_kind"].as<std::string>();  // fleet_msgs 형식
      m.type = m.stop_kind.empty() ? "PASS" : "STOP";  // 정차 종류가 있으면 STOP
      if (n["type"] && n["type"].as<std::string>() == "STOP" && m.stop_kind.empty())
        problems.push_back(m.id + ": type STOP인데 stop_kind 없음");
      if (n["entry"]) {
        std::string entry = n["entry"].as<std::string>();
        if (entry == "REVERSE") m.entry_reverse = true;
        else if (entry != "FORWARD") problems.push_back(m.id + ": 모르는 entry " + entry);
      }
      if (n["next"]) next_lists.push_back({m.id, n["next"].as<std::vector<std::string>>()});
      if (n["table"]) m.table = n["table"].as<std::string>();
      else if (m.stop_kind == "SERVE") m.table = m.id;   // 테이블 정차점 id = 테이블 이름
      if (n["pocket"]) m.pocket = n["pocket"].as<bool>();
      if (n["no_wait"]) m.no_wait = n["no_wait"].as<bool>();
      if (n["lock"]) m.lock = n["lock"].as<bool>();
      m.x = n["x"].as<double>();
      m.y = n["y"].as<double>();
      if (n["yaw"]) {
        m.yaw = n["yaw"].as<double>();
        m.has_yaw = true;
      }

      if (index_.count(m.id)) {
        problems.push_back(m.id + ": ID 중복");
        continue;
      }
      index_[m.id] = static_cast<int>(nodes_.size());
      nodes_.push_back(m);
    }
    for (const auto& nl : next_lists) {
      for (const std::string& b : nl.second) addNext(nl.first, b);
    }
    if (root["edges"]) {
      for (const YAML::Node& e : root["edges"]) {
        std::vector<std::string> chain = e.as<std::vector<std::string>>();
        for (size_t i = 1; i < chain.size(); ++i) {
          addNext(chain[i - 1], chain[i]);
          addNext(chain[i], chain[i - 1]);
        }
      }
    }
    if (root["oneway"]) {
      for (const YAML::Node& e : root["oneway"]) {
        std::vector<std::string> chain = e.as<std::vector<std::string>>();
        for (size_t i = 1; i < chain.size(); ++i) addNext(chain[i - 1], chain[i]);
      }
    }
    if (root["reverse"]) {
      for (const YAML::Node& e : root["reverse"]) reverse_chains.push_back(e.as<std::vector<std::string>>());
    }
    if (root["two_way_zones"]) {
      for (const YAML::Node& z : root["two_way_zones"]) {
        TwoWayZone tz;
        tz.id = z["id"].as<std::string>();
        tz.nodes = z["nodes"].as<std::vector<std::string>>();
        zones_.push_back(tz);
      }
    }
  } catch (const std::exception& e) {
    error = "node_graph 읽기 실패: " + path + " (" + e.what() + ")";
    return false;
  }

  // ---- 검사
  for (const MapNode& m : nodes_) {
    const std::string& k = m.stop_kind;
    if (!k.empty() && k != "QUEUE" && k != "PICKUP" && k != "SERVE" && k != "DOCK")
      problems.push_back(m.id + ": 모르는 kind " + k);
    if (k == "DOCK" && !m.has_yaw) problems.push_back(m.id + ": 도킹인데 yaw 없음");
    if (m.next.empty()) problems.push_back(m.id + ": 나가는 길이 없음");
  }
  for (const TwoWayZone& z : zones_) {
    for (const std::string& id : z.nodes) {
      if (!index_.count(id)) problems.push_back("구간 " + z.id + ": 없는 노드 " + id);
    }
  }
  for (const std::vector<std::string>& chain : reverse_chains) {
    if (chain.size() < 2) {
      problems.push_back("후진 구간: 노드가 2개 이상이어야 함");
      continue;
    }
    bool ok = true;
    for (size_t i = 1; i < chain.size(); ++i) {
      const MapNode* a = node(chain[i - 1]);
      if (!a || !node(chain[i]) || !contains(a->next, chain[i])) {
        problems.push_back("후진 구간: 없는 길 " + chain[i - 1] + " -> " + chain[i]);
        ok = false;
      }
    }
    if (!ok) continue;
    // 방향이 바뀌는 첫 · 끝 노드에서 Move를 끊으므로 lock 노드여야 한다
    if (!node(chain.front())->lock) problems.push_back("후진 구간: 시작 " + chain.front() + "이 lock 노드가 아님");
    if (!node(chain.back())->lock) problems.push_back("후진 구간: 끝 " + chain.back() + "이 lock 노드가 아님");
    for (size_t i = 1; i < chain.size(); ++i) reverse_edges_.insert({chain[i - 1], chain[i]});
  }
  if (nodesOfKind("PICKUP").empty()) problems.push_back("PICKUP 노드 없음");
  if (nodesOfKind("DOCK").empty()) problems.push_back("DOCK 노드 없음");

  if (!problems.empty()) {
    error = "node_graph 검사 실패 (" + path + ")";
    for (const std::string& p : problems) error += "\n  - " + p;
    return false;
  }
  return true;
}

const MapNode* MapManager::node(const std::string& id) const {
  auto it = index_.find(id);
  if (it == index_.end()) return nullptr;
  return &nodes_[it->second];
}

std::string MapManager::tableNode(const std::string& table) const {
  for (const MapNode& m : nodes_) {
    if (m.stop_kind == "SERVE" && m.table == table) return m.id;
  }
  return "";
}

std::vector<std::string> MapManager::nodesOfKind(const std::string& kind) const {
  std::vector<std::string> out;
  for (const MapNode& m : nodes_) {
    if (m.stop_kind == kind) out.push_back(m.id);
  }
  return out;
}

// 경로 위 a -> b를 후진으로 가는지 (fleet_msgs README 4.5 · 4.9)
//   1. reverse 구간에 적힌 길: 후진
//   2. 경로의 목적지(goal)가 pocket이면 마지막 길은 그 pocket의 entry 방향 (D1~D4 = REVERSE, 바구니부터)
//   3. 경로의 출발지(start)가 pocket이면 첫 길은 entry의 반대 (REVERSE로 들어간 도킹은 전진으로 나옴)
//   4. 나머지 (지나가기만 하는 pocket 포함, 예: D2로 가며 지나는 D1): 전진
bool MapManager::driveReverse(const std::string& a, const std::string& b, const std::string& start,
                              const std::string& goal) const {
  if (isReverseEdge(a, b)) return true;
  const MapNode* nb = node(b);
  if (nb && b == goal && nb->pocket) return nb->entry_reverse;
  const MapNode* na = node(a);
  if (na && a == start && na->pocket) return !na->entry_reverse;
  return false;
}

// 포켓에 드나드는 길이면 Move를 따로 끊는다 (로봇이 정밀 도킹 / 출차를 따로 하게)
bool MapManager::isPocketEdge(const std::string& a, const std::string& b, const std::string& start,
                              const std::string& goal) const {
  const MapNode* na = node(a);
  const MapNode* nb = node(b);
  return (na && a == start && na->pocket) || (nb && b == goal && nb->pocket);
}

bool MapManager::isReverseEdge(const std::string& a, const std::string& b) const {
  return reverse_edges_.count({a, b}) > 0;
}

bool MapManager::inZone(const std::string& id) const {
  for (const TwoWayZone& z : zones_) {
    if (contains(z.nodes, id)) return true;
  }
  return false;
}

bool MapManager::isZoneEdge(const std::string& a, const std::string& b) const {
  for (const TwoWayZone& z : zones_) {
    if (contains(z.nodes, a) && contains(z.nodes, b)) return true;
  }
  return false;
}

bool MapManager::isZoneEndOnly(const std::string& id) const {
  bool found = false;
  for (const TwoWayZone& z : zones_) {
    if (!contains(z.nodes, id)) continue;
    found = true;
    if (id != z.nodes.front() && id != z.nodes.back()) return false;  // 이 구간에서는 한가운데
  }
  return found;
}

double MapManager::distance(const std::string& a, const std::string& b) const {
  const MapNode* na = node(a);
  const MapNode* nb = node(b);
  return std::hypot(nb->x - na->x, nb->y - na->y);
}

double MapManager::heading(const std::string& from, const std::string& to) const {
  const MapNode* a = node(from);
  const MapNode* b = node(to);
  return std::atan2(b->y - a->y, b->x - a->x);
}

// 다익스트라: 아직 확정 안 된 노드 중 가장 가까운 것을 하나씩 확정하면서 이웃 거리를 갱신
std::vector<std::string> MapManager::shortestPath(const std::string& from, const std::string& to) const {
  return shortestPathAvoid(from, to, std::set<std::string>());
}

std::vector<std::string> MapManager::shortestPathAvoid(const std::string& from, const std::string& to,
                                                       const std::set<std::string>& ban) const {
  std::vector<std::string> path;
  if (!node(from) || !node(to)) return path;
  if (from == to) {
    path.push_back(from);
    return path;
  }

  std::map<std::string, double> dist;
  std::map<std::string, std::string> prev;
  std::set<std::string> done;
  for (const MapNode& m : nodes_) dist[m.id] = 1e9;
  dist[from] = 0.0;

  while (true) {
    // 확정 안 된 노드 중 거리가 가장 작은 것
    std::string u;
    double best = 1e9;
    for (const MapNode& m : nodes_) {
      if (!done.count(m.id) && dist[m.id] < best) {
        best = dist[m.id];
        u = m.id;
      }
    }
    if (u.empty() || u == to) break;
    done.insert(u);
    for (const std::string& v : node(u)->next) {
      if (v != to && ban.count(v)) continue;  // 막힌 노드는 건너뜀
      double d = dist[u] + distance(u, v);
      if (d < dist[v]) {
        dist[v] = d;
        prev[v] = u;
      }
    }
  }

  if (!prev.count(to)) return path;  // 경로 없음
  for (std::string cur = to; cur != from; cur = prev[cur]) path.push_back(cur);
  path.push_back(from);
  std::reverse(path.begin(), path.end());
  return path;
}

double MapManager::pathLength(const std::vector<std::string>& path) const {
  double sum = 0.0;
  for (size_t i = 1; i < path.size(); ++i) sum += distance(path[i - 1], path[i]);
  return sum;
}

std::string MapManager::nearestNode(double x, double y, double max_dist) const {
  std::string best;
  double best_d = max_dist;
  for (const MapNode& m : nodes_) {
    double d = std::hypot(m.x - x, m.y - y);
    if (d <= best_d) {
      best_d = d;
      best = m.id;
    }
  }
  return best;
}

MapView MapManager::view(double board_w, double board_h) const {
  MapView v;
  v.board_w = board_w;
  v.board_h = board_h;
  for (const MapNode& m : nodes_) {
    MapNodeView nv;
    nv.id = m.id;
    nv.type = m.type;
    nv.stop_kind = m.stop_kind;
    nv.table = m.table;
    nv.x = m.x;
    nv.y = m.y;
    v.nodes.push_back(nv);
    for (const std::string& nx : m.next) {
      MapEdgeView e;
      e.from = m.id;
      e.to = nx;
      v.edges.push_back(e);
    }
  }
  return v;
}

}  // namespace pinky
