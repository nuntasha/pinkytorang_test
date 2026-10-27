// 설정 파일 (config/main_service.yaml)
#pragma once

#include <string>
#include <vector>

#include "pinkytorang/types.hpp"

namespace pinky {

// LCD 문구. {table} 은 테이블 이름으로 바뀐다
struct DisplayTexts {
  std::string to_pickup = "";
  std::string at_pickup = "음식을 올려 주세요";
  std::string to_table_serve = "{table} 배달 중";
  std::string at_table_serve = "음식이 도착했습니다. 가져가 주세요";
  std::string to_table_guide = "{table}(으)로 안내할게요. 따라오세요";
  std::string at_table_guide = "{table}입니다. 자리에 앉아 주세요";
  std::string to_dock = "";
  std::string idle = "";
};

struct TableConfig {
  std::string id;   // 노드 그래프의 table 값과 같아야 함
  int seats = 2;
};

struct Config {
  int http_port = 8080;
  std::string web_root = "../web";
  std::string db_path = "pinkytorang.db";
  std::string node_graph = "config/node_graph.yaml";
  double board_w = 2.28;
  double board_h = 1.82;
  std::vector<std::string> robots = {"pinky1", "pinky2", "pinky3"};

  int table_timeout_sec = 30;    // 서빙: 손님 수령 카운트다운
  int guide_timeout_sec = 15;    // 안내: 착석 확인 카운트다운
  int depart_grace_sec = 2;      // 카운트다운 0 이후 실제 출발까지
  int max_active_serve = 2;      // 동시에 서빙하는 로봇 최대 수

  // 안내
  std::string entrance_dock = "D1";     // 손님을 맞는 입구 도킹 (안내는 여기 있는 로봇만)
  double entrance_keep_penalty_m = 1.5; // 서빙 배정 때 입구 로봇에 더하는 비용 (입구 로봇을 아끼려고)

  // 배터리 (%). min_battery_pct = 0 이면 검사 안 함
  double min_battery_pct = 30.0;   // 이보다 낮으면 작업 배정 안 하고 충전
  double ready_battery_pct = 80.0; // 충전 중이면 이 이상이 돼야 다시 배정

  bool require_dock_start = true;  // 처음 위치는 도킹 노드에서만 인정
  double comm_timeout_sec = 3.0; // state가 이만큼 안 오면 통신 끊김
  int lock_lookahead = 2;        // 로봇 앞으로 미리 잡아 두는 lock 노드 수
  double localize_radius_m = 0.15;
  int done_keep_sec = 600;       // 끝난 작업을 매장 화면에 남기는 시간

  std::vector<TableConfig> tables;
  std::vector<MenuItem> menu;
  DisplayTexts display;
};

// 파일을 읽어 cfg에 채운다. 실패하면 false + error에 이유
bool loadConfig(const std::string& path, Config& cfg, std::string& error);

}  // namespace pinky
