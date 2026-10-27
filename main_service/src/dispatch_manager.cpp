#include "pinkytorang/dispatch_manager.hpp"

namespace pinky {

DispatchManager::DispatchManager(const Config& cfg, const MapManager& map, const TrafficManager& traffic)
    : cfg_(cfg), map_(map), traffic_(traffic) {}

std::string DispatchManager::pickRobot(const std::vector<std::string>& free_robots, const std::string& target) const {
  std::string best;
  double best_cost = 1e9;
  for (const std::string& id : free_robots) {
    double c = traffic_.pathCost(id, target);  // 직선 거리가 아니라 실제로 달릴 경로 길이
    if (c < 0) continue;
    if (traffic_.currentNode(id) == cfg_.entrance_dock) c += cfg_.entrance_keep_penalty_m;
    if (c < best_cost) {
      best_cost = c;
      best = id;
    }
  }
  return best;
}

std::string DispatchManager::pickDock(const std::string& robot_id, bool allow_entrance) const {
  // 입구 도킹이 비어 있으면 먼저 채운다 (안내할 로봇이 늘 입구에 있게)
  if (allow_entrance && traffic_.isDockFree(cfg_.entrance_dock, robot_id) &&
      traffic_.pathCost(robot_id, cfg_.entrance_dock) >= 0)
    return cfg_.entrance_dock;

  std::string best;
  double best_cost = 1e9;
  for (const std::string& dock : map_.nodesOfKind("DOCK")) {
    if (dock == cfg_.entrance_dock) continue;
    if (!traffic_.isDockFree(dock, robot_id)) continue;
    double c = traffic_.pathCost(robot_id, dock);
    if (c >= 0 && c < best_cost) {
      best_cost = c;
      best = dock;
    }
  }
  return best;
}

}  // namespace pinky
