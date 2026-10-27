// Map Manager: node_graph.yaml 읽기 · 검사 · 길 찾기
// 두 형식을 읽는다 (자세한 형식은 map_manager.cpp load 위)
//   fleet_msgs 형식: nodes에 type · stop_kind · next · pocket · entry (fleet_msgs/docs/node_graph.yaml)
//   짧은 형식    : nodes(점) + edges(양방향 길) + oneway(일방통행 길)
//   둘 다 reverse(후진 구간) · two_way_zones 사용 가능
// 노드 성격은 ID 글자가 아니라 필드(kind, lock, no_wait, pocket)로만 판단한다
#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pinkytorang/types.hpp"

namespace pinky {

struct MapNode {
  std::string id;
  std::string type;       // PASS / STOP (kind가 있으면 STOP)
  std::string stop_kind;  // "" / QUEUE / PICKUP / SERVE / DOCK
  std::string table;      // SERVE만
  bool pocket = false;
  bool entry_reverse = false;  // pocket 진입 방향: false = 전진 진입 · 후진 출차, true = 후진 진입 · 전진 출차
  bool no_wait = false;
  bool lock = true;
  double x = 0.0;
  double y = 0.0;
  double yaw = 0.0;
  bool has_yaw = false;
  std::vector<std::string> next;
};

struct TwoWayZone {
  std::string id;
  std::vector<std::string> nodes;  // 맨 앞 · 맨 뒤가 끝 노드
};

class MapManager {
 public:
  // 성공하면 true. 실패하면 error에 문제 목록
  bool load(const std::string& path, std::string& error);

  const MapNode* node(const std::string& id) const;  // 없으면 nullptr
  const std::vector<MapNode>& nodes() const { return nodes_; }

  std::string tableNode(const std::string& table) const;            // 없으면 ""
  std::vector<std::string> nodesOfKind(const std::string& kind) const;

  // 후진 구간 (yaml reverse): a -> b 길을 후진으로 가는지
  bool isReverseEdge(const std::string& a, const std::string& b) const;
  // 경로(start -> goal) 위 a -> b를 후진으로 가는지: reverse 구간 + 포켓 드나듦 (fleet_msgs README 4.5)
  bool driveReverse(const std::string& a, const std::string& b, const std::string& start, const std::string& goal) const;
  // 포켓에 드나드는 길 (출발 pocket에서 나옴 / 목적지 pocket으로 들어감) = Move를 따로 끊는 곳
  bool isPocketEdge(const std::string& a, const std::string& b, const std::string& start, const std::string& goal) const;

  // 양방향 구간
  bool inZone(const std::string& id) const;
  bool isZoneEdge(const std::string& a, const std::string& b) const;  // a, b가 같은 구간 안
  bool isZoneEndOnly(const std::string& id) const;                    // 속한 모든 구간에서 끝 노드

  // 길 찾기 (다익스트라). 경로가 없으면 빈 vector
  std::vector<std::string> shortestPath(const std::string& from, const std::string& to) const;
  // ban 노드는 지나가지 않는다 (from · to는 예외). 우회 경로 찾기용
  std::vector<std::string> shortestPathAvoid(const std::string& from, const std::string& to,
                                             const std::set<std::string>& ban) const;
  double pathLength(const std::vector<std::string>& path) const;
  double distance(const std::string& a, const std::string& b) const;
  double heading(const std::string& from, const std::string& to) const;
  std::string nearestNode(double x, double y, double max_dist) const;  // 없으면 ""

  MapView view(double board_w, double board_h) const;

 private:
  std::vector<MapNode> nodes_;
  std::map<std::string, int> index_;  // id -> nodes_ 위치
  std::vector<TwoWayZone> zones_;
  std::set<std::pair<std::string, std::string>> reverse_edges_;  // (a, b) = a -> b 후진
};

}  // namespace pinky
