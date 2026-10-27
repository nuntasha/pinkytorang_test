#include "pinkytorang/task_manager.hpp"

#include <algorithm>

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
    tables_[tc.id] = tb;
  }
  std::vector<std::string> docks = cfg.entrance_docks;
  docks.insert(docks.end(), cfg.pickup_docks.begin(), cfg.pickup_docks.end());
  for (const auto& via : cfg.dock_via) {
    docks.push_back(via.first);
    docks.push_back(via.second);
  }
  for (const std::string& id : docks) {
    const MapNode* n = map_.node(id);
    if (!n || n->stop_kind != "DOCK") logLine("TASK", "설정 오류: " + id + " 가 도킹 노드가 아님");
  }
  if (cfg.entrance_docks.empty()) logLine("TASK", "설정 오류: entrance_docks 비어 있음 (안내 불가)");
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

ApiResult TaskManager::requestGuide(const std::string& table) {
  std::lock_guard<std::mutex> lock(mutex_);
  Table* tb = findTable(table);
  if (!tb) return errorResult(404, "unknown_table");
  if (tb->state != TableState::FREE) return errorResult(409, "table_not_free");  // 다른 키오스크가 먼저 골랐거나 사용 중

  Task t;
  t.id = next_task_id_++;
  t.kind = TaskKind::GUIDE;
  t.table = tb->id;
  t.table_node = tb->node;
  t.created_at = nowIsoUtc();
  tasks_[t.id] = t;
  setTable(*tb, TableState::RESERVED, t.id);  // 고른 순간 예약 (다른 손님이 못 고르게)
  db_.saveTask(view(t));
  record("ORDER", "guide " + table + " -> task " + std::to_string(t.id));
  dirty_ = true;
  afterChange();  // 입구 로봇이 있으면 바로 출발

  Task& now = tasks_[t.id];
  ApiResult ok;
  ok.task_id = now.id;
  ok.table = now.table;
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
  if (t->lead >= 0 && findTask(t->lead)) t = findTask(t->lead);  // 묶음 작업이면 앞장 작업의 버튼
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

  traffic_.tick();  // 막혀서 기다리는 로봇: 우회 · 대기 시간 다시 판단
  recoverStuckTasks(now);

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
  dispatchGuide();
  dispatchServe();
  sendIdleRobotsHome();
  fixDockConflicts();
  rebalanceStandby();
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
  return robotUsable(r) && !r.charging && r.task_id < 0 && !r.homing && !dispatch_.dockBlocked(r.id);
}

// 안내는 입구 도킹에 서 있는 로봇이 맡는다 (앞칸 로봇부터). 동시에 max_active_guide 대까지
void TaskManager::dispatchGuide() {
  int active = 0;
  for (auto& pair : tasks_) {
    const Task& o = pair.second;
    if (o.kind == TaskKind::GUIDE &&
        (o.stage == Stage::ASSIGNED || o.stage == Stage::TO_TABLE || o.stage == Stage::AT_TABLE))
      active++;
  }
  if (active >= cfg_.max_active_guide) return;

  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.kind != TaskKind::GUIDE || t.stage != Stage::WAITING) continue;
    if (tableBusy(t.table)) continue;  // 그 테이블에 서빙 로봇이 아직 있으면 떠난 뒤에

    std::string robot_id;
    for (const std::string& dock : cfg_.entrance_docks) {
      for (auto& rp : robots_) {
        Robot& r = rp.second;
        if (robot_id.empty() && robotFree(r) && traffic_.isStationary(r.id) && traffic_.currentNode(r.id) == dock)
          robot_id = r.id;
      }
    }
    if (robot_id.empty()) return;  // 입구 로봇이 없으면 다음에 (rebalanceStandby가 채움)

    t.robot = robot_id;
    robots_[robot_id].task_id = t.id;
    enterStage(t, Stage::ASSIGNED);
    enterStage(t, Stage::TO_TABLE);
    return;
  }
}

// 같은 테이블로 가는, 아직 적재 전인 앞장 작업 (자리가 남아 있으면)
TaskManager::Task* TaskManager::joinableTrip(const Task& t) {
  for (auto& pair : tasks_) {
    Task& l = pair.second;
    if (l.id == t.id || l.kind != TaskKind::SERVE || l.lead >= 0 || l.table != t.table || l.robot.empty()) continue;
    if (l.stage != Stage::ASSIGNED && l.stage != Stage::TO_PICKUP && l.stage != Stage::AT_PICKUP) continue;
    if (static_cast<int>(l.riders.size()) + 1 >= cfg_.serve_batch_max) continue;
    return &l;
  }
  return nullptr;
}

void TaskManager::dispatchServe() {
  // 만석이면 안내할 일이 없으니 로봇 전부 서빙, 아니면 max_active_serve 대까지
  bool full = !tables_.empty() && freeTableCount() == 0;
  int limit = full ? static_cast<int>(robots_.size()) : cfg_.max_active_serve;

  // 일을 줄 수 있는 로봇: 놀고 있거나, 도킹으로 돌아가는 중인데 배터리가 충분한 로봇 (이어받기)
  std::vector<std::string> candidates;
  for (auto& pair : robots_) {
    Robot& r = pair.second;
    if (!robotUsable(r) || r.charging || dispatch_.dockBlocked(r.id) || !entranceOk(r.id)) continue;
    if (r.task_id < 0) {
      candidates.push_back(r.id);  // 쉬는 중 · 도킹으로 가는 중(homing)
      continue;
    }
    const Task* cur = findTask(r.task_id);
    if (cur && cur->stage == Stage::TO_DOCK) candidates.push_back(r.id);
  }

  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.kind != TaskKind::SERVE || t.stage != Stage::WAITING || t.lead >= 0) continue;

    // 1) 같은 테이블로 아직 적재 전인 로봇이 있으면 같이 싣는다
    if (Task* l = joinableTrip(t)) {
      t.lead = l->id;
      l->riders.push_back(t.id);
      mirrorStage(t, l->stage, *l);
      continue;
    }
    if (candidates.empty()) continue;

    // 진행 중인 서빙 (앞장 작업만 = 로봇 수)
    int active = 0;
    for (auto& other_pair : tasks_) {
      const Task& o = other_pair.second;
      if (o.kind != TaskKind::SERVE || o.lead >= 0) continue;
      if (o.stage == Stage::ASSIGNED || o.stage == Stage::TO_PICKUP || o.stage == Stage::AT_PICKUP ||
          o.stage == Stage::TO_TABLE || o.stage == Stage::AT_TABLE)
        active++;
    }
    // 테이블 자리(pocket)에는 한 번에 한 대만: 같은 테이블로 가는 로봇이 떠날 때까지 기다린다
    if (active >= limit || tableBusy(t.table)) continue;

    std::string robot_id = dispatch_.pickRobot(candidates, pickup_node_, !full);
    if (robot_id.empty()) continue;
    candidates.erase(std::find(candidates.begin(), candidates.end(), robot_id));

    // 2) 이어받기: 도킹으로 가던 작업은 여기서 끝낸다
    Robot& r = robots_[robot_id];
    if (r.task_id >= 0) {
      Task* old = findTask(r.task_id);
      if (old) {
        record("TASK", robot_id + " 도킹 복귀 중 다음 서빙 이어받기 (task " + std::to_string(old->id) + " -> " +
                           std::to_string(t.id) + ")");
        enterStage(*old, Stage::DONE);
      }
    }
    r.homing = false;

    // 3) 같은 테이블의 다른 대기 주문도 같이 싣는다
    for (auto& other_pair : tasks_) {
      Task& o = other_pair.second;
      if (static_cast<int>(t.riders.size()) + 1 >= cfg_.serve_batch_max) break;
      if (o.id == t.id || o.kind != TaskKind::SERVE || o.stage != Stage::WAITING || o.lead >= 0 || o.table != t.table)
        continue;
      o.lead = t.id;
      t.riders.push_back(o.id);
    }

    t.robot = robot_id;
    r.task_id = t.id;
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
    std::string dock = pickDockFor(r.id);
    if (dock.empty()) continue;
    if (traffic_.setGoal(r.id, dock)) {
      r.homing = true;
      record("TASK", r.id + " 작업 없음 -> 도킹 " + dock);
    }
  }
}

int TaskManager::freeTableCount() const {
  int n = 0;
  for (const auto& pair : tables_) {
    if (pair.second.state == TableState::FREE) n++;
  }
  return n;
}

// 한가할 때(찬 테이블 < busy_tables) 입구 2대, 바쁠 때 1대, 만석이면 0대 (전부 픽업 쪽)
int TaskManager::entranceStandbyWanted() const {
  int busy = static_cast<int>(tables_.size()) - freeTableCount();
  int want = (busy >= cfg_.busy_tables) ? cfg_.entrance_standby_busy : cfg_.entrance_standby_quiet;
  if (!tables_.empty() && freeTableCount() == 0) want = 0;
  return std::min(want, static_cast<int>(cfg_.entrance_docks.size()));
}

int TaskManager::entranceStandbyCount() const {
  int n = 0;
  for (const auto& pair : robots_) {
    const Robot& r = pair.second;
    if (!r.seen || !r.online || r.charging || !traffic_.isLocalized(r.id)) continue;
    std::string target = traffic_.isStationary(r.id) ? traffic_.currentNode(r.id) : traffic_.goalOf(r.id);
    if (dispatch_.isEntranceDock(target)) n++;
  }
  return n;
}

bool TaskManager::wantEntrance(const std::string& robot_id) const {
  const auto it = robots_.find(robot_id);
  if (it == robots_.end() || it->second.charging) return false;  // 충전할 로봇은 입구를 막지 않게
  if (cfg_.min_battery_pct > 0 && it->second.battery < cfg_.min_battery_pct) return false;
  return entranceStandbyCount() < entranceStandbyWanted();
}

std::string TaskManager::pickDockFor(const std::string& robot_id) const {
  return dispatch_.pickDock(robot_id, wantEntrance(robot_id));
}

// 쉬는 로봇 자리 맞추기: 입구에 있어야 할 수(entranceStandbyWanted)보다 적으면 픽업 쪽에서 데려오고,
// 많으면 입구 로봇(앞칸부터)을 픽업 쪽으로 보낸다. 손님이 기다리는데 입구 로봇이 충전 중이면 비켜 준다
void TaskManager::rebalanceStandby() {
  int want = entranceStandbyWanted();
  int have = entranceStandbyCount();

  if (have < want) {
    std::string best;
    double best_cost = 1e9;
    for (auto& pair : robots_) {
      Robot& r = pair.second;
      if (!robotFree(r) || !traffic_.isStationary(r.id) || !entranceOk(r.id)) continue;
      const std::string here = traffic_.currentNode(r.id);
      const MapNode* n = map_.node(here);
      if (!n || n->stop_kind != "DOCK" || dispatch_.isEntranceDock(here)) continue;  // 다른 도킹에서 쉬는 로봇만
      std::string dock = dispatch_.pickDock(r.id, true);
      if (!dispatch_.isEntranceDock(dock)) continue;
      double c = traffic_.pathCost(r.id, dock);
      if (c >= 0 && c < best_cost) {
        best_cost = c;
        best = r.id;
      }
    }
    if (best.empty()) return;
    std::string dock = dispatch_.pickDock(best, true);
    if (traffic_.setGoal(best, dock)) {
      robots_[best].homing = true;
      record("TASK", best + " 입구 대기로 이동 -> " + dock + " (입구 " + std::to_string(have) + "/" +
                         std::to_string(want) + ")");
    }
    return;
  }

  if (have > want) {
    for (const std::string& entrance : cfg_.entrance_docks) {  // 앞칸 로봇부터 (뒤칸은 앞칸이 비어야 나옴)
      for (auto& pair : robots_) {
        Robot& r = pair.second;
        if (!robotFree(r) || !traffic_.isStationary(r.id) || traffic_.currentNode(r.id) != entrance) continue;
        std::string dock = dispatch_.pickDock(r.id, false);
        if (dock.empty() || dispatch_.isEntranceDock(dock)) return;  // 픽업 쪽이 꽉 참
        if (traffic_.setGoal(r.id, dock)) {
          r.homing = true;
          record("TASK", r.id + " 픽업 쪽 대기로 이동 -> " + dock + " (입구 " + std::to_string(have) + "/" +
                             std::to_string(want) + ")");
        }
        return;
      }
    }
    return;
  }

  // 입구에 충전 중인 로봇이 서 있고, 손님이 기다리고, 입구에 쓸 로봇이 없으면 비켜 준다
  if (!hasWaitingGuide() || have > 0) return;
  for (auto& pair : robots_) {
    Robot& r = pair.second;
    if (!r.charging || r.task_id >= 0 || r.homing || !robotUsable(r) || !traffic_.isStationary(r.id)) continue;
    if (!dispatch_.isEntranceDock(traffic_.currentNode(r.id)) || dispatch_.dockBlocked(r.id)) continue;
    std::string dock = dispatch_.pickDock(r.id, false);
    if (dock.empty() || dispatch_.isEntranceDock(dock)) return;
    if (traffic_.setGoal(r.id, dock)) {
      r.homing = true;
      record("TASK", r.id + " 충전 중이라 입구를 비켜 줌 -> " + dock);
    }
    return;
  }
}

bool TaskManager::redirectDock(Robot& r, const std::string& dock) {
  if (!traffic_.setGoal(r.id, dock)) return false;
  Task* t = findTask(r.task_id);
  if (t && t->stage == Stage::TO_DOCK) t->goal_node = dock;  // 도착 판정용
  return true;
}

// 뒤칸(D1)은 앞칸(D2)을 지나야 들어간다. 뒤칸으로 가는 로봇이 있는데 앞칸에 다른 로봇이 서 있으면 (켤 때 순서 등)
//   앞칸 로봇이 쉬는 중이면: 앞칸 로봇을 뒤칸으로 밀어 넣고, 오던 로봇은 앞칸으로
//   못 움직이면: 오던 로봇을 다른 빈 도킹으로
void TaskManager::fixDockConflicts() {
  for (const auto& via : cfg_.dock_via) {
    const std::string& back = via.first;
    const std::string& front = via.second;
    std::string parked;
    for (auto& pair : robots_) {
      if (traffic_.isLocalized(pair.first) && traffic_.isStationary(pair.first) &&
          traffic_.currentNode(pair.first) == front)
        parked = pair.first;
    }
    if (parked.empty()) continue;
    for (auto& pair : robots_) {
      Robot& x = pair.second;
      if (x.id == parked || traffic_.goalOf(x.id) != back) continue;
      Robot& y = robots_[parked];
      if (robotUsable(y) && y.task_id < 0 && !y.homing && traffic_.isDockFree(back, x.id)) {
        if (traffic_.setGoal(y.id, back) && redirectDock(x, front)) {
          y.homing = true;
          record("TASK", "입구 도킹 자리 바꿈: " + y.id + " " + front + " -> " + back + ", " + x.id + " -> " + front);
        }
      } else {
        std::string dock = dispatch_.pickDock(x.id, false);
        if (!dock.empty() && dock != back && redirectDock(x, dock))
          record("TASK", x.id + " " + back + " 앞칸에 로봇이 있어 " + dock + "(으)로");
      }
      return;
    }
  }
}

void TaskManager::retryDocks() {
  for (auto& pair : tasks_) {
    Task& t = pair.second;
    if (t.stage != Stage::TO_DOCK || !t.need_dock || t.lead >= 0) continue;
    std::string dock = pickDockFor(t.robot);
    if (dock.empty()) continue;
    t.need_dock = false;
    goTo(t, dock);
  }
}

// ---------------------------------------------------------------- 고장 · 통신 끊김

// reassign_after_sec 넘게 고장 · 통신 끊김이면
//   적재 전 서빙(ASSIGNED · TO_PICKUP · AT_PICKUP): 로봇에서 떼어 다른 로봇에 다시 배정
//   적재 후 서빙 / 안내 중: 음식 · 손님이 로봇에 있으니 직원 호출 알림만 (한 번)
void TaskManager::recoverStuckTasks(Clock::time_point now) {
  for (auto& pair : robots_) {
    Robot& r = pair.second;
    bool bad = r.seen && (!r.online || r.fault);
    if (!bad) {
      r.bad = false;
      r.stuck_alerted = false;
      continue;
    }
    if (!r.bad) {
      r.bad = true;
      r.bad_since = now;
    }
    if (r.task_id < 0 || now - r.bad_since < std::chrono::seconds(cfg_.reassign_after_sec)) continue;
    Task* t = findTask(r.task_id);
    if (!t) continue;
    if (t->kind == TaskKind::SERVE &&
        (t->stage == Stage::ASSIGNED || t->stage == Stage::TO_PICKUP || t->stage == Stage::AT_PICKUP)) {
      unassign(*t, r.online ? "고장" : "통신 끊김");
    } else if (!r.stuck_alerted && (t->stage == Stage::TO_TABLE || t->stage == Stage::AT_TABLE ||
                                    t->stage == Stage::ASSIGNED)) {
      r.stuck_alerted = true;
      if (t->kind == TaskKind::SERVE)
        alert("ERROR", r.id, "음식을 실은 채 멈췄어요 (" + t->table + "). 직원이 직접 전달해 주세요");
      else
        alert("ERROR", r.id, "안내 중 멈췄어요 (" + t->table + "). 직원이 손님을 안내해 주세요");
    }
  }
}

void TaskManager::unassign(Task& t, const std::string& why) {
  std::string old = t.robot;
  if (t.pending != Confirm::NONE) closeConfirm(t, "REASSIGN");
  Robot* r = findRobot(old);
  if (r && r->task_id == t.id) {
    r->task_id = -1;
    traffic_.clearGoal(old);  // 다시 살아나면 sendIdleRobotsHome이 도킹으로 보낸다
  }
  t.robot = "";
  t.goal_node = "";
  t.need_dock = false;
  enterStage(t, Stage::WAITING);  // 묶음 작업도 같이 WAITING
  for (int id : t.riders) {
    Task* o = findTask(id);
    if (o) o->lead = -1;
  }
  t.riders.clear();
  alert("WARN", old, "task " + std::to_string(t.id) + " (" + t.table + ") " + why + " " +
                         std::to_string(cfg_.reassign_after_sec) + "초 -> 다른 로봇에 다시 배정할게요");
}

// 묶음 작업: 앞장 작업의 단계만 따라간다 (이동 · 확인 버튼 · 로봇 풀기는 앞장 작업이 함)
void TaskManager::mirrorStage(Task& o, Stage s, const Task& lead) {
  o.stage = s;
  o.robot = lead.robot;
  if (isEnded(s)) o.ended_at = Clock::now();
  db_.saveTask(view(o));
  record("TASK", "task " + std::to_string(o.id) + " " + o.table + " " + toString(o.kind) + " -> " + toString(s) +
                     (o.robot.empty() ? "" : " (" + o.robot + ")") + " 묶음 task " + std::to_string(lead.id));
  if (client_) client_->pushTaskUpdate(view(o));
  dirty_ = true;
}

// 단계가 바뀔 때는 항상 여기를 지난다
void TaskManager::enterStage(Task& t, Stage s) {
  t.stage = s;
  db_.saveTask(view(t));
  record("TASK", "task " + std::to_string(t.id) + " " + (t.table.empty() ? "-" : t.table) + " " + toString(t.kind) +
                     " -> " + toString(s) + (t.robot.empty() ? "" : " (" + t.robot + ")"));
  if (client_ && t.kind == TaskKind::SERVE) client_->pushTaskUpdate(view(t));
  dirty_ = true;
  for (int id : t.riders) {  // 같이 실은 작업도 같은 단계로
    Task* o = findTask(id);
    if (o && !isEnded(o->stage)) mirrorStage(*o, s, t);
  }

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
      // 작업이 끝날 때마다 배터리 확인: 낮으면 충전 (이어받기 · 입구 대기 안 함)
      Robot* r = findRobot(t.robot);
      if (r && cfg_.min_battery_pct > 0 && !r->charging && r->battery < cfg_.min_battery_pct) {
        r->charging = true;
        alert("INFO", r->id, "배터리 " + std::to_string(static_cast<int>(r->battery)) + "% - 도킹에서 충전할게요");
      }
      std::string dock = pickDockFor(t.robot);
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
    if (t.kind == TaskKind::GUIDE && t.stage == Stage::WAITING) return true;
  }
  return false;
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
    tv.x = tc.x;
    tv.y = tc.y;
    tv.w = tc.w;
    tv.h = tc.h;
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
