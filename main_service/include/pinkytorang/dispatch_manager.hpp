// Dispatch Manager: 어떤 로봇에 일을 줄지, 어느 도킹으로 보낼지 (판단만, 상태 변경은 Task Manager)
#pragma once

#include <string>
#include <vector>

#include "pinkytorang/config.hpp"
#include "pinkytorang/map_manager.hpp"
#include "pinkytorang/traffic_manager.hpp"

namespace pinky {

class DispatchManager {
 public:
  DispatchManager(const Config& cfg, const MapManager& map, const TrafficManager& traffic);

  // 서빙: candidates 중에서 target까지 가장 가까운 로봇. 없으면 ""
  //       keep_entrance면 입구 도킹에 서 있는 로봇은 entrance_keep_penalty_m 만큼 멀게 친다 (안내용으로 아껴 둠)
  std::string pickRobot(const std::vector<std::string>& candidates, const std::string& target,
                        bool keep_entrance) const;

  // 빈 도킹 고르기
  //   entrance_first = true : 입구 도킹(뒤칸부터) → 픽업 도킹(가까운 곳) → 나머지
  //   entrance_first = false: 픽업 도킹(가까운 곳) → 나머지 → 입구 도킹 (입구밖에 없으면 입구라도)
  // 없으면 ""
  std::string pickDock(const std::string& robot_id, bool entrance_first) const;

  bool isEntranceDock(const std::string& node) const;
  bool isPickupDock(const std::string& node) const;

  // 뒤칸 도킹(dock_via)에 서 있는데 앞칸에 다른 로봇이 있어 못 나오는가
  bool dockBlocked(const std::string& robot_id) const;

 private:
  // 그 도킹에 지금 세울 수 있나: 비어 있고, 앞칸을 지나야 하면 앞칸도 비어 있고,
  // 뒤칸으로 들어가는 로봇이 이 칸을 지나는 중이 아니어야 한다
  bool dockAvailable(const std::string& dock, const std::string& robot_id) const;
  std::string nearestDock(const std::string& robot_id, const std::vector<std::string>& docks) const;

  const Config& cfg_;
  const MapManager& map_;
  const TrafficManager& traffic_;
};

}  // namespace pinky
