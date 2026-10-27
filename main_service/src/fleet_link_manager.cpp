#include "pinkytorang/fleet_link_manager.hpp"

#include <vector>

#include "pinkytorang/log.hpp"

namespace pinky {

FleetLinkManager::FleetLinkManager(rclcpp::Node::SharedPtr node, const Config& cfg) : node_(node), cfg_(cfg) {
  // QoS: fleet_msgs v7 README 2.3
  rclcpp::QoS reliable = rclcpp::QoS(10).reliable();
  rclcpp::QoS display_qos = rclcpp::QoS(1).reliable().transient_local();  // 늦게 켜진 로봇도 마지막 화면을 받음
  rclcpp::QoS state_qos = rclcpp::QoS(1).best_effort();                  // 자주 오니 하나 빠져도 괜찮음

  for (const std::string& id : cfg_.robots) {
    std::string prefix = "/" + id + "/fleet/";
    Link l;
    l.move_pub = node_->create_publisher<fleet_msgs::msg::Move>(prefix + "move", reliable);
    l.control_pub = node_->create_publisher<fleet_msgs::msg::Control>(prefix + "control", reliable);
    l.display_pub = node_->create_publisher<fleet_msgs::msg::Display>(prefix + "display", display_qos);
    l.state_sub = node_->create_subscription<fleet_msgs::msg::RobotState>(
        prefix + "state", state_qos,
        [this, id](const fleet_msgs::msg::RobotState::SharedPtr msg) { onState(id, *msg); });
    l.event_sub = node_->create_subscription<fleet_msgs::msg::RobotEvent>(
        prefix + "event", reliable,
        [this, id](const fleet_msgs::msg::RobotEvent::SharedPtr msg) { onEvent(id, *msg); });
    links_[id] = l;
    status_[id] = LinkStatus();
    logLine("FLEET", "토픽 준비: " + prefix + "{move,control,display,state,event}");
  }

  timer_ = node_->create_wall_timer(std::chrono::milliseconds(200), [this]() { checkLinks(); });
}

// ---------------------------------------------------------------- 서버 -> 로봇

void FleetLinkManager::sendMove(const std::string& robot_id, const MoveCmd& cmd) {
  if (!links_.count(robot_id)) return;
  fleet_msgs::msg::Move msg;
  msg.cmd_id = cmd.cmd_id;
  msg.mode = cmd.append ? fleet_msgs::msg::Move::APPEND : fleet_msgs::msg::Move::REPLACE;
  msg.drive = (cmd.drive == Drive::REVERSE) ? fleet_msgs::msg::Move::REVERSE : fleet_msgs::msg::Move::FORWARD;
  for (const WaypointCmd& w : cmd.waypoints) {
    fleet_msgs::msg::Waypoint wp;
    wp.node_id = w.node_id;
    wp.x = w.x;
    wp.y = w.y;
    wp.yaw = w.yaw;
    msg.waypoints.push_back(wp);
  }
  links_[robot_id].move_pub->publish(msg);
}

void FleetLinkManager::sendControl(const std::string& robot_id, ControlCmd cmd) {
  if (!links_.count(robot_id)) return;
  fleet_msgs::msg::Control msg;
  msg.command = static_cast<uint8_t>(cmd);  // PAUSE=0, RESUME=1, CANCEL=2 로 값이 같다
  links_[robot_id].control_pub->publish(msg);
  logLine("FLEET", robot_id + " Control " + toString(cmd));
}

void FleetLinkManager::sendDisplay(const std::string& robot_id, const DisplayCmd& cmd) {
  if (!links_.count(robot_id)) return;
  fleet_msgs::msg::Display msg;
  msg.text = cmd.text;
  msg.countdown_sec = cmd.countdown_sec;
  links_[robot_id].display_pub->publish(msg);
  logLine("FLEET", robot_id + " Display \"" + cmd.text + "\"");
}

// ---------------------------------------------------------------- 로봇 -> 서버 (ROS executor 스레드)

void FleetLinkManager::markReceived(const std::string& robot_id) {
  bool restored = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    LinkStatus& s = status_[robot_id];
    s.last_rx = std::chrono::steady_clock::now();
    s.ever = true;
    if (s.lost) {
      s.lost = false;
      restored = true;
    }
  }
  // 자물쇠를 푼 다음 Task Manager를 부른다
  if (restored && tasks_) {
    logLine("FLEET", robot_id + " 통신 복구");
    tasks_->onCommRestored(robot_id);
  }
}

void FleetLinkManager::onState(const std::string& robot_id, const fleet_msgs::msg::RobotState& msg) {
  markReceived(robot_id);
  if (tasks_) tasks_->onRobotState(robot_id, msg.x, msg.y, msg.yaw, msg.state, msg.last_node, msg.battery);
}

void FleetLinkManager::onEvent(const std::string& robot_id, const fleet_msgs::msg::RobotEvent& msg) {
  markReceived(robot_id);
  logLine("FLEET", robot_id + " event " + std::to_string(msg.type) + " " + msg.node_id + " (cmd " +
                       std::to_string(msg.cmd_id) + ")");
  if (tasks_) tasks_->onRobotEvent(robot_id, msg.type, msg.cmd_id, msg.node_id, msg.reason);
}

void FleetLinkManager::checkLinks() {
  std::vector<std::string> lost_now;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto& pair : status_) {
      LinkStatus& s = pair.second;
      double quiet = std::chrono::duration<double>(now - s.last_rx).count();
      if (s.ever && !s.lost && quiet > cfg_.comm_timeout_sec) {
        s.lost = true;
        lost_now.push_back(pair.first);
      }
    }
  }
  for (const std::string& id : lost_now) {
    logLine("FLEET", id + " 통신 끊김");
    if (tasks_) tasks_->onCommLost(id);
  }
}

}  // namespace pinky
