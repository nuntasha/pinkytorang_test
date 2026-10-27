// Task Manager: 판단의 중심
// - UI 요청(Client Link)과 로봇 소식(Fleet Link)을 받아 작업 단계를 진행한다
// - 배정 · 테이블 고르기 · 도킹 고르기는 Dispatch Manager, 경로 · lock은 Traffic Manager에 맡긴다
// - 여러 스레드가 동시에 부르므로, public 함수는 전부 mutex_를 잠그고 시작한다
//   (private 함수는 이미 잠긴 상태에서만 불린다)
//
// 서빙: COOKING [조리 완료] → WAITING → ASSIGNED → TO_PICKUP → AT_PICKUP [LOADED]
//       → TO_TABLE → AT_TABLE [RECEIVED/시간초과] → TO_DOCK → DONE
//       취소는 COOKING 에서만
// 안내: 키오스크에서 손님이 빈 테이블을 고름 → 그 테이블 RESERVED
//       WAITING (입구 로봇 기다림) → ASSIGNED → TO_TABLE → AT_TABLE [SEATED/시간초과]
//       → TO_DOCK → DONE.  취소는 착석 전(WAITING · ASSIGNED · TO_TABLE)까지
// 테이블: FREE → (안내 배정) RESERVED → (착석) OCCUPIED → (직원 정리 완료) FREE
//
// 스케줄링 (afterChange): 안내 최대 max_active_guide · 서빙 최대 max_active_serve (만석이면 로봇 전부)
//   대기 배치: 찬 테이블 수에 따라 입구 2 / 1 / 0대, 나머지는 픽업 쪽 도킹
//   같은 테이블 대기 주문은 한 로봇이 묶어서, 도킹 복귀 중인 로봇도 이어받기
//   고장 · 통신 끊김이 reassign_after_sec 넘으면 적재 전 서빙은 다시 배정
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "pinkytorang/config.hpp"
#include "pinkytorang/db_manager.hpp"
#include "pinkytorang/dispatch_manager.hpp"
#include "pinkytorang/map_manager.hpp"
#include "pinkytorang/ports.hpp"
#include "pinkytorang/traffic_manager.hpp"

namespace pinky {

class TaskManager {
 public:
  TaskManager(const Config& cfg, const MapManager& map, TrafficManager& traffic, DispatchManager& dispatch,
              DBManager& db);

  void setFleetOut(IFleetOut* out) { fleet_ = out; }
  void setClientOut(IClientOut* out) { client_ = out; }
  void start();  // 지난번에 끝나지 않은 작업 정리 + 테이블 상태 읽기 + 번호 시작값

  // ---- UI 요청 (Client Link가 부름)
  ApiResult createOrder(const std::string& table, const std::vector<OrderItem>& items);  // -> COOKING
  ApiResult markCooked(int task_id);   // 주방: 조리 완료 (COOKING -> WAITING)
  ApiResult cancelTask(int task_id);   // 서빙은 COOKING, 안내는 착석 전까지만
  ApiResult requestGuide(const std::string& table);  // 입구 키오스크: 손님이 고른 빈 테이블 -> 안내 작업
  ApiResult guideStatus(int task_id);                 // 키오스크: 테이블 · 단계
  ApiResult clearTable(const std::string& table);   // 직원: 정리 완료 (OCCUPIED -> FREE)
  ApiResult occupyTable(const std::string& table);  // 직원: 안내 없이 앉은 손님 (FREE -> OCCUPIED)
  ApiResult confirm(int task_id, Confirm c);
  ApiResult control(const std::string& robot_id, ControlCmd cmd);  // robot_id = "all" 가능
  std::vector<TaskView> ordersOfTable(const std::string& table);
  std::vector<TableView> tables();
  std::vector<HistoryRow> history(const std::string& from, const std::string& to, const std::string& type);
  MapView mapView();

  // ---- 로봇 소식 (Fleet Link / SimFleet이 부름)
  void onRobotState(const std::string& id, double x, double y, double yaw, uint8_t state,
                    const std::string& last_node, float battery);
  void onRobotEvent(const std::string& id, uint8_t type, uint32_t cmd_id, const std::string& node,
                    const std::string& reason);
  void onCommLost(const std::string& id);
  void onCommRestored(const std::string& id);

  // ---- 주기 처리 (main에서 0.1초마다): 확인 시간 초과, snapshot 보내기
  void tick();

 private:
  using Clock = std::chrono::steady_clock;

  struct Task {
    int id = 0;
    TaskKind kind = TaskKind::SERVE;
    std::string table;
    std::string table_node;
    int order_id = -1;
    std::vector<OrderItem> items;
    Stage stage = Stage::WAITING;
    std::string robot;
    std::string created_at;
    std::string goal_node;             // 이동 단계의 목적지
    Confirm pending = Confirm::NONE;   // 기다리는 확인 버튼
    int countdown = 0;
    bool has_timeout = false;
    Clock::time_point timeout_at;
    bool need_dock = false;            // 빈 도킹이 없어 기다리는 중
    Clock::time_point ended_at;
    // 같은 테이블 묶음 배달: 앞장 작업(lead)이 로봇 · 이동 · 확인 버튼을 맡고, 나머지(riders)는 단계만 따라간다
    int lead = -1;                     // 따라가는 작업이면 앞장 작업 번호
    std::vector<int> riders;           // 앞장 작업이면 같이 싣는 작업들
  };

  struct Robot {
    std::string id;
    bool seen = false;
    bool online = false;
    double x = 0.0, y = 0.0, yaw = 0.0;
    uint8_t state = 0;
    std::string last_node;
    float battery = 0.0f;
    int task_id = -1;
    bool paused = false;
    bool fault = false;     // CMD_FAILED 뒤 멈춤
    bool homing = false;    // 작업 없이 도킹으로 가는 중
    bool charging = false;  // 배터리 부족: 배정 안 함
    bool error_alerted = false;
    std::string loc_problem;
    bool bad = false;                // 고장 또는 통신 끊김
    Clock::time_point bad_since;
    bool stuck_alerted = false;      // 실은 채 멈춤 알림 보냄
  };

  struct Table {
    std::string id;
    std::string node;
    TableState state = TableState::FREE;
    int task_id = -1;       // RESERVED 일 때 안내 작업
  };

  // 작업 흐름
  void afterChange();
  void processArrivals();
  void dispatchGuide();      // 테이블 정해진 안내 -> 입구 로봇
  void dispatchServe();      // 조리 완료된 서빙 -> 가까운 로봇
  void sendIdleRobotsHome();
  void rebalanceStandby();   // 찬 테이블 수에 맞게 쉬는 로봇을 입구 / 픽업 쪽으로 옮김
  void fixDockConflicts();   // 뒤칸(D1)으로 가는데 앞칸(D2)에 로봇이 서 있으면 자리 바꿈 / 다른 도킹으로
  bool redirectDock(Robot& r, const std::string& dock);  // 도킹으로 가는 로봇의 목적지 바꾸기
  void retryDocks();
  void recoverStuckTasks(Clock::time_point now);  // 고장 · 통신 끊김 로봇의 작업 처리
  void unassign(Task& t, const std::string& why); // 적재 전 작업을 로봇에서 떼어 WAITING으로
  void mirrorStage(Task& o, Stage s, const Task& lead);  // 묶음 작업 단계 따라가기
  Task* joinableTrip(const Task& t);              // 같은 테이블로 아직 적재 전인 앞장 작업
  void enterStage(Task& t, Stage s);
  void goTo(Task& t, const std::string& node);
  void openConfirm(Task& t, Confirm c, int countdown);
  void closeConfirm(Task& t, const std::string& by);
  ApiResult handleConfirm(Task& t, Confirm c, const std::string& by);
  bool canCancel(const Task& t) const;
  void doCancel(Task& t, const std::string& by);
  bool controlOne(Robot& r, ControlCmd cmd);  // CANCEL이 규칙에 막히면 false
  void updateHold(Robot& r);
  void setTable(Table& tb, TableState s, int task_id);

  // 도우미
  bool robotUsable(const Robot& r);
  bool robotFree(const Robot& r);      // 배정 가능: 쓸 수 있고 · 충전 아님 · 작업 없음 · 귀가 중 아님
  bool hasWaitingGuide() const;
  bool tableBusy(const std::string& table) const;  // 그 테이블로 가는 중이거나 서 있는 로봇이 있나 (종류 상관없이)
  bool entranceOk(const std::string& robot_id);  // 배터리가 충분해서 입구 도킹에 세워도 되나 / 이어받기 가능
  int freeTableCount() const;
  int entranceStandbyWanted() const;   // 입구에 세워 둘 로봇 수 (찬 테이블 수 기준)
  int entranceStandbyCount() const;    // 입구 도킹에 있거나 가는 중인 (충전 아닌) 로봇 수
  bool wantEntrance(const std::string& robot_id) const;  // 이 로봇을 입구 도킹으로 보낼까
  std::string pickDockFor(const std::string& robot_id) const;
  Robot* findRobot(const std::string& id);
  Task* findTask(int id);
  Table* findTable(const std::string& id);
  TaskView view(const Task& t) const;
  SnapshotView buildSnapshot();
  std::string stageText(const Task& t, Stage s) const;
  void display(const std::string& robot_id, const std::string& text, int countdown);
  void alert(const std::string& level, const std::string& robot_id, const std::string& msg);
  void record(const std::string& type, const std::string& msg);

  std::mutex mutex_;
  const Config& cfg_;
  const MapManager& map_;
  TrafficManager& traffic_;
  DispatchManager& dispatch_;
  DBManager& db_;
  IFleetOut* fleet_ = nullptr;
  IClientOut* client_ = nullptr;

  std::map<int, Task> tasks_;
  std::map<std::string, Robot> robots_;
  std::map<std::string, Table> tables_;
  int next_task_id_ = 1;
  int next_order_id_ = 1;
  std::string pickup_node_;
  bool dirty_ = true;
  Clock::time_point last_snapshot_;
};

}  // namespace pinky
