// Fleet Link Manager: 로봇과 ROS 2 토픽으로 통신 (fleet_msgs v7)
// - 서버 쪽 토픽 이름: /pinky1/fleet/{move,control,display,state,event}
// - 받은 메시지는 Task Manager 함수로 넘긴다. 통신 끊김은 감지만 하고 처리는 Task Manager가 한다
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include <fleet_msgs/msg/control.hpp>
#include <fleet_msgs/msg/display.hpp>
#include <fleet_msgs/msg/move.hpp>
#include <fleet_msgs/msg/robot_event.hpp>
#include <fleet_msgs/msg/robot_state.hpp>

#include "pinkytorang/config.hpp"
#include "pinkytorang/ports.hpp"
#include "pinkytorang/task_manager.hpp"

namespace pinky {

class FleetLinkManager : public IFleetOut {
 public:
  FleetLinkManager(rclcpp::Node::SharedPtr node, const Config& cfg);

  void setTaskManager(TaskManager* tasks) { tasks_ = tasks; }

  // IFleetOut (Task Manager가 부름)
  void sendMove(const std::string& robot_id, const MoveCmd& cmd) override;
  void sendControl(const std::string& robot_id, ControlCmd cmd) override;
  void sendDisplay(const std::string& robot_id, const DisplayCmd& cmd) override;

 private:
  struct Link {
    rclcpp::Publisher<fleet_msgs::msg::Move>::SharedPtr move_pub;
    rclcpp::Publisher<fleet_msgs::msg::Control>::SharedPtr control_pub;
    rclcpp::Publisher<fleet_msgs::msg::Display>::SharedPtr display_pub;
    rclcpp::Subscription<fleet_msgs::msg::RobotState>::SharedPtr state_sub;
    rclcpp::Subscription<fleet_msgs::msg::RobotEvent>::SharedPtr event_sub;
  };

  struct LinkStatus {
    std::chrono::steady_clock::time_point last_rx;
    bool ever = false;  // 한 번이라도 받았나
    bool lost = false;
  };

  void onState(const std::string& robot_id, const fleet_msgs::msg::RobotState& msg);
  void onEvent(const std::string& robot_id, const fleet_msgs::msg::RobotEvent& msg);
  void markReceived(const std::string& robot_id);
  void checkLinks();  // 0.2초마다: 3초 넘게 조용한 로봇 찾기

  rclcpp::Node::SharedPtr node_;
  const Config& cfg_;
  TaskManager* tasks_ = nullptr;
  std::map<std::string, Link> links_;  // 만든 뒤로 바뀌지 않음

  std::mutex mutex_;                     // status_ 보호
  std::map<std::string, LinkStatus> status_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // namespace pinky
