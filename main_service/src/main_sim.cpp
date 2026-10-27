// Main Service 시뮬레이션 (ROS 없이 가상 로봇 3대)
// 실행: ./main_service_sim [설정 파일] [배속]
//   예) ./main_service_sim ../config/main_service.yaml 2

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "pinkytorang/client_link_manager.hpp"
#include "pinkytorang/config.hpp"
#include "pinkytorang/db_manager.hpp"
#include "pinkytorang/dispatch_manager.hpp"
#include "pinkytorang/log.hpp"
#include "pinkytorang/map_manager.hpp"
#include "pinkytorang/sim_fleet.hpp"
#include "pinkytorang/task_manager.hpp"
#include "pinkytorang/traffic_manager.hpp"

using namespace pinky;

static std::atomic<bool> g_running(true);
static void onSignal(int) { g_running = false; }  // Ctrl+C

int main(int argc, char** argv) {
  std::string config_path = "../config/main_service.yaml";
  double scale = 1.0;
  if (argc > 1) config_path = argv[1];
  if (argc > 2) scale = std::stod(argv[2]);

  // 1. 설정 · 맵 · DB
  Config cfg;
  std::string error;
  if (!loadConfig(config_path, cfg, error)) {
    std::cerr << error << std::endl;
    return 1;
  }
  MapManager map;
  if (!map.load(cfg.node_graph, error)) {
    std::cerr << error << std::endl;
    return 1;
  }
  DBManager db;
  if (!db.open(cfg.db_path, error)) {
    std::cerr << error << std::endl;
    return 1;
  }

  // 2. 판단 모듈
  TrafficManager traffic(cfg, map);
  for (const std::string& id : cfg.robots) traffic.addRobot(id);
  DispatchManager dispatch(cfg, map, traffic);
  TaskManager tasks(cfg, map, traffic, dispatch, db);

  // 3. 입구 · 출구 연결 (로봇 자리에 가상 로봇)
  SimFleet sim(cfg, map, 0.15, scale);
  ClientLinkManager client(cfg, tasks);
  sim.setTaskManager(&tasks);
  traffic.setFleetOut(&sim);
  tasks.setFleetOut(&sim);
  tasks.setClientOut(&client);

  // 4. 시작
  tasks.start();
  client.start();
  sim.start();
  std::signal(SIGINT, onSignal);
  logLine("MAIN", "시뮬레이션 실행 중 (Ctrl+C 종료)");

  // 5. 0.1초마다 주기 처리 (테이블 시간 초과, snapshot)
  while (g_running) {
    tasks.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  sim.stop();
  client.stop();
  return 0;
}
