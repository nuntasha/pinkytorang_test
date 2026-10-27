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

  // 서빙: free_robots 중에서 target까지 가장 가까운 로봇. 없으면 ""
  //       입구 도킹에 서 있는 로봇은 entrance_keep_penalty_m 만큼 멀게 친다 (안내용으로 아껴 둠)
  std::string pickRobot(const std::vector<std::string>& free_robots, const std::string& target) const;

  // 서빙을 하나 더 시작해도 되나 (서빙이 로봇을 다 가져가지 않게)
  bool underServeLimit(int active_serve) const { return active_serve < cfg_.max_active_serve; }

  // 안내할 테이블: 인원 이상 좌석인 빈 테이블 중 좌석이 가장 적은 곳. free_tables = {id, seats}. 없으면 ""
  std::string pickTable(const std::vector<TableConfig>& free_tables, int party) const;

  // 비어 있는 도킹: 입구 도킹이 비어 있으면 입구, 아니면 가장 가까운 곳. 없으면 ""
  // allow_entrance = false 면 입구 도킹은 고르지 않는다
  std::string pickDock(const std::string& robot_id, bool allow_entrance = true) const;

 private:
  const Config& cfg_;
  const MapManager& map_;
  const TrafficManager& traffic_;
};

}  // namespace pinky
