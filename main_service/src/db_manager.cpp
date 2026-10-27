#include "pinkytorang/db_manager.hpp"

#include "pinkytorang/log.hpp"

namespace pinky {

static const char* SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS tasks("
    "  task_id INTEGER PRIMARY KEY, kind TEXT, table_id TEXT, order_id INTEGER,"
    "  stage TEXT, robot_id TEXT, items TEXT, created_at TEXT, updated_at TEXT);"
    "CREATE TABLE IF NOT EXISTS history("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, time TEXT, type TEXT, message TEXT);"
    "CREATE TABLE IF NOT EXISTS table_state("
    "  table_id TEXT PRIMARY KEY, state TEXT, updated_at TEXT);";

// 결과 칸을 문자열로 (NULL이면 "")
static std::string columnText(sqlite3_stmt* st, int i) {
  const unsigned char* p = sqlite3_column_text(st, i);
  if (p == nullptr) return "";
  return reinterpret_cast<const char*>(p);
}

DBManager::~DBManager() {
  if (db_) sqlite3_close(db_);
}

bool DBManager::open(const std::string& path, std::string& error) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    error = "DB 열기 실패: " + path;
    return false;
  }
  char* err = nullptr;
  if (sqlite3_exec(db_, SCHEMA, nullptr, nullptr, &err) != SQLITE_OK) {
    error = std::string("DB 테이블 생성 실패: ") + (err ? err : "?");
    sqlite3_free(err);
    return false;
  }
  logLine("DB", "열림: " + path);
  return true;
}

int DBManager::queryInt(const char* sql) {
  sqlite3_stmt* st = nullptr;
  int value = 0;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    value = sqlite3_column_int(st, 0);
  }
  sqlite3_finalize(st);
  return value;
}

int DBManager::failUnfinishedTasks() {
  sqlite3_exec(db_,
               "UPDATE tasks SET stage='FAILED' "
               "WHERE stage NOT IN ('DONE','FAILED','CANCELED')",
               nullptr, nullptr, nullptr);
  return sqlite3_changes(db_);
}

int DBManager::maxTaskId() { return queryInt("SELECT COALESCE(MAX(task_id), 0) FROM tasks"); }

int DBManager::maxOrderId() { return queryInt("SELECT COALESCE(MAX(order_id), 0) FROM tasks"); }

void DBManager::saveTask(const TaskView& t) {
  // 메뉴는 "토마토 파스타 x2, 레몬에이드 x1" 같은 글자로 저장 (기록용). 안내는 빈 칸
  std::string items;
  for (size_t i = 0; i < t.items.size(); ++i) {
    if (i > 0) items += ", ";
    items += t.items[i].name + " x" + std::to_string(t.items[i].qty);
  }
  std::string now = nowIsoUtc();

  const char* sql =
      "INSERT INTO tasks(task_id, kind, table_id, order_id, stage, robot_id, items, created_at, updated_at) "
      "VALUES(?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(task_id) DO UPDATE SET stage=excluded.stage, robot_id=excluded.robot_id, "
      "table_id=excluded.table_id, updated_at=excluded.updated_at";  // 안내는 대기 중에 테이블이 정해짐
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  std::string kind = toString(t.kind);
  std::string stage = toString(t.stage);
  sqlite3_bind_int(st, 1, t.task_id);                          // ? 1번 자리에 값 넣기
  sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, t.table.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, t.order_id);
  sqlite3_bind_text(st, 5, stage.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, t.robot_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, items.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, t.created_at.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, now.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

void DBManager::addHistory(const std::string& type, const std::string& message) {
  std::string now = nowIsoUtc();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "INSERT INTO history(time, type, message) VALUES(?,?,?)", -1, &st, nullptr) != SQLITE_OK)
    return;
  sqlite3_bind_text(st, 1, now.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, message.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

void DBManager::saveTableState(const std::string& table, TableState state) {
  const char* sql =
      "INSERT INTO table_state(table_id, state, updated_at) VALUES(?,?,?) "
      "ON CONFLICT(table_id) DO UPDATE SET state=excluded.state, updated_at=excluded.updated_at";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  std::string s = toString(state);
  std::string now = nowIsoUtc();
  sqlite3_bind_text(st, 1, table.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, s.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, now.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::map<std::string, std::string> DBManager::loadTableStates() {
  std::map<std::string, std::string> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT table_id, state FROM table_state", -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) out[columnText(st, 0)] = columnText(st, 1);
  sqlite3_finalize(st);
  return out;
}

std::vector<HistoryRow> DBManager::queryHistory(const std::string& from, const std::string& to, const std::string& type) {
  std::vector<HistoryRow> rows;
  // 빈 문자열이면 그 조건은 안 거는 것. 최신 2000개, 최신이 위
  const char* sql =
      "SELECT time, type, message FROM history "
      "WHERE (?1 = '' OR time >= ?1) AND (?2 = '' OR time <= ?2) AND (?3 = '' OR type = ?3) "
      "ORDER BY id DESC LIMIT 2000";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return rows;
  sqlite3_bind_text(st, 1, from.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, to.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, type.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    HistoryRow r;
    r.time = columnText(st, 0);
    r.type = columnText(st, 1);
    r.message = columnText(st, 2);
    rows.push_back(r);
  }
  sqlite3_finalize(st);
  return rows;
}

}  // namespace pinky
