// 핑키토랑 매장관리 UI
// 규칙
// - 판단하지 않는다. 화면은 마지막으로 받은 snapshot과 confirm_needed를 그대로 그린다.
//   (취소 가능 여부처럼 버튼을 보여 줄지만 단계 이름으로 정하고, 최종 판단은 서버가 409로 한다)
// - 버튼은 응답이 올 때까지 비활성화, 409면 조용히 닫는다.
// - 로봇·테이블·노드 ID는 문자열 그대로 표시만 한다.

const ROBOT_COLORS = { pinky1: '#e5487f', pinky2: '#3b82f6', pinky3: '#14b8a6' };   // 맵·상태판 공통
const ROBOT_STATE = { IDLE: '대기', MOVING: '이동 중', PAUSED: '일시정지', BLOCKED: '막힘', ERROR: '오류' };
const STAGE_TEXT = {
  COOKING: '조리 중', WAITING: '대기', ASSIGNED: '배정됨', TO_PICKUP: '픽업 창구로 이동', AT_PICKUP: '픽업 창구 도착',
  TO_TABLE: '테이블로 이동', AT_TABLE: '테이블 도착', TO_DOCK: '도킹 복귀', DONE: '완료', FAILED: '실패', CANCELED: '취소',
};
const KIND_TEXT = { SERVE: '서빙', GUIDE: '안내' };
const TABLE_STATE = { FREE: '빈자리', RESERVED: '안내 중', OCCUPIED: '사용 중' };
const ENDED = ['DONE', 'FAILED', 'CANCELED'];

// 주방이 누르는 확인 (나머지는 손님 화면 버튼이라 여기서는 표시만)
const KITCHEN_CONFIRM = { LOADED: { kind: '픽업 창구 · 음식 적재', btn: '적재 완료' } };
const GUEST_CONFIRM = { RECEIVED: '손님 수령 대기', SEATED: '손님 착석 대기' };

const state = { robots: [], tasks: [], locks: [], tables: [], confirms: {}, alerts: [] };
const $ = (id) => document.getElementById(id);
const robotColor = (id) => ROBOT_COLORS[id] || '#a1a1a6';

// 같은 내용이면 다시 그리지 않는다 (5 Hz snapshot에 버튼이 계속 바뀌면 깜빡이고 클릭이 씹힘)
const lastHtml = {};
function setHtml(id, html) {
  if (lastHtml[id] === html) return false;
  lastHtml[id] = html;
  $(id).innerHTML = html;
  return true;
}

// 취소 버튼을 보여 줄 단계 (서버 규칙과 같음: 서빙은 조리 중, 안내는 착석 전)
function cancellable(t) {
  if (!t) return false;
  if (t.kind === 'SERVE') return t.stage === 'COOKING';
  return ['WAITING', 'ASSIGNED', 'TO_TABLE'].includes(t.stage);
}

// ---------- 시작 ----------
$('stopAllBtn').addEventListener('click', () => control('all', 'PAUSE', $('stopAllBtn')));
$('historyBtn').addEventListener('click', () => { $('historyModal').hidden = false; updateCsvLink(); });
$('historyClose').addEventListener('click', () => { $('historyModal').hidden = true; });
$('historyModal').addEventListener('click', (e) => { if (e.target.id === 'historyModal') $('historyModal').hidden = true; });
$('historyForm').addEventListener('submit', (e) => { e.preventDefault(); searchHistory(); });
['hFrom', 'hTo', 'hType'].forEach((id) => $(id).addEventListener('change', updateCsvLink));

// 버튼은 한 곳에서 처리 (다시 그려도 핸들러가 사라지지 않게)
document.addEventListener('click', (e) => {
  const b = e.target.closest('button[data-act]');
  if (!b || b.disabled) return;
  const act = b.dataset.act;
  if (act === 'confirm') pressConfirm(b);
  else if (act === 'ready') post(b, `/api/tasks/${b.dataset.task}/ready`, '조리 완료');
  else if (act === 'cancel') {
    if (confirm(`작업 #${b.dataset.task}을(를) 취소할까요?`)) post(b, `/api/tasks/${b.dataset.task}/cancel`, '취소했어요', '취소할 수 없는 단계예요');
  } else if (act === 'clear') post(b, `/api/tables/${encodeURIComponent(b.dataset.table)}/clear`, `${b.dataset.table} 빈자리`);
  else if (act === 'occupy') post(b, `/api/tables/${encodeURIComponent(b.dataset.table)}/occupy`, `${b.dataset.table} 사용 중`);
  else if (act === 'robot') {
    if (b.dataset.cmd === 'CANCEL' && !confirm(`${b.dataset.robot}의 작업을 취소할까요?`)) return;
    control(b.dataset.robot, b.dataset.cmd, b);
  }
});

setInterval(tick, 1000);
tick();

openSocket('/ws/store', onMessage, (on) => {
  $('conn').className = 'conn ' + (on ? 'on' : 'off');
  $('conn').textContent = on ? '서버 연결됨' : '서버 끊김 · 다시 연결 중';
  if (!on) { state.confirms = {}; renderConfirms(); }   // 다시 연결되면 서버가 현재 상태를 다시 보내준다
});

function tick() {
  $('clock').textContent = new Date().toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  let changed = false;
  Object.values(state.confirms).forEach((c) => {     // 손님 카운트다운 표시용
    if (c.left > 0) { c.left -= 1; changed = true; }
  });
  if (changed) renderTasks();
}

// ---------- 서버 알림 ----------
function onMessage(msg) {
  switch (msg.type) {
    case 'snapshot':
      state.robots = msg.robots || [];
      state.tasks = msg.tasks || [];
      state.locks = msg.locks || [];
      state.tables = msg.tables || [];
      renderRobots();
      renderTasks();
      renderTables();
      renderConfirms();
      break;
    case 'confirm_needed':
      state.confirms[msg.task_id] = { ...msg, left: msg.countdown_sec || 0 };
      renderConfirms();
      renderTasks();
      break;
    case 'confirm_closed':
      delete state.confirms[msg.task_id];
      renderConfirms();
      renderTasks();
      break;
    case 'alert':
      pushAlert(msg);
      break;
  }
}

// ---------- 확인 필요 ----------
function taskById(id) { return state.tasks.find((t) => t.task_id === id); }
function itemsText(t) {
  if (!t) return '';
  if (t.kind === 'GUIDE') return '';
  return t.items ? t.items.map((i) => `${i.name ?? i.menu_id} ${i.qty}`).join(', ') : '';
}

function renderConfirms() {
  const list = Object.values(state.confirms).filter((c) => KITCHEN_CONFIRM[c.confirm]);
  $('confirmCount').textContent = list.length || '';
  setHtml('confirms', list.map((c) => {
    const def = KITCHEN_CONFIRM[c.confirm];
    const t = taskById(c.task_id) || {};
    const table = c.table ?? t.table ?? '';
    return `
      <div class="confirm ${esc(c.confirm)}">
        <div class="confirm-kind">${def.kind}</div>
        <div class="confirm-table">${esc(table || `작업 #${c.task_id}`)}</div>
        <div class="confirm-items">${esc(itemsText(t))}</div>
        ${t.robot_id ? `<div class="confirm-robot"><span class="dot" style="background:${robotColor(t.robot_id)}"></span>${esc(t.robot_id)}</div>` : ''}
        <button class="btn btn-primary" data-act="confirm" data-task="${esc(c.task_id)}" data-confirm="${esc(c.confirm)}">${def.btn}</button>
      </div>`;
  }).join(''));
}

async function pressConfirm(btn) {
  const taskId = btn.dataset.task;
  btn.disabled = true;
  const r = await api('POST', `/api/tasks/${encodeURIComponent(taskId)}/confirm`, { confirm: btn.dataset.confirm });
  if (r.ok || r.status === 409) {
    delete state.confirms[taskId];      // 409 = 이미 처리됨, 조용히 닫는다
    renderConfirms();
  } else {
    btn.disabled = false;
    toast(r.status === 0 ? '서버에 연결할 수 없어요' : '처리하지 못했어요. 다시 눌러주세요');
  }
}

// 버튼 하나로 POST 보내기 (409면 bad 문구)
async function post(btn, path, okText, badText) {
  btn.disabled = true;
  const r = await api('POST', path);
  if (r.ok) toast(okText);
  else if (r.status === 409) toast(badText || '이미 처리됐어요');
  else toast(r.status === 0 ? '서버에 연결할 수 없어요' : '처리하지 못했어요');
  btn.disabled = false;
  delete lastHtml.colCook; delete lastHtml.colWait; delete lastHtml.colRun; delete lastHtml.tables;  // 다음 snapshot에 새로 그림
}

// ---------- 주문·작업판 ----------
function renderTasks() {
  const cook = state.tasks.filter((t) => t.stage === 'COOKING');
  const wait = state.tasks.filter((t) => t.stage === 'WAITING');
  const done = state.tasks.filter((t) => ENDED.includes(t.stage));
  const run = state.tasks.filter((t) => !['COOKING', 'WAITING'].includes(t.stage) && !ENDED.includes(t.stage));
  [['colCook', 'cntCook', cook], ['colWait', 'cntWait', wait], ['colRun', 'cntRun', run], ['colDone', 'cntDone', done]]
    .forEach(([col, cnt, list]) => {
      $(cnt).textContent = list.length || '';
      setHtml(col, list.sort((a, b) => (col === 'colDone' ? b.task_id - a.task_id : a.task_id - b.task_id)).map(card).join(''));
    });
}

function card(t) {
  const c = state.confirms[t.task_id];
  const guestWait = c && GUEST_CONFIRM[c.confirm];
  const bad = t.stage === 'FAILED' || t.stage === 'CANCELED';
  const tableText = t.table || '';
  return `
    <div class="card">
      <div class="card-top">
        <span class="card-table">${esc(tableText)}</span>
        <span class="kind ${esc(t.kind)}">${KIND_TEXT[t.kind] || esc(t.kind)}</span>
        <span class="card-id num">#${esc(t.task_id)}</span>
      </div>
      ${itemsText(t) ? `<div class="card-items">${esc(itemsText(t))}</div>` : ''}
      <div class="card-foot">
        <span class="stage ${bad ? 'bad' : ''}">${STAGE_TEXT[t.stage] || esc(t.stage)}</span>
        ${guestWait ? `<span class="wait-badge num">${guestWait}${c.left > 0 ? ` · ${c.left}초` : ''}</span>` : ''}
        ${t.robot_id ? `<span class="robot-tag"><span class="dot" style="background:${robotColor(t.robot_id)}"></span>${esc(t.robot_id)}</span>` : ''}
      </div>
      ${t.stage === 'COOKING' || cancellable(t) ? `
      <div class="card-actions">
        ${t.stage === 'COOKING' ? `<button class="btn btn-primary" data-act="ready" data-task="${esc(t.task_id)}">조리 완료</button>` : ''}
        ${cancellable(t) ? `<button class="btn btn-ghost" data-act="cancel" data-task="${esc(t.task_id)}">취소</button>` : ''}
      </div>` : ''}
    </div>`;
}

// ---------- 테이블 ----------
function renderTables() {
  setHtml('tables', state.tables.map((tb) => `
    <div class="table-cell ${esc(tb.state)}">
      <div class="table-top"><span class="table-name">${esc(tb.id)}</span></div>
      <div class="table-state">${TABLE_STATE[tb.state] || esc(tb.state)}</div>
      ${tb.state === 'OCCUPIED' ? `<button class="btn btn-dark" data-act="clear" data-table="${esc(tb.id)}">정리 완료</button>` : ''}
      ${tb.state === 'FREE' ? `<button class="btn btn-ghost" data-act="occupy" data-table="${esc(tb.id)}">착석 처리</button>` : ''}
    </div>`).join('') || '<div class="empty">테이블 정보가 없어요</div>');
}

// ---------- 로봇 상태판 ----------
function renderRobots() {
  setHtml('robots', state.robots.map((r) => {
    const online = r.online !== false;
    const st = online ? r.state : 'OFFLINE';
    const t = r.task_id != null ? taskById(r.task_id) : null;
    const batt = Math.round(r.battery ?? 0);
    return `
      <div class="robot ${online ? '' : 'offline'}" style="--rc:${robotColor(r.id)}">
        <div class="robot-head">
          <span class="robot-name">${esc(r.id)}</span>
          <span class="badge ${esc(st)}">${online ? (ROBOT_STATE[r.state] || esc(r.state)) : '통신 끊김'}</span>
          ${r.charging ? '<span class="badge CHARGING">충전 중</span>' : ''}
        </div>
        <dl class="robot-meta">
          <dt>작업</dt><dd>${t ? `${esc(t.table || '')} ${KIND_TEXT[t.kind] || ''} · ${STAGE_TEXT[t.stage] || esc(t.stage)}` : (r.task_id != null ? `#${esc(r.task_id)}` : '없음')}</dd>
          <dt>마지막 노드</dt><dd class="num">${esc(r.last_node || '-')}</dd>
          <dt>배터리</dt><dd class="batt"><span class="batt-bar"><i class="${batt < 30 ? 'low' : ''}" style="width:${Math.max(0, Math.min(100, batt))}%"></i></span><span class="num">${batt}%</span></dd>
        </dl>
        <div class="robot-actions">
          ${r.state === 'PAUSED'
            ? `<button class="btn btn-ghost" data-act="robot" data-robot="${esc(r.id)}" data-cmd="RESUME">재개</button>`
            : `<button class="btn btn-ghost" data-act="robot" data-robot="${esc(r.id)}" data-cmd="PAUSE">일시정지</button>`}
          <button class="btn btn-ghost" data-act="robot" data-robot="${esc(r.id)}" data-cmd="CANCEL" ${cancellable(t) ? '' : 'disabled'}
            title="안내는 착석 전까지만 취소할 수 있어요. 조리가 끝난 서빙은 취소할 수 없어요">작업 취소</button>
        </div>
      </div>`;
  }).join('') || '<div class="empty">로봇 정보가 아직 없어요</div>');
}

async function control(robotId, command, btn) {
  btn.disabled = true;
  const r = await api('POST', `/api/robots/${encodeURIComponent(robotId)}/control`, { command });
  btn.disabled = false;
  if (r.ok) toast(robotId === 'all' ? '전체 정지를 보냈어요' : `${robotId} · ${command}`);
  else if (r.status === 409) toast('지금은 취소할 수 없는 단계예요');
  else toast(r.status === 0 ? '서버에 연결할 수 없어요' : '명령을 보내지 못했어요');
  delete lastHtml.robots;
}

// ---------- 알림 ----------
function pushAlert(a) {
  const id = Date.now() + Math.random();
  state.alerts.unshift({ ...a, id, time: new Date() });
  state.alerts = state.alerts.slice(0, 5);
  renderAlerts();
  if (a.level === 'INFO') setTimeout(() => dismissAlert(id), 6000);
}
function dismissAlert(id) {
  state.alerts = state.alerts.filter((a) => a.id !== id);
  renderAlerts();
}
function renderAlerts() {
  $('alerts').innerHTML = state.alerts.map((a) => `
    <div class="alert ${esc(a.level)}">
      ${a.robot_id ? `<b>${esc(a.robot_id)}</b>` : ''}<span>${esc(a.message)}</span>
      <span class="time num">${a.time.toLocaleTimeString('ko-KR')}</span>
      <button data-id="${a.id}" aria-label="닫기">✕</button>
    </div>`).join('');
  $('alerts').querySelectorAll('button').forEach((b) => { b.onclick = () => dismissAlert(Number(b.dataset.id)); });
}

// ---------- 이력 ----------
function historyQuery() {
  const p = new URLSearchParams();
  if ($('hFrom').value) p.set('from', new Date($('hFrom').value).toISOString());
  if ($('hTo').value) p.set('to', new Date($('hTo').value).toISOString());
  if ($('hType').value) p.set('type', $('hType').value);
  return p;
}
function updateCsvLink() {
  const p = historyQuery();
  p.set('format', 'csv');
  $('csvLink').href = '/api/history?' + p.toString();
}
async function searchHistory() {
  updateCsvLink();
  const r = await api('GET', '/api/history?' + historyQuery().toString());
  if (!r.ok) { toast('이력을 불러오지 못했어요'); return; }
  const rows = Array.isArray(r.data) ? r.data : (r.data.items || []);
  $('historyRows').innerHTML = rows.map((h) => `
    <tr><td class="num">${esc(h.time ? new Date(h.time).toLocaleString('ko-KR') : '')}</td>
        <td>${esc(h.type)}</td><td>${esc(h.message ?? '')}</td></tr>`).join('')
    || '<tr><td colspan="3" class="empty">결과가 없어요</td></tr>';
}
