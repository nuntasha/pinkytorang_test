// 핑키토랑 입구 키오스크
// - 좌석 배치도를 보여 주고, 손님이 빈 테이블을 고르면 POST /api/guide {table}
//   서버가 그 테이블을 예약하고 입구 로봇이 안내한다
// - 테이블 상태(빈자리 / 사용 중)는 매장 snapshot으로 실시간으로 바뀐다
// - 판단하지 않는다. 빈자리인지는 서버가 최종 확인한다 (이미 찼으면 409)

const $ = (id) => document.getElementById(id);
const state = { board: { w: 2.28, h: 1.82 }, entrance: null, tables: [], selected: null, taskId: null, poll: null, resetTimer: null };
const ENDED = ['DONE', 'FAILED', 'CANCELED'];
const MIN_W = 104, MIN_H = 72;   // 손가락으로 누를 수 있는 최소 크기 (px)

$('guideBtn').onclick = requestGuide;
$('homeBtn').onclick = reset;
$('cancelBtn').onclick = cancelGuide;
window.addEventListener('resize', renderFloor);

// 매장 snapshot을 같이 받아서 테이블 상태를 바로 반영 (연결 표시도 겸함)
openSocket('/ws/store', (msg) => {
  if (msg.type !== 'snapshot' || !msg.tables) return;
  mergeTables(msg.tables);
}, (on) => {
  $('conn').className = 'conn ' + (on ? 'on' : 'off');
  $('conn').textContent = on ? '' : '서버 연결 중';
  if (on) loadLayout();
});

loadLayout();

async function loadLayout() {
  const r = await api('GET', '/api/tables');
  if (!r.ok) return;
  if (r.data.board) state.board = r.data.board;
  state.entrance = r.data.entrance || null;
  state.tables = r.data.tables || [];
  renderFloor();
}

function mergeTables(list) {
  const byId = Object.fromEntries(list.map((t) => [t.id, t]));
  let changed = false;
  state.tables.forEach((t) => {
    const n = byId[t.id];
    if (n && n.state !== t.state) { t.state = n.state; changed = true; }
  });
  if (!changed) return;
  const sel = state.tables.find((t) => t.id === state.selected);
  if (sel && sel.state !== 'FREE' && !$('pickView').hidden) {
    state.selected = null;
    toast('고르신 자리가 방금 찼어요. 다른 자리를 골라 주세요');
  }
  renderFloor();
}

// 맵 좌표(m, 원점 왼쪽 아래)를 화면(px, 원점 왼쪽 위)으로
function renderFloor() {
  const wrap = document.querySelector('.floor-wrap');
  const pad = 8;
  if (!wrap.clientWidth) return;   // 안내 화면일 때는 안 그림
  const scale = Math.min((wrap.clientWidth - pad * 2) / state.board.w, (wrap.clientHeight - pad * 2) / state.board.h);
  const W = state.board.w * scale, H = state.board.h * scale;
  const floor = $('floor');
  floor.style.width = W + 'px';
  floor.style.height = H + 'px';
  const px = (x) => x * scale, py = (y) => H - y * scale;

  let html = state.tables.map((t) => {
    const w = Math.max(MIN_W, (t.w || 0.165) * scale), h = Math.max(MIN_H, (t.h || 0.11) * scale);
    const free = t.state === 'FREE';
    return `<button class="tbl ${t.id === state.selected ? 'selected' : ''}" data-id="${esc(t.id)}" ${free ? '' : 'disabled'}
      style="left:${px(t.x)}px; top:${py(t.y)}px; width:${w}px; height:${h}px">${esc(t.id)}<small>${free ? '빈자리' : '사용 중'}</small></button>`;
  }).join('');
  if (state.entrance) html += `<span class="here" style="left:${Math.max(70, px(state.entrance.x))}px; top:${py(state.entrance.y)}px">지금 여기 (입구)</span>`;
  floor.innerHTML = html;
  floor.querySelectorAll('.tbl').forEach((b) => { b.onclick = () => selectTable(b.dataset.id); });

  const sel = state.selected;
  $('guideBtn').disabled = !sel;
  $('guideBtn').textContent = sel ? `${sel} 자리로 안내 받기` : '자리를 골라 주세요';
  const anyFree = state.tables.some((t) => t.state === 'FREE');
  $('pickSub').textContent = anyFree ? '빈자리를 누르면 로봇이 안내해 드려요' : '지금은 빈자리가 없어요. 잠시만 기다려 주세요';
}

function selectTable(id) {
  state.selected = state.selected === id ? null : id;
  renderFloor();
}

async function requestGuide() {
  if (!state.selected) return;
  $('guideBtn').disabled = true;
  const r = await api('POST', '/api/guide', { table: state.selected });
  if (!r.ok) {
    $('guideBtn').disabled = false;
    if (r.status === 409) { toast('방금 다른 손님이 고른 자리예요. 다른 자리를 골라 주세요'); state.selected = null; loadLayout(); }
    else toast(r.status === 0 ? '서버에 연결할 수 없어요' : '요청하지 못했어요. 직원을 불러 주세요');
    return;
  }
  state.taskId = r.data.task_id;
  state.selected = null;   // 이제 이 손님 자리는 예약됨 (snapshot에서 '찼다'고 알리지 않게)
  showStatus(r.data);
  clearInterval(state.poll);
  state.poll = setInterval(poll, 2000);
}

async function poll() {
  if (state.taskId == null) return;
  const r = await api('GET', `/api/guide/${state.taskId}`);
  if (r.ok) showStatus(r.data);
}

function showStatus(g) {
  $('pickView').hidden = true;
  $('resultView').hidden = false;
  const badge = $('resultBadge');
  badge.className = 'result-badge';
  badge.textContent = g.table;

  if (g.stage === 'CANCELED' || g.stage === 'FAILED') {
    badge.className = 'result-badge warn';
    badge.textContent = '!';
    $('resultTitle').textContent = g.stage === 'CANCELED' ? '안내를 취소했어요' : '안내하지 못했어요';
    $('resultSub').textContent = '직원을 불러 주세요';
    $('cancelBtn').hidden = true;
    stopPolling();
    autoReset(6);
    return;
  }
  if (g.stage === 'TO_TABLE' || g.stage === 'AT_TABLE' || ENDED.includes(g.stage) || g.stage === 'TO_DOCK') {
    $('resultTitle').textContent = `${g.table} 자리로 안내할게요`;
    $('resultSub').textContent = '로봇을 따라가 주세요';
    $('cancelBtn').hidden = true;
    stopPolling();          // 다음 손님이 쓸 수 있게 잠시 뒤 처음 화면으로
    autoReset(8);
    return;
  }
  // WAITING / ASSIGNED: 입구 로봇을 기다리는 중
  $('resultTitle').textContent = `${g.table} 자리를 잡아 뒀어요`;
  $('resultSub').textContent = '안내 로봇이 곧 출발해요. 잠시만 기다려 주세요';
  $('cancelBtn').hidden = false;
}

async function cancelGuide() {
  if (state.taskId == null) return;
  $('cancelBtn').disabled = true;
  const r = await api('POST', `/api/tasks/${state.taskId}/cancel`);
  $('cancelBtn').disabled = false;
  if (r.ok || r.status === 409) reset();
  else toast('취소하지 못했어요');
}

function stopPolling() { clearInterval(state.poll); state.poll = null; }

function autoReset(sec) {
  clearTimeout(state.resetTimer);
  state.resetTimer = setTimeout(reset, sec * 1000);
}

function reset() {
  stopPolling();
  clearTimeout(state.resetTimer);
  state.taskId = null;
  state.selected = null;
  $('resultView').hidden = true;
  $('pickView').hidden = false;
  loadLayout();
}
