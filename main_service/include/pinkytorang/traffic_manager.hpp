// Traffic Manager: 경로 + 노드 lock (fleet_msgs v4 README 4장)
//
// 로봇마다 경로를 세 덩어리로 들고 있다
//   at        : 마지막으로 도착한 노드 (lock 보유)
//   committed : lock 잡고 로봇에 보낸, 아직 도착 못 한 노드
//   plan      : 아직 lock 안 잡은 남은 경로
//
// 규칙
//   1. Move는 항상 lock 노드에서 끝난다. 코너(lock 없음)는 다음 lock 노드와 묶는다
//   2. 노드 N의 lock은 로봇이 N 다음 노드에 도착하면 푼다
//   3. 양방향 구간: 구간을 따라 지나갈 노드 + 빠져나가는 끝 노드(또는 도킹)까지 한 번에. 하나라도 못 잡으면 안 잡음
//   4. no_wait 노드: 그 다음 lock 노드까지 함께 잡을 수 있을 때만
//   5. 포켓(테이블 · 도킹)에서 나올 때 첫 Waypoint는 reverse = true
//   6. 로봇이 명령 수행 중이면 APPEND(같은 cmd_id), 끝났으면 REPLACE(새 cmd_id)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "pinkytorang/config.hpp"
#include "pinkytorang/map_manager.hpp"
#include "pinkytorang/ports.hpp"

namespace pinky {

struct Arrival {
  std::string robot_id;
  std::string node;
};

class TrafficManager {
 public:
  TrafficManager(const Config& cfg, const MapManager& map);

  void setFleetOut(IFleetOut* out) { fleet_ = out; }
  void addRobot(const std::string& id);

  // ---- 위치
  bool localize(const std::string& id, const std::string& last_node, double x, double y, std::string& reason);
  bool isLocalized(const std::string& id) const;
  std::string currentNode(const std::string& id) const;   // 마지막 도착 노드
  std::string lastSentNode(const std::string& id) const;  // 보낸 경로의 끝 (다음 계획의 출발점)

  // ---- 목적지
  bool setGoal(const std::string& id, const std::string& node);  // 경로가 없으면 false
  void clearGoal(const std::string& id);
  bool isStationary(const std::string& id) const;  // 보낸 노드도 없고 목적지도 없음

  // ---- 로봇 이벤트
  void onNodeReached(const std::string& id, const std::string& node);
  void onCmdDone(const std::string& id, uint32_t cmd_id, const std::string& node);
  void onCmdFailed(const std::string& id);

  // ---- 멈춤: true면 경로를 늘리지 않는다 (일시정지 · 고장 · 통신 끊김)
  void setHold(const std::string& id, bool hold);
  // 보냈지만 아직 도착 못 한 노드를 새 cmd_id로 다시 보낸다
  void resend(const std::string& id);

  void tick();                        // 모든 로봇 경로 늘리기 시도
  std::vector<Arrival> takeArrivals(); // 목적지 도착 목록 (꺼내면 비워짐)

  // ---- 조회
  std::vector<LockView> locks() const;
  bool isDockFree(const std::string& dock, const std::string& for_robot) const;
  double pathCost(const std::string& id, const std::string& target) const;  // 경로 없으면 -1

 private:
  struct Robot {
    std::string id;
    bool localized = false;
    std::string at;
    std::vector<std::string> committed;
    std::vector<std::string> plan;
    std::string goal;          // "" = 목적지 없음
    uint32_t cmd_id = 0;
    bool cmd_active = false;   // 로봇이 지금 명령을 수행 중인가
    bool hold = false;
    std::string last_wait_log;
  };

  std::string lastSent(const Robot& r) const;
  bool tryExtend(Robot& r);
  std::vector<std::string> buildSegment(const Robot& r) const;
  bool canLock(const std::vector<std::string>& seg, const std::string& id, std::string& blocker) const;
  int committedLockCount(const Robot& r) const;
  void checkArrival(Robot& r);
  void publish(Robot& r, const std::vector<std::string>& nodes, bool append, bool reverse_first);
  double yawFor(const std::vector<std::string>& seq, size_t i, const std::string& before) const;
  void releaseLock(const std::string& node, const std::string& id);

  const Config& cfg_;
  const MapManager& map_;
  IFleetOut* fleet_ = nullptr;
  std::map<std::string, Robot> robots_;
  std::map<std::string, std::string> lock_;  // 노드 -> 로봇
  std::vector<Arrival> arrivals_;
  uint32_t next_cmd_id_ = 1;
};

}  // namespace pinky
