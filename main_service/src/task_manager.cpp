#include "pinkytorang/task_manager.hpp"

#include "pinkytorang/log.hpp"

namespace pinky {

static ApiResult errorResult(int status, const std::string& error) {
  ApiResult r;
  r.status = status;
  r.error = error;
  return r;
}

TaskManager::TaskManager(const Config& cfg, const MapManager& map, TrafficManager& traffic,
                         DispatchManager& dispatch, DBManager& db)
    : cfg_(cfg), map_(map), traffic_(traffic), dispatch_(dispatch), db_(db) {
  for (const std::string& id : cfg.robots) {
    Robot r;
    r.id = id;
    robots_[id] = r;
  }
  pickup_node_ = map_.nodesOfKind("PICKUP").front();

  // 설정의 테이블이 맵에 있는지 확인
  for (const TableConfig& tc : cfg.tables) {
    std::string node = map_.tableNode(tc.id);
    if (node.empty()) {
      logLine("TASK", "설정 오류: 테이블 " + tc.id + " 가 node_graph에 없음 (무시)");
      continue;
    }
    Table tb;
    tb.id = tc.id;
    tb.node = node;
    tb.seats = tc.seats;
    tables_[tc.id] = tb;
  }
  const MapNode* e = map_.node(cfg.entrance_dock);
  if (!e || e->stop_kind != "DOCK") logLine("TASK", "설정 오류: entrance_dock " + cfg.entrance_dock + " 가 도킹 노드가 아님");
}

void TaskManager::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  int failed = db_.failUnfinishedTasks();
  next_task_id_ = db_.maxTaskId() + 1;
  next_order_id_ = db_.maxOrderId() + 1;
  if (failed > 0) record("TASK", "지난 실행에서 끝나지 않은 작업 " + std::to_string(failed) + "건 -> FAILED");

  // 테이블 상태: 사용 중은 그대로, 안내 중(RESERVED)은 안내가 끊겼으니 빈자리로
  std::map<std::string, std::string> saved = db_.loadTableStates();
  for (auto& pair : tables_) {
    Table& tb = pair.second;
    tb.state = (saved[tb.id] == "OCCUPIED") ? TableState::OCCUPIED : TableState::FREE;
    db_.saveTableState(tb.id, tb.state);
  }
  logLine("TASK", "시작. 다음 task 번호 " + std::to_string(next_task_id_));
}

// ================================================================ UI 요청

ApiResult TaskManager::createOrder(const std::string& table, const std::vector<OrderItem>& items) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string node = map_.tableNode(table);
  if (node.empty()) return errorResult(404, "unknown_table");
  if (items.empty()) return errorResult(400, "empty_order");

  Task t;
  t.kind = TaskKind::SERVE;
  t.stage = Stage::COOKING;  // 주방에서 조리 완료를 누르면 WAITING
  t.table = table;
  t.table_node = node;
  std::string summary;
  for (const OrderItem& in : items) {
    if (in.qty < 1 || in.qty > 99) return errorResult(400, "bad_qty");
    std::string name;
    for (const MenuItem& m : cfg_.menu) {
      if (m.id == in.menu_id) name = m.name;
    }
    if (name.empty()) return errorResult(400, "unknown_menu");
    OrderItem item;
    item.menu_id = in.menu_id;
    item.name = name;
    item.qty = in.qty;
    t.items.push_back(item);
    if (!summary.empty()) summary += ", ";
    summary += name + " x" + std::to_string(in.qty);
  }
  t.id = next_task_id_++;
  t.order_id = next_order_id_++;
  t.created_at = nowIsoUtc();
  tasks_[t.id] = t;

  db_.saveTask(view(t));
  record("ORDER", "order " + std::to_string(t.order_id) + " " + table + " " + summary + " -> 조리 중");
  if (client_) client_->pushTaskUpdate(view(t));
  dirty_ = true;

  ApiResult ok;
  ok.task_id = t.id;
  ok.order_id = t.order_id;
  return ok;
}

ApiResult TaskManager::markCooked(int task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Task* t = findTask(task_id);
  if (!t || t->kind != TaskKind::SERVE) return errorResult(404, "unknown_task");
  if (t->stage != Stage::COOKING) return errorResult(409, "not_cooking");
  record("TASK", "task " + std::to_string(t->id) + " 조리 완료");
  enterStage(*t, Stage::WAITING);
  afterChange();
  return ApiResult();
}

ApiResult TaskManager::cancelTask(int task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Task* t = findTask(task_id);
  if (!t) return errorResult(404, "unknown_task");
  if (!canCancel(*t)) return errorResult(409, "cannot_cancel");
  doCancel(*t, "BUTTON");
  afterChange();
  return ApiResult();
}

ApiResult TaskManager::requestGuide(int party) {
  std::lock_guard<std::mutex> lock(mutex_);
  int max_seats = 0;
  for (auto& pair : tables_) {
    if (pair.second.seats > max_seats) max_seats = pair.second.seats;
  }
  if (party < 1) return errorResult(400, "bad_party");
  if (party > max_seats) return errorResult(400, "party_too_large");

  Task t;
  t.id = next_task_id_++;
  t.kind = TaskKind::GUIDE;
  t.party = party;
  t.created_at = nowIsoUtc();
  tasks_[t.id] = t;
  db_.saveTask(view(t));
  record("ORDER", "guide " + std::to_string(party) + "명 -> task " + std::to_string(t.id));
  dirty_ = true;
  afterChange();  // 여기서 빈 테이블이 있으면 바로 정해진다

  Task& now = tasks_[t.id];
  ApiResult ok;
  ok.task_id = now.id;
  ok.table = now.table;
  ok.queue_pos = queuePos(now);
  ok.stage = toString(now.stage);
  return ok;
}

ApiResult TaskManager::guideStatus(int task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Task* t = findTask(task_id);
  if (!t || t->kind != TaskKind::GUIDE) return errorResult(404, "unknown_task");
  ApiResult r;
  r.task_id = t->id;
  r.table = t->table;
  r.queue_pos = queuePos(*t);
  r.stage = toString(t->stage);
  return r;
}

ApiResult TaskManager::clearTable(const std::string& table) {
  std::lock_guard<std::mutex> lock(mutex_);
  Table* tb = findTable(table);
  if (!tb) return errorResult(404, "unknown_table");
  if (tb->state == TableState::RESERVED) return errorResult(409, "guiding");  // 손님이 오는 중
  setTable(*tb, TableState::FREE, -1);
  record("TASK", table + " 정리 완료 -> 빈자리");
  afterChange();  // 기다리는 손님이 있으면 바로 안내
  return ApiResult();
}

ApiResult TaskManager::occupyTable(const std::string& table) {
  std::lock_guard<std::mutex> lock(mutex_);
  Table* tb = findTable(table);
  if (!tb) return errorResult(404, "unknown_table");
  if (tb->state != TableState::FREE) return errorResult(409, "not_free");
  setTable(*tb, TableState::OCCUPIED, -1);
  record("TASK", table + " 직원이 사용 중으로 바꿈");
  return ApiResult();
}

ApiResult TaskManager::confirm(int task_id, Confirm c) {
  std::lock_guard<std::mutex> lock(mutex_);
  Task* t = findTask(task_id);
  if (!t) return errorResult(409, "not_waiting");
  ApiResult r = handleConfirm(*t, c, "BUTTON");
  afterChange();
  return r;
}

ApiResult TaskManager::control(const std::string& robot_id, ControlCmd cmd) {
  std::lock_guard<std::mutex> lock(mutex_);
  ApiResult result;
  if (robot_id == "all") {
    for (auto& pair : robots_) controlOne(pair.second, cmd);  // 취소는 되는 로봇만
  } else {
    Robot* r = findRobot(robot_id);
    if (!r) return errorResult(404, "unknown_robot");
    if (!controlOne(*r, cmd)) result = errorResult(409, "cannot_cancel");
  }
  dirty_ = true;
  afterChange();
  return result;
}

std::vector<TaskView> TaskManager::ordersOfTable(const std::string& table) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TaskView> out;
  for (auto& pair : tasks_) {
    const Task& t = pair.second;
    if (t.kind == TaskKind::SERVE && t.table == table) out.push_back(view(t));
  }
  return out;
}

std::vector<TableView> TaskManager::tables() {
  std::lock_guard<std::mutex> lock(mutex_);
  return buildSnapshot().tables;
}

std::vector<HistoryRow> TaskManager::history(const std::string& from, const std::string& to, const std::string& type) {
  std::lock_guard<std::mutex> lock(mutex_);
  return db_.queryHistory(from, to, type);
}

MapView TaskManager::mapView() {
  return map_.view(cfg_.board_w, cfg_.board_h);  // 맵은 바뀌지 않으니 잠글 필요 없음
}

// ================================================================ 로봇 소식

void TaskManager::onRobotState(const std::string& id, double x, double y, double yaw, uint8_t state,
                               const std::string& last_node, float battery) {
  std::lock_guard<std::mutex> lock(mutex_);
  Robot* r = findRobot(id);
  if (!r) return;
  if (!r->seen) {
    r->seen = true;
    r->online = true;
    logLine("TASK", id + " 첫 상태 수신");
  }
  r->x = x;
  r->y = y;
  r->yaw = yaw;
  r->state = state;
  r->last_node = last_node;
  r->battery = battery;
  dirty_ = true;

  // 배터리: 작업이 없을 때 기준보다 낮으면 충전, ready 이상이 되면 다시 배정
  if (cfg_.min_battery_pct > 0) {
    if (!r->charging && r->task_id < 0 && battery < cfg_.min_battery_pct) {
      r->charging = true;
      alert("INFO", id, "배터리 " + std::to_string(static_cast<int>(battery)) + "% - 충전할게요");
    } else if (r->charging && battery >= cfg_.ready_battery_pct) {
      r->charging = false;
      record("TASK", id + " 충전 완료 " + std::to_string(static_cast<int>(battery)) + "%");
      afterChange();
    }
  }

  // 로봇이 ERROR를 보고하면 한 번만 알림
  if (state == static_cast<uint8_t>(RobotRunState::ERROR)) {
    if (!r->error_alerted) alert("ERROR", id, "로봇이 오류 상태를 보고했어요");
    r->error_alerted = true;
  } else {
    r->error_alerted = false;
  }

  // 처음 위치 잡기 (도킹 위에서 켰는지도 여기서 검사)
  if (!traffic_.isLocalized(id)) {
    std::string reason;
    if (traffic_.localize(id, last_node, x, y, reason)) {
      r->loc_problem = "";
      afterChange();
    } else if (reason != r->loc_problem) {
      r->loc_problem = reason;
      alert("WARN", id, "위치 확인 실패: " + reason);
    }
  }
}

void TaskManager::onRobotEvent(const std::string& id, uint8_t type, uint32_t cmd_id, const std::string& node,
                               const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  Robot* r = findRobot(id);
  if (!r || !traffic_.isLocalized(id)) return;

  if (type == EVENT_NODE_REACHED) {
    traffic_.onNodeReached(id, node);
  } else if (type == EVENT_CMD_DONE) {
    traffic_.onCmdDone(id, cmd_id, node);
  } else if (type == EVENT_CMD_FAILED) {
    traffic_.onCmdFailed(id);
    r->fault = true;
    updateHold(*r);
    alert("ERROR", id, "이동 실패 " + reason + ". 확인 후 재개해 주세요");
  } else if (type == EVENT_BLOCKED) {
    alert("WARN", id, "진행이 막혔어요 (" + node + ")");
  }
  dirty_ = true;
  afterChange();
}

void TaskManager::onCommLost(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Robot* r = findRobot(id);
  if (!r || !r->online) return;
  r->online = false;
  updateHold(*r);
  alert("ERROR", id, "통신이 끊겼어요");
  dirty_ = true;
}

void TaskManager::onCommRestored(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Robot* r = findRobot(id);
  if (!r || r->online) return;
  r->online = true;
  alert("INFO", id, "통신이 다시 연결됐어요");
  if (!r->paused) traffic_.resend(id);  // 끊긴 동안 멈춘 로봇에게 남은 구간을 다시 보냄
  updateHold(*r);
  dirty_ = true;
  afterChange();
}

// ================================================================ 주기 처리

void TaskManager::tick() {
  std::lock_guard<std::mutex> lock(mutex_);
  Clock::time_point now = Clock::now();

  // 테이블 확인 시간 초과 -> 일이 끝난 것으로 보고 출발
  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.pending != Confirm::NONE && t.has_timeout && now >= t.timeout_at) {
      handleConfirm(t, t.pending, "TIMEOUT");
    }
  }
  afterChange();

  // snapshot: 바뀐 게 있으면 0.2초마다, 없어도 1초마다
  long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_snapshot_).count();
  if ((dirty_ && ms >= 200) || ms >= 1000) {
    if (client_) client_->pushSnapshot(buildSnapshot());
    dirty_ = false;
    last_snapshot_ = now;
  }
}

// ================================================================ 작업 흐름 (잠긴 상태에서만 부름)

void TaskManager::afterChange() {
  processArrivals();
  retryDocks();
  assignTables();
  dispatchGuide();
  dispatchServe();
  sendIdleRobotsHome();
  fillEntranceDock();
  processArrivals();
}

// Traffic이 알려준 "목적지 도착"을 다음 단계로
void TaskManager::processArrivals() {
  for (int round = 0; round < 5; ++round) {
    std::vector<Arrival> list = traffic_.takeArrivals();
    if (list.empty()) return;
    for (const Arrival& a : list) {
      Robot* r = findRobot(a.robot_id);
      if (!r) continue;
      dirty_ = true;
      if (r->homing) {
        r->homing = false;
        record("TASK", r->id + " 도킹 복귀 " + a.node);
        continue;
      }
      Task* t = findTask(r->task_id);
      if (!t || t->goal_node != a.node) continue;
      if (t->stage == Stage::TO_PICKUP) enterStage(*t, Stage::AT_PICKUP);
      else if (t->stage == Stage::TO_TABLE) enterStage(*t, Stage::AT_TABLE);
      else if (t->stage == Stage::TO_DOCK) enterStage(*t, Stage::DONE);
    }
  }
}

bool TaskManager::robotUsable(const Robot& r) {
  return r.seen && r.online && !r.paused && !r.fault && traffic_.isLocalized(r.id);
}

bool TaskManager::robotFree(const Robot& r) {
  return robotUsable(r) && !r.charging && r.task_id < 0 && !r.homing;
}

// 테이블을 기다리는 안내(먼저 온 순서)에 빈 테이블을 정해 준다
void TaskManager::assignTables() {
  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.kind != TaskKind::GUIDE || t.stage != Stage::WAITING || !t.table.empty()) continue;

    std::vector<TableConfig> free_tables;
    for (auto& tp : tables_) {
      if (tp.second.state != TableState::FREE) continue;
      TableConfig c;
      c.id = tp.second.id;
      c.seats = tp.second.seats;
      free_tables.push_back(c);
    }
    std::string table = dispatch_.pickTable(free_tables, t.party);
    if (table.empty()) continue;  // 맞는 자리가 없으면 다음 손님 (작은 테이블은 날 수 있음)

    Table& tb = tables_[table];
    setTable(tb, TableState::RESERVED, t.id);
    t.table = tb.id;
    t.table_node = tb.node;
    db_.saveTask(view(t));
    record("TASK", "guide task " + std::to_string(t.id) + " " + std::to_string(t.party) + "명 -> " + tb.id);
    dirty_ = true;
  }
}

// 테이블이 정해진 안내는 입구 도킹에 서 있는 로봇이 맡는다
void TaskManager::dispatchGuide() {
  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.kind != TaskKind::GUIDE || t.stage != Stage::WAITING || t.table.empty()) continue;
    if (tableBusy(t.table)) continue;  // 그 테이블에 서빙 로봇이 아직 있으면 떠난 뒤에

    std::string robot_id;
    for (auto& rp : robots_) {
      Robot& r = rp.second;
      if (robotFree(r) && traffic_.isStationary(r.id) && traffic_.currentNode(r.id) == cfg_.entrance_dock) {
        robot_id = r.id;
      }
    }
    if (robot_id.empty()) return;  // 입구 로봇이 없으면 다음 tick에 (입구는 한 대뿐)

    t.robot = robot_id;
    robots_[robot_id].task_id = t.id;
    enterStage(t, Stage::ASSIGNED);
    enterStage(t, Stage::TO_TABLE);
    return;
  }
}

void TaskManager::dispatchServe() {
  // 놀고 있는 로봇 목록
  std::vector<std::string> free_robots;
  for (auto& pair : robots_) {
    if (robotFree(pair.second)) free_robots.push_back(pair.first);
  }

  for (auto& pair : tasks_) {
    if (free_robots.empty()) return;
    Task& t = pair.second;
    if (t.kind != TaskKind::SERVE || t.stage != Stage::WAITING) continue;

    // 진행 중인 서빙 수
    int active = 0;
    for (auto& other_pair : tasks_) {
      const Task& o = other_pair.second;
      if (o.kind != TaskKind::SERVE) continue;
      if (o.stage == Stage::COOKING || o.stage == Stage::WAITING || isEnded(o.stage) || o.stage == Stage::TO_DOCK)
        continue;
      active++;
    }
    // 테이블 자리(pocket)에는 한 번에 한 대만: 같은 테이블로 가는 로봇이 떠날 때까지 기다린다
    if (!dispatch_.underServeLimit(active) || tableBusy(t.table)) continue;

    std::string robot_id = dispatch_.pickRobot(free_robots, pickup_node_);
    if (robot_id.empty()) continue;

    // 배정
    for (size_t i = 0; i < free_robots.size(); ++i) {
      if (free_robots[i] == robot_id) {
        free_robots.erase(free_robots.begin() + static_cast<long>(i));
        break;
      }
    }
    t.robot = robot_id;
    robots_[robot_id].task_id = t.id;
    enterStage(t, Stage::ASSIGNED);
    enterStage(t, Stage::TO_PICKUP);
  }
}

// 작업 없이 도킹 밖에 서 있는 로봇은 빈 도킹으로 (작업 취소 후, 충전하러)
void TaskManager::sendIdleRobotsHome() {
  for (auto& pair : robots_) {
    Robot& r = pair.second;
    if (!robotUsable(r) || r.task_id >= 0 || r.homing || !traffic_.isStationary(r.id)) continue;
    const MapNode* here = map_.node(traffic_.currentNode(r.id));
    if (!here || here->stop_kind == "DOCK") continue;
    std::string dock = dispatch_.pickDock(r.id, entranceOk(r.id));  // 충전할 로봇은 입구를 막지 않게
    if (dock.empty()) dock = dispatch_.pickDock(r.id, true);          // 입구밖에 없으면 입구라도
    if (dock.empty()) continue;
    if (traffic_.setGoal(r.id, dock)) {
      r.homing = true;
      record("TASK", r.id + " 작업 없음 -> 도킹 " + dock);
    }
  }
}

// 입구 도킹이 비어 있으면 다른 도킹에서 쉬는 로봇을 입구로 옮긴다.
// 입구 로봇이 충전 중인데 손님이 기다리면, 충전 로봇을 다른 빈 도킹으로 비켜 준다
void TaskManager::fillEntranceDock() {
  const std::string& entrance = cfg_.entrance_dock;
  if (!map_.node(entrance)) return;

  if (traffic_.isDockFree(entrance, "")) {
    std::string best;
    double best_cost = 1e9;
    for (auto& pair : robots_) {
      Robot& r = pair.second;
      if (!robotFree(r) || !traffic_.isStationary(r.id)) continue;
      const MapNode* here = map_.node(traffic_.currentNode(r.id));
      if (!here || here->stop_kind != "DOCK") continue;  // 도킹에서 쉬는 로봇만
      double c = traffic_.pathCost(r.id, entrance);
      if (c >= 0 && c < best_cost) {
        best_cost = c;
        best = r.id;
      }
    }
    if (best.empty()) return;
    if (traffic_.setGoal(best, entrance)) {
      robots_[best].homing = true;
      record("TASK", best + " 입구 도킹으로 이동");
    }
    return;
  }

  // 입구에 충전 중인 로봇이 서 있고, 손님이 기다리고, 대신할 로봇이 있으면 비켜 준다
  if (!hasWaitingGuide()) return;
  for (auto& pair : robots_) {
    Robot& r = pair.second;
    if (!r.charging || r.task_id >= 0 || r.homing || !robotUsable(r) || !traffic_.isStationary(r.id)) continue;
    if (traffic_.currentNode(r.id) != entrance) continue;
    bool substitute = false;
    for (auto& other : robots_) {
      if (other.first != r.id && robotFree(other.second) && traffic_.isStationary(other.first)) substitute = true;
    }
    if (!substitute) return;
    std::string dock = dispatch_.pickDock(r.id, false);
    if (dock.empty()) return;
    if (traffic_.setGoal(r.id, dock)) {
      r.homing = true;
      record("TASK", r.id + " 충전 중이라 입구를 비켜 줌 -> " + dock);
    }
    return;
  }
}

void TaskManager::retryDocks() {
  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.stage != Stage::TO_DOCK || !t.need_dock) continue;
    std::string dock = dispatch_.pickDock(t.robot, entranceOk(t.robot));
    if (dock.empty()) dock = dispatch_.pickDock(t.robot, true);
    if (dock.empty()) continue;
    t.need_dock = false;
    goTo(t, dock);
  }
}

// 단계가 바뀔 때는 항상 여기를 지난다
void TaskManager::enterStage(Task& t, Stage s) {
  t.stage = s;
  db_.saveTask(view(t));
  record("TASK", "task " + std::to_string(t.id) + " " + (t.table.empty() ? "-" : t.table) + " " + toString(t.kind) +
                     " -> " + toString(s) + (t.robot.empty() ? "" : " (" + t.robot + ")"));
  if (client_ && t.kind == TaskKind::SERVE) client_->pushTaskUpdate(view(t));
  dirty_ = true;

  // 끝난 단계: 확인 버튼 닫고, 예약한 테이블 풀고, 로봇 풀어 주기
  if (isEnded(s)) {
    t.ended_at = Clock::now();
    if (t.pending != Confirm::NONE) closeConfirm(t, "BUTTON");
    if (t.kind == TaskKind::GUIDE) {
      Table* tb = findTable(t.table);
      if (tb && tb->state == TableState::RESERVED && tb->task_id == t.id) setTable(*tb, TableState::FREE, -1);
    }
    Robot* r = findRobot(t.robot);
    if (r && r->task_id == t.id) {
      if (s != Stage::DONE) traffic_.clearGoal(r->id);
      r->task_id = -1;
      display(r->id, cfg_.display.idle, 0);
    }
    return;
  }

  switch (s) {
    case Stage::TO_PICKUP: goTo(t, pickup_node_); break;
    case Stage::TO_TABLE: goTo(t, t.table_node); break;
    case Stage::TO_DOCK: {
      std::string dock = dispatch_.pickDock(t.robot, entranceOk(t.robot));
      if (dock.empty()) dock = dispatch_.pickDock(t.robot, true);
      if (dock.empty()) {
        t.need_dock = true;  // retryDocks에서 다시 시도
        display(t.robot, stageText(t, s), 0);
      } else {
        goTo(t, dock);
      }
      break;
    }
    case Stage::AT_PICKUP: openConfirm(t, Confirm::LOADED, 0); break;
    case Stage::AT_TABLE:
      if (t.kind == TaskKind::SERVE) openConfirm(t, Confirm::RECEIVED, cfg_.table_timeout_sec);
      else openConfirm(t, Confirm::SEATED, cfg_.guide_timeout_sec);
      break;
    default: break;
  }
}

void TaskManager::goTo(Task& t, const std::string& node) {
  t.goal_node = node;
  display(t.robot, stageText(t, t.stage), 0);  // 문구 먼저 (카운트다운 지움), 그다음 이동
  if (!traffic_.setGoal(t.robot, node)) {
    alert("ERROR", t.robot, "task " + std::to_string(t.id) + " 경로를 만들 수 없어요");
    enterStage(t, Stage::FAILED);
  }
}

void TaskManager::openConfirm(Task& t, Confirm c, int countdown) {
  t.pending = c;
  t.countdown = countdown;
  t.has_timeout = countdown > 0;  // 테이블만 시간 초과 있음, 주방은 버튼으로만
  if (t.has_timeout) t.timeout_at = Clock::now() + std::chrono::seconds(countdown + cfg_.depart_grace_sec);
  display(t.robot, stageText(t, t.stage), countdown);

  ConfirmView v;
  v.task_id = t.id;
  v.confirm = c;
  v.countdown_sec = countdown;
  v.table = t.table;
  v.robot_id = t.robot;
  if (client_) client_->pushConfirmNeeded(v);
}

void TaskManager::closeConfirm(Task& t, const std::string& by) {
  ConfirmView v;
  v.task_id = t.id;
  v.confirm = t.pending;
  v.table = t.table;
  v.robot_id = t.robot;
  if (client_) client_->pushConfirmClosed(v, by);
  t.pending = Confirm::NONE;
  t.has_timeout = false;
}

// 버튼과 시간 초과가 같이 쓰는 입구. 버튼과 시간 초과가 겹치면 먼저 온 것 하나만 처리된다
ApiResult TaskManager::handleConfirm(Task& t, Confirm c, const std::string& by) {
  if (t.pending == Confirm::NONE || t.pending != c) return errorResult(409, "not_waiting");
  record("TASK", "task " + std::to_string(t.id) + " confirm " + toString(c) + " by " + by);
  closeConfirm(t, by);

  // 로봇이 멈춰 있어도 단계는 넘긴다. Traffic이 hold 중이면 재개될 때 출발한다
  if (t.stage == Stage::AT_PICKUP) {
    enterStage(t, Stage::TO_TABLE);
  } else if (t.stage == Stage::AT_TABLE) {
    if (t.kind == TaskKind::GUIDE) {
      Table* tb = findTable(t.table);
      if (tb) setTable(*tb, TableState::OCCUPIED, -1);  // 손님이 앉음
    }
    enterStage(t, Stage::TO_DOCK);
  }
  return ApiResult();
}

// 서빙은 조리 완료 전까지만, 안내는 손님이 앉기 전까지만
bool TaskManager::canCancel(const Task& t) const {
  if (t.kind == TaskKind::SERVE) return t.stage == Stage::COOKING;
  return t.stage == Stage::WAITING || t.stage == Stage::ASSIGNED || t.stage == Stage::TO_TABLE;
}

// 로봇이 있었으면 이미 보낸 구간까지 간 다음 빈 도킹으로 돌아간다 (sendIdleRobotsHome)
void TaskManager::doCancel(Task& t, const std::string& by) {
  record("TASK", "task " + std::to_string(t.id) + " 취소 by " + by);
  enterStage(t, Stage::CANCELED);
}

bool TaskManager::controlOne(Robot& r, ControlCmd cmd) {
  record("TASK", "control " + r.id + " " + toString(cmd));
  if (cmd == ControlCmd::PAUSE) {
    if (fleet_) fleet_->sendControl(r.id, ControlCmd::PAUSE);
    r.paused = true;
  } else if (cmd == ControlCmd::RESUME) {
    if (fleet_) fleet_->sendControl(r.id, ControlCmd::RESUME);
    r.paused = false;
    r.fault = false;
    traffic_.resend(r.id);  // 멈춰 있던 구간을 새 명령으로 다시
  } else {
    // CANCEL: 그 로봇의 작업을 취소 (규칙상 안 되면 아무것도 안 함)
    Task* t = findTask(r.task_id);
    if (!t || !canCancel(*t)) return false;
    doCancel(*t, "ROBOT " + r.id);
    if (r.fault) {
      r.fault = false;
      traffic_.resend(r.id);
    }
  }
  updateHold(r);
  return true;
}

// 일시정지 · 고장 · 통신 끊김 중 하나라도 있으면 Traffic이 경로를 늘리지 않게
void TaskManager::updateHold(Robot& r) {
  traffic_.setHold(r.id, r.paused || r.fault || !r.online);
}

void TaskManager::setTable(Table& tb, TableState s, int task_id) {
  tb.state = s;
  tb.task_id = task_id;
  db_.saveTableState(tb.id, s);
  dirty_ = true;
}

// ================================================================ 도우미

bool TaskManager::entranceOk(const std::string& robot_id) {
  Robot* r = findRobot(robot_id);
  if (!r || cfg_.min_battery_pct <= 0) return true;
  return !r->charging && r->battery >= cfg_.min_battery_pct;
}

// 테이블 자리는 막다른 pocket이라 두 대가 동시에 오가면 서로 막힌다 (나가는 로봇 ↔ 들어오는 로봇)
bool TaskManager::tableBusy(const std::string& table) const {
  for (auto& pair : tasks_) {
    const Task& o = pair.second;
    if (o.table != table || o.robot.empty()) continue;
    if (o.stage == Stage::ASSIGNED || o.stage == Stage::TO_PICKUP || o.stage == Stage::AT_PICKUP ||
        o.stage == Stage::TO_TABLE || o.stage == Stage::AT_TABLE)
      return true;
  }
  // 일을 마치고 TO_DOCK이 된 로봇도 pocket을 빠져나갈 때까지는 테이블 노드 lock을 쥐고 있다
  std::string node = map_.tableNode(table);
  for (const LockView& l : traffic_.locks()) {
    if (l.node == node) return true;
  }
  return false;
}

bool TaskManager::hasWaitingGuide() const {
  for (auto& pair : tasks_) {
    const Task& t = pair.second;
    if (t.kind == TaskKind::GUIDE && t.stage == Stage::WAITING && !t.table.empty()) return true;
  }
  return false;
}

int TaskManager::queuePos(const Task& t) const {
  if (t.kind != TaskKind::GUIDE || t.stage != Stage::WAITING || !t.table.empty()) return 0;
  int pos = 0;
  for (auto& pair : tasks_) {
    const Task& o = pair.second;
    if (o.kind == TaskKind::GUIDE && o.stage == Stage::WAITING && o.table.empty() && o.id <= t.id) pos++;
  }
  return pos;
}

TaskManager::Robot* TaskManager::findRobot(const std::string& id) {
  auto it = robots_.find(id);
  if (it == robots_.end()) return nullptr;
  return &it->second;
}

TaskManager::Task* TaskManager::findTask(int id) {
  auto it = tasks_.find(id);
  if (it == tasks_.end()) return nullptr;
  return &it->second;
}

TaskManager::Table* TaskManager::findTable(const std::string& id) {
  auto it = tables_.find(id);
  if (it == tables_.end()) return nullptr;
  return &it->second;
}

TaskView TaskManager::view(const Task& t) const {
  TaskView v;
  v.task_id = t.id;
  v.kind = t.kind;
  v.table = t.table;
  v.order_id = t.order_id;
  v.party = t.party;
  v.stage = t.stage;
  v.robot_id = t.robot;
  v.items = t.items;
  v.created_at = t.created_at;
  return v;
}

SnapshotView TaskManager::buildSnapshot() {
  SnapshotView s;
  for (const std::string& id : cfg_.robots) {
    const Robot& r = robots_[id];
    RobotView v;
    v.id = r.id;
    v.x = r.x;
    v.y = r.y;
    v.yaw = r.yaw;
    v.state = r.state;
    v.last_node = traffic_.isLocalized(id) ? traffic_.currentNode(id) : r.last_node;
    v.battery = r.battery;
    v.task_id = r.task_id;
    v.online = r.online;
    v.charging = r.charging;
    s.robots.push_back(v);
  }
  Clock::time_point now = Clock::now();
  for (auto& pair : tasks_) {
    const Task& t = pair.second;
    // 끝난 작업은 done_keep_sec 동안만 화면에 남긴다
    if (isEnded(t.stage) && now - t.ended_at > std::chrono::seconds(cfg_.done_keep_sec)) continue;
    s.tasks.push_back(view(t));
  }
  s.locks = traffic_.locks();
  for (const TableConfig& tc : cfg_.tables) {  // 설정 순서대로
    auto it = tables_.find(tc.id);
    if (it == tables_.end()) continue;
    TableView tv;
    tv.id = it->second.id;
    tv.seats = it->second.seats;
    tv.state = it->second.state;
    tv.task_id = it->second.task_id;
    s.tables.push_back(tv);
  }
  return s;
}

std::string TaskManager::stageText(const Task& t, Stage s) const {
  const DisplayTexts& d = cfg_.display;
  bool guide = t.kind == TaskKind::GUIDE;
  std::string text;
  switch (s) {
    case Stage::TO_PICKUP: text = d.to_pickup; break;
    case Stage::AT_PICKUP: text = d.at_pickup; break;
    case Stage::TO_TABLE: text = guide ? d.to_table_guide : d.to_table_serve; break;
    case Stage::AT_TABLE: text = guide ? d.at_table_guide : d.at_table_serve; break;
    case Stage::TO_DOCK: text = d.to_dock; break;
    default: text = d.idle; break;
  }
  size_t p = text.find("{table}");
  if (p != std::string::npos) text.replace(p, 7, t.table);
  return text;
}

void TaskManager::display(const std::string& robot_id, const std::string& text, int countdown) {
  if (!fleet_ || robot_id.empty()) return;
  DisplayCmd d;
  d.text = text;
  d.countdown_sec = static_cast<uint16_t>(countdown);
  fleet_->sendDisplay(robot_id, d);
}

void TaskManager::alert(const std::string& level, const std::string& robot_id, const std::string& msg) {
  if (client_) client_->pushAlert(level, robot_id, msg);
  record("ALERT", level + " " + robot_id + " " + msg);
}

void TaskManager::record(const std::string& type, const std::string& msg) {
  logLine(type, msg);
  db_.addHistory(type, msg);
}

}  // namespace pinky
