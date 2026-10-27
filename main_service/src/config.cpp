#include "pinkytorang/config.hpp"

#include <filesystem>

#include <yaml-cpp/yaml.h>

namespace pinky {

// 상대 경로를 설정 파일이 있는 폴더 기준으로 바꾼다
static std::string resolvePath(const std::string& base_dir, const std::string& p) {
  if (p.empty() || p[0] == '/') return p;
  std::filesystem::path full = std::filesystem::path(base_dir) / p;
  return full.lexically_normal().string();
}

bool loadConfig(const std::string& path, Config& cfg, std::string& error) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);

    if (root["http_port"]) cfg.http_port = root["http_port"].as<int>();
    if (root["web_root"]) cfg.web_root = root["web_root"].as<std::string>();
    if (root["db_path"]) cfg.db_path = root["db_path"].as<std::string>();
    if (root["node_graph"]) cfg.node_graph = root["node_graph"].as<std::string>();
    if (root["board_x"]) cfg.board_x = root["board_x"].as<double>();
    if (root["board_y"]) cfg.board_y = root["board_y"].as<double>();
    if (root["board_w"]) cfg.board_w = root["board_w"].as<double>();
    if (root["board_h"]) cfg.board_h = root["board_h"].as<double>();
    if (root["robots"]) cfg.robots = root["robots"].as<std::vector<std::string>>();
    if (root["table_timeout_sec"]) cfg.table_timeout_sec = root["table_timeout_sec"].as<int>();
    if (root["depart_grace_sec"]) cfg.depart_grace_sec = root["depart_grace_sec"].as<int>();
    if (root["guide_timeout_sec"]) cfg.guide_timeout_sec = root["guide_timeout_sec"].as<int>();
    if (root["max_active_serve"]) cfg.max_active_serve = root["max_active_serve"].as<int>();
    if (root["max_active_guide"]) cfg.max_active_guide = root["max_active_guide"].as<int>();
    if (root["serve_batch_max"]) cfg.serve_batch_max = root["serve_batch_max"].as<int>();
    if (root["reassign_after_sec"]) cfg.reassign_after_sec = root["reassign_after_sec"].as<int>();
    if (root["entrance_docks"]) cfg.entrance_docks = root["entrance_docks"].as<std::vector<std::string>>();
    else if (root["entrance_dock"]) cfg.entrance_docks = {root["entrance_dock"].as<std::string>()};  // 예전 설정
    if (root["pickup_docks"]) cfg.pickup_docks = root["pickup_docks"].as<std::vector<std::string>>();
    if (root["dock_via"]) cfg.dock_via = root["dock_via"].as<std::map<std::string, std::string>>();
    if (root["busy_tables"]) cfg.busy_tables = root["busy_tables"].as<int>();
    if (root["entrance_standby_quiet"]) cfg.entrance_standby_quiet = root["entrance_standby_quiet"].as<int>();
    if (root["entrance_standby_busy"]) cfg.entrance_standby_busy = root["entrance_standby_busy"].as<int>();
    if (root["entrance_keep_penalty_m"]) cfg.entrance_keep_penalty_m = root["entrance_keep_penalty_m"].as<double>();
    if (root["min_battery_pct"]) cfg.min_battery_pct = root["min_battery_pct"].as<double>();
    if (root["ready_battery_pct"]) cfg.ready_battery_pct = root["ready_battery_pct"].as<double>();
    if (root["require_dock_start"]) cfg.require_dock_start = root["require_dock_start"].as<bool>();
    if (root["comm_timeout_sec"]) cfg.comm_timeout_sec = root["comm_timeout_sec"].as<double>();
    if (root["lock_lookahead"]) cfg.lock_lookahead = root["lock_lookahead"].as<int>();
    if (root["detour_max_m"]) cfg.detour_max_m = root["detour_max_m"].as<double>();
    if (root["detour_wait_sec"]) cfg.detour_wait_sec = root["detour_wait_sec"].as<double>();
    if (root["avoid_sec"]) cfg.avoid_sec = root["avoid_sec"].as<double>();
    if (root["localize_radius_m"]) cfg.localize_radius_m = root["localize_radius_m"].as<double>();
    if (root["done_keep_sec"]) cfg.done_keep_sec = root["done_keep_sec"].as<int>();

    if (root["tables"]) {
      for (const YAML::Node& it : root["tables"]) {
        TableConfig t;
        t.id = it["id"].as<std::string>();
        if (it["x"]) t.x = it["x"].as<double>();
        if (it["y"]) t.y = it["y"].as<double>();
        if (it["w"]) t.w = it["w"].as<double>();
        if (it["h"]) t.h = it["h"].as<double>();
        cfg.tables.push_back(t);
      }
    }

    if (root["menu"]) {
      for (const YAML::Node& it : root["menu"]) {
        MenuItem m;
        m.id = it["id"].as<int>();
        m.name = it["name"].as<std::string>();
        if (it["price"]) m.price = it["price"].as<int>();
        if (it["desc"]) m.desc = it["desc"].as<std::string>();
        cfg.menu.push_back(m);
      }
    }

    YAML::Node d = root["display"];
    if (d) {
      if (d["to_pickup"]) cfg.display.to_pickup = d["to_pickup"].as<std::string>();
      if (d["at_pickup"]) cfg.display.at_pickup = d["at_pickup"].as<std::string>();
      if (d["to_table_serve"]) cfg.display.to_table_serve = d["to_table_serve"].as<std::string>();
      if (d["at_table_serve"]) cfg.display.at_table_serve = d["at_table_serve"].as<std::string>();
      if (d["to_table_guide"]) cfg.display.to_table_guide = d["to_table_guide"].as<std::string>();
      if (d["at_table_guide"]) cfg.display.at_table_guide = d["at_table_guide"].as<std::string>();
      if (d["to_dock"]) cfg.display.to_dock = d["to_dock"].as<std::string>();
      if (d["idle"]) cfg.display.idle = d["idle"].as<std::string>();
    }
  } catch (const std::exception& e) {
    error = "설정 파일 오류: " + path + " (" + e.what() + ")";
    return false;
  }

  std::string base_dir = std::filesystem::absolute(path).parent_path().string();
  cfg.web_root = resolvePath(base_dir, cfg.web_root);
  cfg.db_path = resolvePath(base_dir, cfg.db_path);
  cfg.node_graph = resolvePath(base_dir, cfg.node_graph);

  if (cfg.robots.empty()) {
    error = "robots 목록이 비어 있음";
    return false;
  }
  if (cfg.tables.empty()) {
    error = "tables 목록이 비어 있음 (테이블 id, x, y)";
    return false;
  }
  if (cfg.lock_lookahead < 1) cfg.lock_lookahead = 1;
  if (cfg.ready_battery_pct < cfg.min_battery_pct) cfg.ready_battery_pct = cfg.min_battery_pct;
  return true;
}

}  // namespace pinky
