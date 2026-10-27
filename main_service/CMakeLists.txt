cmake_minimum_required(VERSION 3.16)
project(pinkytorang_main CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 필요한 라이브러리 찾기 (없으면 여기서 에러)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "asio가 없습니다: sudo apt install libasio-dev")
endif()

# ROS 2가 있나? (없어도 에러 아님. 있으면 실제 로봇용 main_service까지 빌드)
find_package(ament_cmake QUIET)

# ---------------------------------------------------------------- 공통 부분 (ROS 없음)
add_library(pinkytorang_core STATIC
  src/types.cpp
  src/config.cpp
  src/map_manager.cpp
  src/traffic_manager.cpp
  src/dispatch_manager.cpp
  src/task_manager.cpp
  src/db_manager.cpp
  src/client_link_manager.cpp
  src/sim_fleet.cpp
)
target_include_directories(pinkytorang_core PUBLIC include third_party ${ASIO_INCLUDE_DIR})
target_compile_definitions(pinkytorang_core PUBLIC ASIO_STANDALONE)
target_link_libraries(pinkytorang_core PUBLIC Threads::Threads yaml-cpp::yaml-cpp SQLite::SQLite3)
set_target_properties(pinkytorang_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- 시뮬레이션 (ROS 없이 가상 로봇)
add_executable(main_service_sim src/main_sim.cpp)
target_link_libraries(main_service_sim PRIVATE pinkytorang_core)

# ---------------------------------------------------------------- 실제 로봇용 (ROS 2 Jazzy)
if(ament_cmake_FOUND)
  find_package(rclcpp REQUIRED)
  find_package(fleet_msgs REQUIRED)

  add_executable(main_service src/main.cpp src/fleet_link_manager.cpp)
  target_link_libraries(main_service pinkytorang_core)
  ament_target_dependencies(main_service rclcpp fleet_msgs)

  install(TARGETS main_service main_service_sim DESTINATION lib/${PROJECT_NAME})
  install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
  ament_package()
endif()
