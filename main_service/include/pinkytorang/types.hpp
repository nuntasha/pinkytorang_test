// 공통 타입: 모듈끼리 주고받는 값은 전부 여기에 정의한다
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pinky {

// ---------------------------------------------------------------- 작업

enum class TaskKind { SERVE, GUIDE };  // 서빙 / 손님 안내

enum class Stage {
  COOKING,                 // 서빙만: 조리 중 (이때만 취소 가능)
  WAITING, ASSIGNED,       // 로봇(안내는 테이블도) 기다림 / 배정됨
  TO_PICKUP, AT_PICKUP,    // 서빙만
  TO_TABLE, AT_TABLE,
  TO_DOCK, DONE,
  FAILED, CANCELED
};

// 확인 버튼: LOADED(주방 적재) / RECEIVED(손님 수령) / SEATED(손님 착석)
enum class Confirm { NONE, LOADED, RECEIVED, SEATED };

// 테이블 상태: 빈자리 / 안내 중(예약) / 사용 중
enum class TableState { FREE, RESERVED, OCCUPIED };

// ---------------------------------------------------------------- 로봇 (fleet_msgs v7 숫자값 그대로. 메시지는 v5부터 같음)

enum class RobotRunState : uint8_t { IDLE = 0, MOVING = 1, PAUSED = 2, BLOCKED = 3, ERROR = 4 };
enum class ControlCmd : uint8_t { PAUSE = 0, RESUME = 1, CANCEL = 2 };

// RobotEvent.type 값
const uint8_t EVENT_NODE_REACHED = 0;
const uint8_t EVENT_CMD_DONE = 1;
const uint8_t EVENT_CMD_FAILED = 2;
const uint8_t EVENT_BLOCKED = 4;

// ---------------------------------------------------------------- 서버 -> 로봇 명령 (ROS 메시지와 필드 1:1)

struct WaypointCmd {
  std::string node_id;
  double x = 0.0;
  double y = 0.0;
  double yaw = 0.0;  // 로봇 앞이 보는 방향 (후진 중이면 진행 방향 + 180도)
};

// Move.drive (fleet_msgs v5~): Move 하나에 담긴 waypoints 전체의 주행 방향
enum class Drive : uint8_t { FORWARD = 0, REVERSE = 1 };

struct MoveCmd {
  uint32_t cmd_id = 0;
  bool append = false;  // false = REPLACE, true = APPEND
  Drive drive = Drive::FORWARD;
  std::vector<WaypointCmd> waypoints;
};

struct DisplayCmd {
  std::string text;
  uint16_t countdown_sec = 0;
};

// ---------------------------------------------------------------- 메뉴 · 주문

struct MenuItem {
  int id = 0;
  std::string name;
  int price = 0;
  std::string desc;
};

struct OrderItem {
  int menu_id = 0;
  std::string name;
  int qty = 0;
};

// ---------------------------------------------------------------- UI로 나가는 값 (Client Link가 JSON으로 바꾼다)

struct TaskView {
  int task_id = 0;
  TaskKind kind = TaskKind::SERVE;
  std::string table;
  int order_id = -1;      // -1 = 없음 (안내 작업)
  Stage stage = Stage::WAITING;
  std::string robot_id;   // "" = 미배정
  std::vector<OrderItem> items;
  std::string created_at;
};

struct RobotView {
  std::string id;
  double x = 0.0;
  double y = 0.0;
  double yaw = 0.0;
  uint8_t state = 0;      // RobotRunState 값
  std::string last_node;
  double battery = 0.0;
  int task_id = -1;       // -1 = 없음
  bool online = false;
  bool charging = false;  // 배터리 부족으로 충전 중 (배정 안 함)
};

struct TableView {
  std::string id;
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;  // 배치도 그림용 (m, 중심 좌표)
  TableState state = TableState::FREE;
  int task_id = -1;       // 이 테이블로 안내 중인 작업 (-1 = 없음)
};

struct LockView {
  std::string node;
  std::string robot_id;
};

struct SnapshotView {
  std::vector<RobotView> robots;
  std::vector<TaskView> tasks;
  std::vector<LockView> locks;
  std::vector<TableView> tables;
};

struct ConfirmView {
  int task_id = 0;
  Confirm confirm = Confirm::NONE;
  int countdown_sec = 0;
  std::string table;
  std::string robot_id;
};

struct HistoryRow {
  std::string time;     // ISO 8601 UTC (예: 2026-10-23T01:02:03.456Z)
  std::string type;     // ORDER / TASK / ALERT
  std::string message;
};

struct MapNodeView {
  std::string id;
  std::string type;       // PASS / STOP
  std::string stop_kind;  // "" / QUEUE / PICKUP / SERVE / DOCK
  std::string table;
  double x = 0.0;
  double y = 0.0;
};

struct MapEdgeView {
  std::string from;
  std::string to;
};

struct MapView {
  double board_w = 0.0;
  double board_h = 0.0;
  std::vector<MapNodeView> nodes;
  std::vector<MapEdgeView> edges;
};

// HTTP 요청 처리 결과 (Task Manager -> Client Link)
struct ApiResult {
  int status = 200;       // 200 / 400 / 404 / 409
  std::string error;      // status != 200 일 때 {"error": ...}
  int task_id = -1;
  int order_id = -1;
  std::string table;      // 안내: 손님이 고른 테이블
  std::string stage;      // 안내 상태 조회용 (Stage 이름)
};

// ---------------------------------------------------------------- 문자열 변환

std::string toString(TaskKind v);
std::string toString(Stage v);
std::string toString(Confirm v);
std::string toString(ControlCmd v);
std::string toString(TableState v);
std::string robotStateName(uint8_t state);

// 성공하면 true, out에 결과
bool parseConfirm(const std::string& s, Confirm& out);
bool parseControlCmd(const std::string& s, ControlCmd& out);

bool isEnded(Stage s);  // DONE / FAILED / CANCELED

std::string nowIsoUtc();   // 2026-10-23T01:02:03.456Z
std::string nowLocalHms(); // 10:02:03 (로그용)

}  // namespace pinky
