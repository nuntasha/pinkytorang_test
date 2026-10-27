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
| 안내 | `WAITING`(테이블 · 입구 로봇 대기) → `ASSIGNED` → `TO_TABLE` → `AT_TABLE` →[착석 / 15초]→ `TO_DOCK` → `DONE` |
| 테이블 | `FREE` →(안내 배정)→ `RESERVED` →(착석)→ `OCCUPIED` →(직원 정리 완료)→ `FREE` |

규칙

- **취소**: 서빙은 `COOKING`(조리 완료 전)에서만, 안내는 착석 전(`WAITING · ASSIGNED · TO_TABLE`)까지만. 나머지는 409.
- **안내 테이블**: 인원 이상 좌석인 빈 테이블 중 **좌석이 가장 적은 곳**. 없으면 대기열(먼저 온 순서, 맞는 자리가 나면 바로 배정).
- **안내 로봇**: `entrance_dock`(입구 도킹)에 서 있는 로봇만 안내한다.
  - 일을 마친 로봇은 입구 도킹이 비어 있으면 입구로 먼저 들어간다.
  - 입구가 비면 다른 도킹에서 쉬는 로봇을 입구로 옮긴다.
  - 서빙 배정 때 입구 로봇은 `entrance_keep_penalty_m` 만큼 멀게 쳐서, 비슷한 거리면 다른 로봇이 서빙한다.
- **테이블 자리(pocket)에는 한 번에 한 대**: 그 테이블로 가는 로봇이 있거나 테이블 노드 lock이 잡혀 있으면, 서빙 · 안내 모두 기다린다 (나가는 로봇 ↔ 들어오는 로봇 교착 방지).
- **배터리**: 작업이 없을 때 `min_battery_pct`보다 낮으면 충전(배정 안 함), `ready_battery_pct` 이상이면 다시 배정. 충전할 로봇은 입구 도킹을 쓰지 않는다. `min_battery_pct: 0`이면 검사 안 함.
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
| `POST /api/guide` `{party}` | 키오스크 | 안내 요청 → `{task_id, table, queue_pos, stage}` (`table: null` = 대기) |
| `GET /api/guide/{id}` | 키오스크 | 안내 상태 |
| `GET /api/tables` | 전부 | `[{id, seats, state, task_id}]` |
| `POST /api/tables/{id}/clear` | 매장 | 정리 완료 (`OCCUPIED` → `FREE`, 안내 중이면 409) |
| `POST /api/tables/{id}/occupy` | 매장 | 안내 없이 앉은 손님 (`FREE` → `OCCUPIED`) |
| `POST /api/robots/{id or all}/control` `{command: PAUSE / RESUME / CANCEL}` | 매장 | 로봇 제어 (CANCEL은 위 취소 규칙을 따름) |
| `GET /api/map`, `GET /api/history?from&to&type&format=csv` | 매장 | 맵 · 이력 |

WebSocket: `/ws/store` (snapshot에 `tables`, 로봇 `charging` 포함), `/ws/customer?table=T3` (`task_update`, 수령 · 착석 확인 버튼)

화면: `/store` 매장, `/c?table=T3` 고객, `/kiosk` 입구 키오스크(7인치 1024×600 터치)

## 설정 (config/main_service.yaml)

새로 생긴 항목: `tables`(id · seats), `entrance_dock`, `entrance_keep_penalty_m`, `guide_timeout_sec`, `max_active_serve`, `min_battery_pct`, `ready_battery_pct`, `require_dock_start`, `display.to_table_guide / at_table_guide`

맵은 **v6** (`config/node_graph.yaml`, 그림 `config/node_map_v6.png`): 왼쪽 입구 통로(입구 도킹 D1, 양방향 1차선),
홀 테이블 2×2(T1~T4, 길 위 정차), 오른쪽 주방 통로(아래 → 위: 도킹 D2 · D3 → 픽업 대기 P1 → 픽업 P2).
좌표는 2280 × 1820 mm 판 기준 설계값이라, SLAM 맵이 나오면 실측 좌표로 바꾼다.

## 빌드 · 실행

```bash
sudo apt install -y libasio-dev libyaml-cpp-dev libsqlite3-dev

# 워크스페이스: ~/projects/Pinkytorang/{src/fleet_msgs, src/main_service, web}
cd ~/projects/Pinkytorang
colcon build --packages-select fleet_msgs pinkytorang_main
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

## 교착 주의 (자물쇠 순서)

Task Manager는 자물쇠를 쥔 채로 `sendMove`, `pushSnapshot` 등을 부른다.
그러니 **Link 쪽은 자기 자물쇠를 쥔 채로 Task Manager를 부르면 안 된다**.
SimFleet · FleetLinkManager는 자물쇠 안에서 이벤트를 모으고, 자물쇠를 푼 뒤 Task Manager를 부른다.
