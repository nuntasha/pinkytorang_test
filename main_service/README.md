# pinkytorang_main — Main Service (서빙 + 안내)

C++17 · ROS 2 Jazzy. 로봇 3대가 **서빙**(주방 → 테이블)과 **안내**(입구 → 빈 테이블)를 한다.
퇴식은 사람이 하므로 회수 작업은 없다.

## 구조

```
 [고객 UI] [매장 UI] [입구 키오스크]          [pinky1~3]
        │ HTTP / WS                          │ ROS 2 토픽 (fleet_msgs)
 ClientLinkManager                     FleetLinkManager (또는 SimFleet)
        │  함수 호출                          │  함수 호출
        ▼                                     ▼
 ┌────────────── TaskManager (mutex 하나로 보호) ──────────────┐
 │  DispatchManager (로봇 · 테이블 · 도킹 고르기)  TrafficManager (경로 · lock) │
 └──────────────┬───────────────────────────────────┬─────────┘
            MapManager                          DBManager (작업 · 이력 · 테이블 상태)
```

## 작업 흐름

| 작업 | 단계 |
|---|---|
| 서빙 | `COOKING` →[조리 완료]→ `WAITING` → `ASSIGNED` → `TO_PICKUP` → `AT_PICKUP` →[적재 완료]→ `TO_TABLE` → `AT_TABLE` →[수령 / 30초]→ `TO_DOCK` → `DONE` |
| 안내 | 키오스크에서 빈 테이블 고름 → `WAITING`(입구 로봇 대기) → `ASSIGNED` → `TO_TABLE` → `AT_TABLE` →[착석 / 15초]→ `TO_DOCK` → `DONE` |
| 테이블 | `FREE` →(키오스크에서 고름)→ `RESERVED` →(착석)→ `OCCUPIED` →(직원 정리 완료)→ `FREE` |

규칙

- **취소**: 서빙은 `COOKING`(조리 완료 전)에서만, 안내는 착석 전(`WAITING · ASSIGNED · TO_TABLE`)까지만. 나머지는 409.
- **안내 테이블**: 손님이 입구 키오스크의 **좌석 배치도에서 빈 테이블을 직접 고른다** (인원수는 받지 않음). 고르는 순간 그 테이블이 예약되고, 이미 찬 테이블이면 409 `table_not_free`. 빈자리가 없으면 키오스크에 '빈자리 없음'만 표시 (대기열 없음).
- **스케줄링** (`TaskManager::afterChange`: 도착 처리 → 도킹 재시도 → 안내 배정 → 서빙 배정 → 쉬는 로봇 귀가 → 입구 자리 바꿈 → 대기 배치)
  - **동시 작업 수**: 빈 테이블이 있으면 안내 최대 `max_active_guide`(1) · 서빙 최대 `max_active_serve`(2), **만석이면 서빙은 로봇 전부**(안내할 일이 없으니)
  - **대기 배치**: 찬 테이블(사용 중 + 안내 중) < `busy_tables`(4)면 입구에 2대, 이상이면 1대, 만석이면 0대. 나머지는 픽업 쪽 도킹(`pickup_docks`). 쉬는 로봇을 자동으로 옮긴다 (`rebalanceStandby`)
  - **안내 로봇**: 입구 도킹(`entrance_docks`)에 서 있는 로봇만, 앞칸(D2) 로봇부터
  - **입구 도킹 두 칸 (`dock_via: {D1: D2}`)**: D1은 D2를 지나 드나든다. 입구는 뒤칸(D1)부터 채우고, D2에 로봇이 있으면 D1 로봇은 배정 · 이동 안 함. D1으로 오는데 D2에 쉬는 로봇이 있으면 D2 로봇을 D1으로 밀어 넣고 오던 로봇이 D2로 (`fixDockConflicts`)
  - **서빙 로봇 고르기**: 픽업까지 실제 경로가 가장 짧은 로봇. 만석이 아니면 입구 로봇은 `entrance_keep_penalty_m`만큼 멀게 친다
  - **이어받기**: 도킹으로 돌아가는 중인 로봇도 배터리가 `min_battery_pct` 이상이면 서빙을 받는다 (도킹 작업은 그 자리에서 `DONE`, 바로 픽업으로)
  - **같은 테이블 묶음**: 같은 테이블의 대기 주문은 한 로봇이 `serve_batch_max`(3)건까지 같이 싣는다. 적재 전(`ASSIGNED · TO_PICKUP · AT_PICKUP`)이면 나중에 조리 완료된 주문도 끼어든다. 앞장 작업이 이동 · 확인 버튼을 맡고 나머지는 단계만 따라간다 (묶음 작업 id로 확인 버튼을 눌러도 앞장 작업으로 처리)
  - **고장 · 통신 끊김**: `reassign_after_sec`(10초) 넘으면, 적재 전 서빙은 `WAITING`으로 되돌려 다른 로봇에 다시 배정. 적재 후 서빙 · 안내 중이면 직원 호출 알림만
  - **배터리**: 작업이 끝날 때(`TO_DOCK`)마다 확인해서 `min_battery_pct`보다 낮으면 충전(이어받기 · 입구 대기 안 함), `ready_battery_pct` 이상이면 다시 배정. 도킹에서 충전된다고 본다
- **교통**: 주방 통로(N12 → P1 → N4)만 일방통행이고 나머지 길은 전부 양방향 1차선
  - 다음 칸(노드)을 잡을 때 그 칸으로 가는 **길(엣지)도 같이** 잡는다 → 마주 오는 로봇이 같은 길에 못 들어옴
  - 앞이 막히면: 앞 로봇이 서 있으면(일하는 중 · 멈춤) 길이 상관없이 우회, 움직이는 중이면 우회가 `detour_max_m`(1 m) 이하로 길어질 때만 우회하고 아니면 기다림. `detour_wait_sec`(8초) 넘게 기다리면 길이 상관없이 우회
  - 서로 기다리는 교착이면 바로 우회, 우회 길도 없으면 한 대가 옆 칸으로 비켜 줌
- **후진 (fleet_msgs `Move.drive`)**: Move 하나에는 방향 하나. 규칙은 아래 맵 설명 참고. 나뉜 뒤쪽 Move는 같은 cmd_id로 APPEND
  - 예: `Move(REPLACE, FORWARD, [N16, N12])` + `Move(APPEND, REVERSE, [N13])` → 로봇은 N12에서 멈추고 돌아서 후진
  - 픽업 쪽 도킹 D3 · D4는 후진으로 들어가고 전진으로 나온다
  - 후진 중 PASS 노드의 yaw는 진행 방향 + 180°
- **테이블 자리(pocket)에는 한 번에 한 대**: 그 테이블로 가는 로봇이 있거나 테이블 노드 lock이 잡혀 있으면, 서빙 · 안내 모두 기다린다 (나가는 로봇 ↔ 들어오는 로봇 교착 방지).
- **배터리**: 위 스케줄링 참고. 충전할 로봇은 입구 도킹을 쓰지 않는다. `min_battery_pct: 0`이면 검사 안 함.
- **도킹에서 시작**: 처음 위치가 도킹 노드가 아니면 위치 확인을 하지 않는다 → 배정 안 됨 + 매장 UI 경고. (`require_dock_start`)
- **재시작**: 끝나지 않은 작업은 FAILED. 테이블은 사용 중(`OCCUPIED`)만 그대로, 안내 중(`RESERVED`)은 빈자리로.

## HTTP API

| 메서드 · 경로 | 누가 | 내용 |
|---|---|---|
| `GET /api/menu` | 고객 | 메뉴 |
| `POST /api/orders` `{table, items:[{menu_id, qty}]}` | 고객 | 주문 → `COOKING` |
| `GET /api/orders?table=T3` | 고객 | 그 테이블 서빙 목록 |
| `POST /api/tasks/{id}/ready` | 주방 | 조리 완료 |
| `POST /api/tasks/{id}/cancel` | 고객 · 매장 · 키오스크 | 취소 (규칙 밖이면 409 `cannot_cancel`) |
| `POST /api/tasks/{id}/confirm` `{confirm: LOADED / RECEIVED / SEATED}` | 주방 · 손님 | 확인 버튼 |
| `POST /api/guide` `{table}` | 키오스크 | 고른 테이블로 안내 요청 → `{task_id, table, stage}` (찬 테이블 409, 없는 테이블 404) |
| `GET /api/guide/{id}` | 키오스크 | 안내 상태 |
| `GET /api/tables` | 전부 | `{board:{w,h}, entrance:{x,y}, tables:[{id, x, y, w, h, state, task_id}]}` (배치도용) |
| `POST /api/tables/{id}/clear` | 매장 | 정리 완료 (`OCCUPIED` → `FREE`, 안내 중이면 409) |
| `POST /api/tables/{id}/occupy` | 매장 | 안내 없이 앉은 손님 (`FREE` → `OCCUPIED`) |
| `POST /api/robots/{id or all}/control` `{command: PAUSE / RESUME / CANCEL}` | 매장 | 로봇 제어 (CANCEL은 위 취소 규칙을 따름) |
| `GET /api/map`, `GET /api/history?from&to&type&format=csv` | 매장 | 맵 · 이력 |

WebSocket: `/ws/store` (snapshot에 `tables`, 로봇 `charging` 포함), `/ws/customer?table=T3` (`task_update`, 수령 · 착석 확인 버튼)

화면: `/store` 매장, `/c?table=T3` 고객, `/kiosk` 입구 키오스크(7인치 1024×600 터치)

## 설정 (config/main_service.yaml)

새로 생긴 항목: `tables`(id · 배치도 좌표 x, y · 크기 w, h), `entrance_docks` · `pickup_docks` · `dock_via`, `busy_tables` · `entrance_standby_quiet` · `entrance_standby_busy`, `max_active_guide`, `serve_batch_max`, `reassign_after_sec`, `entrance_keep_penalty_m`, `guide_timeout_sec`, `max_active_serve`, `min_battery_pct`, `ready_battery_pct`, `require_dock_start`, `display.to_table_guide / at_table_guide`

맵은 **fleet_msgs v7.1** (`config/node_graph.yaml` = fleet_msgs `docs/node_graph.yaml` 복사본, 그림 `config/node_map_v7.png`)
- ros-repo-2에 올릴 때는 설정의 `node_graph`를 `../../fleet_msgs/docs/node_graph.yaml`로 바꿔 복사 없이 읽고, `web_root`는 `../../../web`
- 좌표는 SLAM 맵 `fleet_msgs/docs/maps/project_map_v2` 기준 map 프레임. 맵이 바뀌면 fleet_msgs 쪽만 고치면 된다
- 홀 격자 N1~N13 (위 → 아래 줄, 왼 → 오), 테이블 T1~T6, 전부 양방향 1차선
- 입구 통로: N1 → D1(앞칸) → D2(안쪽) 한 줄. 둘 다 비면 D2부터 채우고, D2 로봇은 D1이 비어야 나온다 (`dock_via: {D2: D1}`)
- 주방 통로: N13 → N14 → N15 → N16 → P2(대기) → P1(픽업) → N4 일방통행, **N14부터 P1까지 후진**(`reverse`)
- 전진 / 후진 · Move 나누기 (`MapManager::driveReverse`, fleet_msgs README 4.5 · 4.9). **노드 그래프를 읽고 Move로 바꾸는 기준 구현은 main_service 하나**
  - `reverse` 구간: 후진 / 목적지 pocket으로 들어가는 길: entry 방향 / 출발 pocket에서 나오는 길: entry 반대 / 나머지(지나가는 pocket 포함): 전진
  - 방향이 바뀌는 곳, 목적지 pocket으로 들어가는 길에서 Move를 끊는다
  - 예: D2로 `FORWARD [.., N1, D1]` → `REVERSE [D2]`, D2에서 `FORWARD [D1, N1, ..]` / D3에서 `FORWARD [N16]` → `REVERSE [P2]` → `[P1]`
- 정차 방향(`yaw`)은 **그 노드가 목적지일 때만** 맞춘다 (가는 길에 지나가는 테이블 · D1은 진행 방향)
- 정밀 정차(dock / align / tape)는 로봇 쪽 `fleet_msgs/docs/precision_stops.yaml`. 서버는 모른다
- 키오스크 배치도는 `board_x · board_y · board_w · board_h`(map 좌표)와 `tables`(map 좌표)로 그린다

노드 그래프 형식 (`MapManager::load`, 두 형식 다 읽음)
- **fleet_msgs 형식** (팀 기준): 노드마다 `type`(PASS / STOP), `stop_kind`, `table`, `lock`, `pocket`, `entry`(FORWARD / REVERSE), `x`, `y`, `yaw`, `next`(갈 수 있는 다음 노드)
- **짧은 형식**: `nodes`(`{id, x, y}` + `kind`), `edges`(양방향 사슬), `oneway`(일방 사슬)
- 공통: `reverse`(후진 구간, 첫 · 끝 노드는 lock 노드), `two_way_zones`(선택)

## 빌드 · 실행

```bash
sudo apt install -y libasio-dev libyaml-cpp-dev libsqlite3-dev

# 워크스페이스: {src/fleet_msgs, src/main_service, web} (ros-repo-2 구조)
cd ~/ros-repo-2
colcon build --packages-select fleet_msgs pinkytorang_main   # fleet_msgs 0.7.1 (메시지는 v5부터 같음)
source install/setup.bash

# 가상 로봇 (ROS 없이 관제 · UI 확인, 2배속)
ros2 run pinkytorang_main main_service_sim src/main_service/config/main_service.yaml 2
# 실제 로봇
ros2 run pinkytorang_main main_service src/main_service/config/main_service.yaml
```

가상 로봇은 배터리를 흉내 낸다 (움직이면 줄고 도킹에서 참). pinky3은 12%로 시작해서 충전부터 한다.

## 실제 로봇 쪽에서 맞춰야 할 것

- `fleet/state`의 `battery`를 **% (0~100)** 로 보낸다. 값이 없으면 `min_battery_pct: 0`으로 꺼 둔다.
- 안내 중에는 손님이 따라오므로 로봇 쪽(fleet_agent / Nav2)에서 속도를 낮춘다. 서버는 Display 문구로만 알린다.
- **fleet_msgs**: `Move.drive`가 REVERSE면 waypoints 전체를 후진. 앞 Move와 drive가 다른 APPEND는 앞 Move 끝 노드에서 멈추고 돌아서 출발.
- P1(픽업)은 도킹처럼 정밀 정차: 도착 → yaw로 회전 → 테이프로 뒤를 맞춰 넣음 → `CMD_DONE`. 가상 로봇은 도킹 · 픽업에서 1초 쉬고 `CMD_DONE`.

## 교착 주의 (자물쇠 순서)

Task Manager는 자물쇠를 쥔 채로 `sendMove`, `pushSnapshot` 등을 부른다.
그러니 **Link 쪽은 자기 자물쇠를 쥔 채로 Task Manager를 부르면 안 된다**.
SimFleet · FleetLinkManager는 자물쇠 안에서 이벤트를 모으고, 자물쇠를 푼 뒤 Task Manager를 부른다.
