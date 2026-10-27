// Traffic Manager: 경로 + 노드 lock (fleet_msgs v7 README 4장)
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
//   5. Move 하나에는 방향(drive) 하나. 후진 구간(yaml reverse)과 포켓 드나듦(목적지 pocket은 entry 방향,
//      출발 pocket은 그 반대)으로 정하고, 방향이 바뀌는 곳 · 포켓 드나드는 곳에서 Move를 나눈다 (fleet_msgs README 4.5 · 4.9)
//   6. 로봇이 명령 수행 중이면 APPEND(같은 cmd_id), 끝났으면 REPLACE(새 cmd_id)
//   7. 다음 칸(노드)을 잡을 때 그 칸으로 가는 길(엣지)도 같이 잡는다. 양방향 길에서 마주 오는 로봇이 같은 길에 못 들어온다
//   8. 앞이 막히면 우회 경로를 찾는다
//      - 앞 로봇이 서 있으면(목적지 도착 · 멈춤) 길이 상관없이 우회
//      - 움직이는 중이면 우회가 detour_max_m 이하로 길어질 때만 우회, 아니면 기다림 (detour_wait_sec 지나면 우회)
//      - 서로 기다리는 중(교착)이면 우회, 우회 길도 없으면 한 대가 옆 칸으로 비켜 준다
#pragma once

#include <chrono>
#include <map>
#include <set>
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
  std::string goalOf(const std::string& id) const;  // 목적지 ("" = 없음)

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
  using Clock = std::chrono::steady_clock;

  struct Robot {
    std::string id;
    bool localized = false;
    std::string at;
    std::vector<std::string> committed;
    std::vector<std::string> plan;
    std::string goal;          // "" = 목적지 없음
    std::string route_start;   // 이 목적지로 출발한 노드 (포켓 출차 방향 판단)
    uint32_t cmd_id = 0;
    bool cmd_active = false;   // 로봇이 지금 명령을 수행 중인가
    bool hold = false;
    std::string last_wait_log;
    // 막혔을 때
    bool waiting = false;          // 다음 칸을 못 잡고 기다리는 중
    std::string waiting_for;       // 막고 있는 로봇
    Clock::time_point wait_since;
    std::set<std::string> avoid;   // 우회 중: 경로 계산에서 뺄 노드
    Clock::time_point avoid_until;
    std::string via;               // 비켜 주기: 먼저 들를 옆 칸 ("" = 없음)
  };

  std::string lastSent(const Robot& r) const;
  bool tryExtend(Robot& r);
  std::vector<std::string> buildSegment(const Robot& r) const;
  std::vector<std::string> planPath(const Robot& r) const;
  bool canLock(const Robot& r, const std::vector<std::string>& seg, std::string& blocker, std::string& blocker_robot,
               std::string& blocked_node) const;
  bool resolveBlock(Robot& r, const std::string& blocker_robot, const std::string& blocked_node,
                    const std::vector<std::string>& path);
  bool inWaitCycle(const std::string& start) const;
  bool sideStep(Robot& r, const Robot& other);
  static std::string edgeKey(const std::string& a, const std::string& b);
  int committedLockCount(const Robot& r) const;
  void checkArrival(Robot& r);
  void publish(Robot& r, const std::vector<std::string>& nodes, bool append);
  void releaseLock(const std::string& node, const std::string& id);

  const Config& cfg_;
  const MapManager& map_;
  IFleetOut* fleet_ = nullptr;
  std::map<std::string, Robot> robots_;
  std::map<std::string, std::string> lock_;       // 노드 -> 로봇
  std::map<std::string, std::string> edge_lock_;  // "A|B" (이름 순) -> 로봇
  std::vector<Arrival> arrivals_;
  uint32_t next_cmd_id_ = 1;
};

}  // namespace pinky
