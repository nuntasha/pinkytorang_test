// DB Manager: SQLite에 작업 · 이력 · 테이블 상태 저장, 이력 조회
// Task Manager 안에서만 부르므로 (Task Manager 자물쇠 안) 따로 잠그지 않는다
#pragma once

#include <map>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "pinkytorang/types.hpp"

namespace pinky {

class DBManager {
 public:
  ~DBManager();

  bool open(const std::string& path, std::string& error);

  // 서버가 꺼질 때 끝나지 않은 작업을 FAILED로 바꾼다 (중복 실행 방지). 바꾼 개수를 돌려줌
  int failUnfinishedTasks();
  int maxTaskId();
  int maxOrderId();

  void saveTask(const TaskView& t);  // 없으면 추가, 있으면 단계 · 로봇만 갱신
  void addHistory(const std::string& type, const std::string& message);

  // 테이블 상태 (재시작해도 사용 중인 테이블은 유지)
  void saveTableState(const std::string& table, TableState state);
  std::map<std::string, std::string> loadTableStates();  // 테이블 -> "FREE"/"RESERVED"/"OCCUPIED"
  std::vector<HistoryRow> queryHistory(const std::string& from, const std::string& to, const std::string& type);

 private:
  int queryInt(const char* sql);
  sqlite3* db_ = nullptr;
};

}  // namespace pinky
