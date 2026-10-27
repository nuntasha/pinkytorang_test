// 설정 파일 (config/main_service.yaml)
#pragma once

#include <map>
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
  double x = 0.0;   // 키오스크 배치도에 그릴 테이블 중심 (m, 맵 좌표)
  double y = 0.0;
  double w = 0.165; // 테이블 크기 (m)
  double h = 0.110;
};

struct Config {
  int http_port = 8080;
  std::string web_root = "../web";
  std::string db_path = "pinkytorang.db";
  std::string node_graph = "config/node_graph.yaml";
  double board_x = 0.0;   // 판(UI 배치도) 왼쪽 아래의 map 좌표 (m). SLAM 맵 yaml의 origin
  double board_y = 0.0;
  double board_w = 2.28;  // 판 크기 (m)
  double board_h = 1.82;
  std::vector<std::string> robots = {"pinky1", "pinky2", "pinky3"};

  int table_timeout_sec = 30;    // 서빙: 손님 수령 카운트다운
  int guide_timeout_sec = 15;    // 안내: 착석 확인 카운트다운
  int depart_grace_sec = 2;      // 카운트다운 0 이후 실제 출발까지
  int max_active_serve = 2;      // 빈 테이블이 있을 때 동시에 서빙하는 로봇 최대 수 (만석이면 로봇 수 전부)
  int max_active_guide = 1;      // 동시에 안내하는 로봇 최대 수
  int serve_batch_max = 3;       // 같은 테이블 주문을 한 번에 싣는 최대 건수
  int reassign_after_sec = 10;   // 적재 전 로봇이 이만큼 고장 · 통신 끊김이면 다른 로봇에 다시 배정

  // 도킹 구역
  std::vector<std::string> entrance_docks = {"D2", "D1"};  // 입구 쪽 (앞칸부터). 안내는 여기 서 있는 로봇만
  std::vector<std::string> pickup_docks = {"D3", "D4"};    // 픽업 쪽
  std::map<std::string, std::string> dock_via;              // 뒤칸 -> 앞칸 (D1: D2 = D1은 D2를 지나 드나듦)
  double entrance_keep_penalty_m = 1.5; // 서빙 배정 때 입구 로봇에 더하는 비용 (입구 로봇을 아끼려고)

  // 대기 배치: 찬 테이블(사용 중 + 안내 중) 수에 따라 입구에 세워 둘 로봇 수. 만석이면 0
  int busy_tables = 4;           // 이 수 이상 차면 바쁨
  int entrance_standby_quiet = 2;
  int entrance_standby_busy = 1;

  // 배터리 (%). min_battery_pct = 0 이면 검사 안 함
  double min_battery_pct = 30.0;   // 이보다 낮으면 작업 배정 안 하고 충전
  double ready_battery_pct = 80.0; // 충전 중이면 이 이상이 돼야 다시 배정

  bool require_dock_start = true;  // 처음 위치는 도킹 노드에서만 인정
  double comm_timeout_sec = 3.0; // state가 이만큼 안 오면 통신 끊김
  int lock_lookahead = 2;        // 로봇 앞으로 미리 잡아 두는 lock 노드 수
  double detour_max_m = 1.0;     // 앞 로봇이 움직이는 중이면, 우회가 이만큼(m) 이하로 길어질 때만 돌아간다
  double detour_wait_sec = 8.0;  // 이만큼 기다려도 안 풀리면 길이 상관없이 우회
  double avoid_sec = 6.0;        // 우회를 정하면 막힌 노드를 이 시간 동안 경로 계산에서 뺀다
  double localize_radius_m = 0.15;
  int done_keep_sec = 600;       // 끝난 작업을 매장 화면에 남기는 시간

  std::vector<TableConfig> tables;
  std::vector<MenuItem> menu;
  DisplayTexts display;
};

// 파일을 읽어 cfg에 채운다. 실패하면 false + error에 이유
bool loadConfig(const std::string& path, Config& cfg, std::string& error);

}  // namespace pinky
