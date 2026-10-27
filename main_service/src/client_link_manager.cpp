#include "pinkytorang/client_link_manager.hpp"

#include <fstream>
#include <sstream>

#include "pinkytorang/log.hpp"

namespace pinky {

using crow::json::wvalue;
using Conn = crow::websocket::connection;

// ---------------------------------------------------------------- JSON 도우미

static crow::response jsonResponse(int code, const wvalue& j) {
  crow::response res(code, j.dump());
  res.set_header("Content-Type", "application/json; charset=utf-8");
  res.set_header("Cache-Control", "no-store");
  return res;
}

static crow::response errorResponse(int code, const std::string& error) {
  wvalue j;
  j["error"] = error;
  return jsonResponse(code, j);
}

static wvalue taskJson(const TaskView& t) {
  wvalue j;
  j["task_id"] = t.task_id;
  j["kind"] = toString(t.kind);
  j["table"] = t.table;
  if (t.order_id >= 0) j["order_id"] = t.order_id;
  else j["order_id"] = nullptr;
  j["stage"] = toString(t.stage);
  if (!t.robot_id.empty()) j["robot_id"] = t.robot_id;
  else j["robot_id"] = nullptr;
  wvalue::list items;
  for (const OrderItem& i : t.items) {
    wvalue it;
    it["menu_id"] = i.menu_id;
    it["name"] = i.name;
    it["qty"] = i.qty;
    items.push_back(std::move(it));
  }
  j["items"] = std::move(items);
  j["created_at"] = t.created_at;
  return j;
}

static wvalue tableJson(const TableView& t) {
  wvalue j;
  j["id"] = t.id;
  j["x"] = t.x;
  j["y"] = t.y;
  j["w"] = t.w;
  j["h"] = t.h;
  j["state"] = toString(t.state);
  if (t.task_id >= 0) j["task_id"] = t.task_id;
  else j["task_id"] = nullptr;
  return j;
}

// 안내 결과 / 상태 (키오스크)
static wvalue guideJson(const ApiResult& r) {
  wvalue j;
  j["task_id"] = r.task_id;
  if (!r.table.empty()) j["table"] = r.table;
  else j["table"] = nullptr;
  j["stage"] = r.stage;
  return j;
}

static crow::response okResponse() {
  wvalue j;
  j["ok"] = true;
  return jsonResponse(200, j);
}

// 손님이 누르는 확인(수령 · 착석)은 그 테이블 고객 UI에도 띄운다
static bool isGuestConfirm(Confirm c) { return c == Confirm::RECEIVED || c == Confirm::SEATED; }

// CSV 칸: 큰따옴표로 감싸고 안의 큰따옴표는 두 번
static std::string csvField(const std::string& s) {
  std::string out = "\"";
  for (char ch : s) {
    if (ch == '"') out += "\"\"";
    else if (ch == '\n') out += ' ';
    else out += ch;
  }
  return out + "\"";
}

// ---------------------------------------------------------------- 시작 · 종료

ClientLinkManager::ClientLinkManager(const Config& cfg, TaskManager& tasks) : cfg_(cfg), tasks_(tasks) {
  app_.loglevel(crow::LogLevel::Warning);
  setupRoutes();
}

ClientLinkManager::~ClientLinkManager() { stop(); }

void ClientLinkManager::start() {
  app_.signal_clear();  // Ctrl+C는 main이 처리
  app_.bindaddr("0.0.0.0").port(static_cast<uint16_t>(cfg_.http_port)).multithreaded();
  thread_ = std::thread([this]() { app_.run(); });
  app_.wait_for_server_start();
  logLine("HTTP", "서버 시작: http://<이 PC IP>:" + std::to_string(cfg_.http_port) + "/store , /c?table=T1 , /kiosk");
}

void ClientLinkManager::stop() {
  if (!thread_.joinable()) return;
  app_.stop();
  thread_.join();
}

// ---------------------------------------------------------------- 정적 파일 (web/)

crow::response ClientLinkManager::serveFile(const std::string& rel) {
  if (rel.find("..") != std::string::npos) return crow::response(400);  // 폴더 밖 파일 막기
  std::ifstream f(cfg_.web_root + "/" + rel, std::ios::binary);
  if (!f) return crow::response(404);
  std::stringstream ss;
  ss << f.rdbuf();
  crow::response res(ss.str());

  std::string ext = rel.substr(rel.find_last_of('.') + 1);
  std::string type = "application/octet-stream";
  if (ext == "html") type = "text/html; charset=utf-8";
  else if (ext == "css") type = "text/css; charset=utf-8";
  else if (ext == "js") type = "application/javascript; charset=utf-8";
  else if (ext == "png") type = "image/png";
  else if (ext == "svg") type = "image/svg+xml";
  res.set_header("Content-Type", type);
  res.set_header("Cache-Control", "no-store");
  return res;
}

// ---------------------------------------------------------------- 라우트

void ClientLinkManager::setupRoutes() {
  // ---- 화면
  CROW_ROUTE(app_, "/")([]() {
    crow::response r;
    r.redirect("/store");
    return r;
  });
  CROW_ROUTE(app_, "/c")([this]() { return serveFile("customer/index.html"); });
  CROW_ROUTE(app_, "/store")([this]() { return serveFile("store/index.html"); });
  CROW_ROUTE(app_, "/kiosk")([this]() { return serveFile("kiosk/index.html"); });
  CROW_ROUTE(app_, "/common/<path>")([this](const std::string& p) { return serveFile("common/" + p); });
  CROW_ROUTE(app_, "/customer/<path>")([this](const std::string& p) { return serveFile("customer/" + p); });
  CROW_ROUTE(app_, "/store/<path>")([this](const std::string& p) { return serveFile("store/" + p); });
  CROW_ROUTE(app_, "/kiosk/<path>")([this](const std::string& p) { return serveFile("kiosk/" + p); });

  // ---- GET /api/menu
  CROW_ROUTE(app_, "/api/menu")([this]() {
    wvalue j;
    wvalue::list arr;
    for (const MenuItem& m : cfg_.menu) {
      wvalue o;
      o["id"] = m.id;
      o["name"] = m.name;
      o["price"] = m.price;
      o["desc"] = m.desc;
      arr.push_back(std::move(o));
    }
    j["menu"] = std::move(arr);
    return jsonResponse(200, j);
  });

  // ---- GET /api/orders?table=T3, POST /api/orders
  CROW_ROUTE(app_, "/api/orders").methods("GET"_method, "POST"_method)([this](const crow::request& req) {
    if (req.method == "GET"_method) {
      const char* table = req.url_params.get("table");
      if (!table) return errorResponse(400, "table_required");
      wvalue j;
      wvalue::list arr;
      for (const TaskView& t : tasks_.ordersOfTable(table)) arr.push_back(taskJson(t));
      j["tasks"] = std::move(arr);
      return jsonResponse(200, j);
    }

    logLine("HTTP", "POST /api/orders " + req.body);
    crow::json::rvalue body = crow::json::load(req.body);
    if (!body || !body.has("table") || !body.has("items") || body["items"].t() != crow::json::type::List)
      return errorResponse(400, "bad_request");
    std::vector<OrderItem> items;
    for (const crow::json::rvalue& it : body["items"]) {
      if (!it.has("menu_id") || !it.has("qty")) return errorResponse(400, "bad_request");
      OrderItem item;
      item.menu_id = static_cast<int>(it["menu_id"].i());
      item.qty = static_cast<int>(it["qty"].i());
      items.push_back(item);
    }
    ApiResult r = tasks_.createOrder(std::string(body["table"].s()), items);
    if (r.status != 200) return errorResponse(r.status, r.error);
    wvalue j;
    j["order_id"] = r.order_id;
    j["task_id"] = r.task_id;
    return jsonResponse(200, j);
  });

  // ---- POST /api/tasks/3/ready  (주방: 조리 완료)
  CROW_ROUTE(app_, "/api/tasks/<int>/ready").methods("POST"_method)([this](int task_id) {
    logLine("HTTP", "POST /api/tasks/" + std::to_string(task_id) + "/ready");
    ApiResult r = tasks_.markCooked(task_id);
    if (r.status != 200) return errorResponse(r.status, r.error);
    return okResponse();
  });

  // ---- POST /api/tasks/3/cancel  (서빙은 조리 중, 안내는 착석 전까지)
  CROW_ROUTE(app_, "/api/tasks/<int>/cancel").methods("POST"_method)([this](int task_id) {
    logLine("HTTP", "POST /api/tasks/" + std::to_string(task_id) + "/cancel");
    ApiResult r = tasks_.cancelTask(task_id);
    if (r.status != 200) return errorResponse(r.status, r.error);
    return okResponse();
  });

  // ---- POST /api/guide {"table":"T3"}  (입구 키오스크: 손님이 배치도에서 고른 빈 테이블)
  CROW_ROUTE(app_, "/api/guide").methods("POST"_method)([this](const crow::request& req) {
    logLine("HTTP", "POST /api/guide " + req.body);
    crow::json::rvalue body = crow::json::load(req.body);
    if (!body || !body.has("table")) return errorResponse(400, "bad_request");
    ApiResult r = tasks_.requestGuide(std::string(body["table"].s()));
    if (r.status != 200) return errorResponse(r.status, r.error);
    return jsonResponse(200, guideJson(r));
  });

  // ---- GET /api/guide/7  (키오스크가 대기 상태를 물어봄)
  CROW_ROUTE(app_, "/api/guide/<int>")([this](int task_id) {
    ApiResult r = tasks_.guideStatus(task_id);
    if (r.status != 200) return errorResponse(r.status, r.error);
    return jsonResponse(200, guideJson(r));
  });

  // ---- GET /api/tables  (키오스크 배치도: 판 크기 · 입구 위치 · 테이블 위치와 상태)
  CROW_ROUTE(app_, "/api/tables")([this]() {
    wvalue j;
    wvalue::list arr;
    for (const TableView& t : tasks_.tables()) arr.push_back(tableJson(t));
    j["tables"] = std::move(arr);
    j["board"]["x"] = cfg_.board_x;
    j["board"]["y"] = cfg_.board_y;
    j["board"]["w"] = cfg_.board_w;
    j["board"]["h"] = cfg_.board_h;
    for (const MapNodeView& n : tasks_.mapView().nodes) {
      if (!cfg_.entrance_docks.empty() && n.id == cfg_.entrance_docks.front()) {
        j["entrance"]["x"] = n.x;
        j["entrance"]["y"] = n.y;
      }
    }
    return jsonResponse(200, j);
  });

  // ---- POST /api/tables/T3/clear (정리 완료 -> 빈자리), /occupy (안내 없이 앉음 -> 사용 중)
  CROW_ROUTE(app_, "/api/tables/<string>/<string>")
      .methods("POST"_method)([this](const std::string& table, const std::string& action) {
        logLine("HTTP", "POST /api/tables/" + table + "/" + action);
        ApiResult r;
        if (action == "clear") r = tasks_.clearTable(table);
        else if (action == "occupy") r = tasks_.occupyTable(table);
        else return errorResponse(404, "unknown_action");
        if (r.status != 200) return errorResponse(r.status, r.error);
        return okResponse();
      });

  // ---- POST /api/tasks/3/confirm {"confirm":"LOADED"}
  CROW_ROUTE(app_, "/api/tasks/<int>/confirm").methods("POST"_method)([this](const crow::request& req, int task_id) {
    logLine("HTTP", "POST /api/tasks/" + std::to_string(task_id) + "/confirm " + req.body);
    crow::json::rvalue body = crow::json::load(req.body);
    if (!body || !body.has("confirm")) return errorResponse(400, "bad_request");
    Confirm c;
    if (!parseConfirm(std::string(body["confirm"].s()), c)) return errorResponse(400, "bad_confirm");
    ApiResult r = tasks_.confirm(task_id, c);
    if (r.status != 200) return errorResponse(r.status, r.error);
    wvalue j;
    j["ok"] = true;
    return jsonResponse(200, j);
  });

  // ---- POST /api/robots/pinky1/control {"command":"PAUSE"}  (pinky1 대신 all 가능)
  CROW_ROUTE(app_, "/api/robots/<string>/control")
      .methods("POST"_method)([this](const crow::request& req, const std::string& robot_id) {
        logLine("HTTP", "POST /api/robots/" + robot_id + "/control " + req.body);
        crow::json::rvalue body = crow::json::load(req.body);
        if (!body || !body.has("command")) return errorResponse(400, "bad_request");
        ControlCmd cmd;
        if (!parseControlCmd(std::string(body["command"].s()), cmd)) return errorResponse(400, "bad_command");
        ApiResult r = tasks_.control(robot_id, cmd);
        if (r.status != 200) return errorResponse(r.status, r.error);
        wvalue j;
        j["ok"] = true;
        return jsonResponse(200, j);
      });

  // ---- GET /api/map
  CROW_ROUTE(app_, "/api/map")([this]() {
    MapView m = tasks_.mapView();
    wvalue j;
    j["board"]["x"] = cfg_.board_x;
    j["board"]["y"] = cfg_.board_y;
    j["board"]["w"] = m.board_w;
    j["board"]["h"] = m.board_h;
    wvalue::list nodes;
    for (const MapNodeView& n : m.nodes) {
      wvalue o;
      o["id"] = n.id;
      o["type"] = n.type;
      o["stop_kind"] = n.stop_kind;
      o["table"] = n.table;
      o["x"] = n.x;
      o["y"] = n.y;
      nodes.push_back(std::move(o));
    }
    j["nodes"] = std::move(nodes);
    wvalue::list edges;
    for (const MapEdgeView& e : m.edges) {
      wvalue o;
      o["from"] = e.from;
      o["to"] = e.to;
      edges.push_back(std::move(o));
    }
    j["edges"] = std::move(edges);
    return jsonResponse(200, j);
  });

  // ---- GET /api/history?from=&to=&type=&format=csv
  CROW_ROUTE(app_, "/api/history")([this](const crow::request& req) {
    std::string from = req.url_params.get("from") ? req.url_params.get("from") : "";
    std::string to = req.url_params.get("to") ? req.url_params.get("to") : "";
    std::string type = req.url_params.get("type") ? req.url_params.get("type") : "";
    std::string format = req.url_params.get("format") ? req.url_params.get("format") : "";
    std::vector<HistoryRow> rows = tasks_.history(from, to, type);  // 최신이 앞

    if (format == "csv") {
      std::string csv = "\xEF\xBB\xBFtime,type,message\n";  // 엑셀 한글 깨짐 방지 BOM
      for (auto it = rows.rbegin(); it != rows.rend(); ++it)   // 파일은 오래된 것부터
        csv += it->time + "," + it->type + "," + csvField(it->message) + "\n";
      crow::response res(csv);
      res.set_header("Content-Type", "text/csv; charset=utf-8");
      res.set_header("Content-Disposition", "attachment; filename=\"pinkytorang_history.csv\"");
      return res;
    }
    wvalue j;
    wvalue::list arr;
    for (const HistoryRow& h : rows) {
      wvalue o;
      o["time"] = h.time;
      o["type"] = h.type;
      o["message"] = h.message;
      arr.push_back(std::move(o));
    }
    j["items"] = std::move(arr);
    return jsonResponse(200, j);
  });

  // ---- WebSocket: 매장 UI
  CROW_WEBSOCKET_ROUTE(app_, "/ws/store")
      .onopen([this](Conn& c) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        store_conns_.insert(&c);
        logLine("WS", "매장 UI 연결");
        if (!last_snapshot_.empty()) c.send_text(last_snapshot_);       // 지금 상태 바로 보냄
        for (auto& pair : open_confirms_) c.send_text(pair.second.json);  // 떠 있어야 할 버튼
      })
      .onclose([this](Conn& c, const std::string&, uint16_t) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        store_conns_.erase(&c);
        logLine("WS", "매장 UI 연결 끊김");
      })
      .onmessage([](Conn&, const std::string&, bool) {});  // UI -> 서버는 HTTP만 쓴다

  // ---- WebSocket: 고객 UI (/ws/customer?table=T3)
  CROW_WEBSOCKET_ROUTE(app_, "/ws/customer")
      .onaccept([](const crow::request& req, void** userdata) {
        const char* table = req.url_params.get("table");
        if (!table) return false;
        *userdata = new std::string(table);  // 이 연결이 어느 테이블인지 기억
        return true;
      })
      .onopen([this](Conn& c) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::string* table = static_cast<std::string*>(c.userdata());
        if (!table) return;
        customer_conns_[&c] = *table;
        logLine("WS", "고객 UI 연결: " + *table);
        for (auto& pair : open_confirms_) {
          if (pair.second.guest && pair.second.table == *table) c.send_text(pair.second.json);
        }
      })
      .onclose([this](Conn& c, const std::string&, uint16_t) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        customer_conns_.erase(&c);
        delete static_cast<std::string*>(c.userdata());
        c.userdata(nullptr);
      })
      .onmessage([](Conn&, const std::string&, bool) {});
}

// ---------------------------------------------------------------- 보내기 (mutex_ 잡은 상태에서)

void ClientLinkManager::sendToStore(const std::string& text) {
  for (Conn* c : store_conns_) c->send_text(text);
}

void ClientLinkManager::sendToTable(const std::string& table, const std::string& text) {
  for (auto& pair : customer_conns_) {
    if (pair.second == table) pair.first->send_text(text);
  }
}

// ---------------------------------------------------------------- IClientOut

void ClientLinkManager::pushSnapshot(const SnapshotView& snap) {
  wvalue j;
  j["type"] = "snapshot";
  wvalue::list robots;
  for (const RobotView& r : snap.robots) {
    wvalue o;
    o["id"] = r.id;
    o["x"] = r.x;
    o["y"] = r.y;
    o["yaw"] = r.yaw;
    o["state"] = robotStateName(r.state);
    o["last_node"] = r.last_node;
    o["battery"] = r.battery;
    if (r.task_id >= 0) o["task_id"] = r.task_id;
    else o["task_id"] = nullptr;
    o["online"] = r.online;
    o["charging"] = r.charging;
    robots.push_back(std::move(o));
  }
  j["robots"] = std::move(robots);
  wvalue::list tasks;
  for (const TaskView& t : snap.tasks) tasks.push_back(taskJson(t));
  j["tasks"] = std::move(tasks);
  wvalue::list locks;
  for (const LockView& l : snap.locks) {
    wvalue o;
    o["node"] = l.node;
    o["robot_id"] = l.robot_id;
    locks.push_back(std::move(o));
  }
  j["locks"] = std::move(locks);
  wvalue::list tables;
  for (const TableView& t : snap.tables) tables.push_back(tableJson(t));
  j["tables"] = std::move(tables);

  std::string text = j.dump();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  last_snapshot_ = text;
  sendToStore(text);
}

void ClientLinkManager::pushTaskUpdate(const TaskView& t) {
  wvalue j;
  j["type"] = "task_update";
  j["task_id"] = t.task_id;
  j["kind"] = toString(t.kind);
  j["stage"] = toString(t.stage);
  j["table"] = t.table;
  if (t.order_id >= 0) j["order_id"] = t.order_id;
  else j["order_id"] = nullptr;
  std::string text = j.dump();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  sendToTable(t.table, text);
}

void ClientLinkManager::pushConfirmNeeded(const ConfirmView& c) {
  wvalue j;
  j["type"] = "confirm_needed";
  j["task_id"] = c.task_id;
  j["confirm"] = toString(c.confirm);
  j["countdown_sec"] = c.countdown_sec;
  j["table"] = c.table;
  j["robot_id"] = c.robot_id;
  std::string text = j.dump();
  bool guest = isGuestConfirm(c.confirm);

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  OpenConfirm oc;
  oc.json = text;
  oc.table = c.table;
  oc.guest = guest;
  open_confirms_[c.task_id] = oc;
  sendToStore(text);                     // 매장은 전부 본다
  if (guest) sendToTable(c.table, text);  // 손님 버튼은 그 테이블 휴대폰에도
}

void ClientLinkManager::pushConfirmClosed(const ConfirmView& c, const std::string& by) {
  wvalue j;
  j["type"] = "confirm_closed";
  j["task_id"] = c.task_id;
  j["by"] = by;
  std::string text = j.dump();

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  open_confirms_.erase(c.task_id);
  sendToStore(text);
  if (isGuestConfirm(c.confirm)) sendToTable(c.table, text);
}

void ClientLinkManager::pushAlert(const std::string& level, const std::string& robot_id, const std::string& message) {
  wvalue j;
  j["type"] = "alert";
  j["level"] = level;
  j["robot_id"] = robot_id;
  j["message"] = message;
  std::string text = j.dump();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  sendToStore(text);
}

}  // namespace pinky
