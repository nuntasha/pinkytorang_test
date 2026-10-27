#include "pinkytorang/dispatch_manager.hpp"

#include <algorithm>

namespace pinky {

static bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

DispatchManager::DispatchManager(const Config& cfg, const MapManager& map, const TrafficManager& traffic)
    : cfg_(cfg), map_(map), traffic_(traffic) {}

std::string DispatchManager::pickRobot(const std::vector<std::string>& candidates, const std::string& target,
                                       bool keep_entrance) const {
  std::string best;
  double best_cost = 1e9;
  for (const std::string& id : candidates) {
    double c = traffic_.pathCost(id, target);  // 직선 거리가 아니라 실제로 달릴 경로 길이
    if (c < 0) continue;
    if (keep_entrance && traffic_.isStationary(id) && isEntranceDock(traffic_.currentNode(id)))
      c += cfg_.entrance_keep_penalty_m;
    if (c < best_cost) {
      best_cost = c;
      best = id;
    }
  }
  return best;
}

bool DispatchManager::isEntranceDock(const std::string& node) const { return contains(cfg_.entrance_docks, node); }

bool DispatchManager::isPickupDock(const std::string& node) const { return contains(cfg_.pickup_docks, node); }

bool DispatchManager::dockBlocked(const std::string& robot_id) const {
  if (!traffic_.isStationary(robot_id)) return false;
  auto it = cfg_.dock_via.find(traffic_.currentNode(robot_id));
  if (it == cfg_.dock_via.end()) return false;
  return !traffic_.isDockFree(it->second, robot_id);  // 앞칸에 누가 서 있거나 가는 중
}

bool DispatchManager::dockAvailable(const std::string& dock, const std::string& robot_id) const {
  if (!traffic_.isDockFree(dock, robot_id)) return false;
  // 뒤칸: 앞칸이 비어 있어야 지나갈 수 있다
  auto via = cfg_.dock_via.find(dock);
  if (via != cfg_.dock_via.end() && !traffic_.isDockFree(via->second, robot_id)) return false;
  // 앞칸: 뒤칸으로 가는 로봇이 있으면 그 로봇이 지나갈 때까지 비워 둔다
  for (const auto& pair : cfg_.dock_via) {
    if (pair.second != dock) continue;
    for (const std::string& other : cfg_.robots) {
      if (other != robot_id && traffic_.goalOf(other) == pair.first) return false;
    }
  }
  return traffic_.pathCost(robot_id, dock) >= 0;
}

std::string DispatchManager::nearestDock(const std::string& robot_id, const std::vector<std::string>& docks) const {
  std::string best;
  double best_cost = 1e9;
  for (const std::string& dock : docks) {
    if (!dockAvailable(dock, robot_id)) continue;
    double c = traffic_.pathCost(robot_id, dock);
    if (c < best_cost) {
      best_cost = c;
      best = dock;
    }
  }
  return best;
}

std::string DispatchManager::pickDock(const std::string& robot_id, bool entrance_first) const {
  // 입구: 뒤칸부터 채운다 (앞칸을 비워 두면 다음 로봇이 들어올 수 있고, 안내는 앞칸 로봇부터 나간다)
  auto pickEntrance = [&]() -> std::string {
    for (auto it = cfg_.entrance_docks.rbegin(); it != cfg_.entrance_docks.rend(); ++it) {
      if (dockAvailable(*it, robot_id)) return *it;
    }
    return "";
  };
  std::vector<std::string> others;
  for (const std::string& dock : map_.nodesOfKind("DOCK")) {
    if (!isEntranceDock(dock) && !isPickupDock(dock)) others.push_back(dock);
  }

  std::string d;
  if (entrance_first && !(d = pickEntrance()).empty()) return d;
  if (!(d = nearestDock(robot_id, cfg_.pickup_docks)).empty()) return d;
  if (!(d = nearestDock(robot_id, others)).empty()) return d;
  if (!entrance_first) d = pickEntrance();
  return d;
}

}  // namespace pinky
