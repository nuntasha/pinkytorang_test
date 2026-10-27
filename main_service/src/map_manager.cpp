#include "pinkytorang/map_manager.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <yaml-cpp/yaml.h>

namespace pinky {

static bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool MapManager::load(const std::string& path, std::string& error) {
  nodes_.clear();
  index_.clear();
  zones_.clear();
  std::vector<std::string> problems;

  try {
    YAML::Node root = YAML::LoadFile(path);
    for (const YAML::Node& n : root["nodes"]) {
      MapNode m;
      m.id = n["id"].as<std::string>();
      m.type = n["type"].as<std::string>();
      if (n["stop_kind"]) m.stop_kind = n["stop_kind"].as<std::string>();
      if (n["table"]) m.table = n["table"].as<std::string>();
      if (n["pocket"]) m.pocket = n["pocket"].as<bool>();
      if (n["no_wait"]) m.no_wait = n["no_wait"].as<bool>();
      if (n["lock"]) m.lock = n["lock"].as<bool>();
      m.x = n["x"].as<double>();
      m.y = n["y"].as<double>();
      if (n["yaw"]) {
        m.yaw = n["yaw"].as<double>();
        m.has_yaw = true;
      }
      if (n["next"]) m.next = n["next"].as<std::vector<std::string>>();

      if (index_.count(m.id)) {
        problems.push_back(m.id + ": ID 중복");
        continue;
      }
      index_[m.id] = static_cast<int>(nodes_.size());
      nodes_.push_back(m);
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
    for (const std::string& nx : m.next) {
      if (!index_.count(nx)) problems.push_back(m.id + ": next에 없는 노드 " + nx);
    }
    if (m.type == "STOP" && !m.has_yaw) problems.push_back(m.id + ": STOP인데 yaw 없음");
    if (m.type == "STOP" && m.stop_kind.empty()) problems.push_back(m.id + ": STOP인데 stop_kind 없음");
    if (m.stop_kind == "SERVE" && m.table.empty()) problems.push_back(m.id + ": SERVE인데 table 없음");
  }
  for (const TwoWayZone& z : zones_) {
    for (const std::string& id : z.nodes) {
      if (!index_.count(id)) problems.push_back("구간 " + z.id + ": 없는 노드 " + id);
    }
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
