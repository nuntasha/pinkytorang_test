// 핑키토랑 고객 UI
// 규칙 (작업 지시서 5장)
// - 판단하지 않는다. 버튼은 서버의 confirm_needed가 왔을 때만 보인다.
// - 카운트다운은 표시용. 0이 돼도 아무것도 보내지 않는다.
// - 버튼은 응답이 올 때까지 비활성화, 409면 조용히 닫는다.
// - 테이블 ID는 주소에서 읽은 문자열 그대로 쓴다.

const TABLE = new URLSearchParams(location.search).get('table');

const STAGES = {
  SERVE: {
    title: (t) => `주문 #${t.order_id ?? t.task_id}`,
    steps: ['조리 중', '준비 완료', '배달 중', '완료'],
    step: { COOKING: 0, WAITING: 1, ASSIGNED: 1, TO_PICKUP: 1, AT_PICKUP: 1, TO_TABLE: 2, AT_TABLE: 2, TO_DOCK: 3, DONE: 3 },
    text: {
      COOKING: '주방에서 조리하고 있어요', WAITING: '음식이 준비됐어요', ASSIGNED: '음식이 준비됐어요',
      TO_PICKUP: '로봇이 음식을 가지러 가요', AT_PICKUP: '로봇에 음식을 싣고 있어요',
      TO_TABLE: '로봇이 배달 중이에요', AT_TABLE: '도착했어요', TO_DOCK: '배달 완료', DONE: '배달 완료',
    },
  },
};
const ENDED = ['DONE', 'FAILED', 'CANCELED'];
const BAD_TEXT = { FAILED: '문제가 생겼어요. 직원을 불러주세요', CANCELED: '취소됐어요' };

const CONFIRM_TEXT = {
  RECEIVED: { title: '음식이 도착했어요', sub: '음식을 가져가신 뒤 눌러주세요', btn: '수령 완료' },
  SEATED: { title: '자리에 도착했어요', sub: '자리에 앉으신 뒤 눌러주세요', btn: '착석 완료' },
};

const state = { menu: [], cart: {}, tasks: {}, confirm: null };
const $ = (id) => document.getElementById(id);

// ---------- 시작 ----------
if (!TABLE) {
  $('noTable').hidden = false;
} else {
  $('app').hidden = false;
  $('tableChip').textContent = TABLE;
  document.title = `핑키토랑 · ${TABLE}`;
  init();
}

async function init() {
  document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => showView(b.dataset.view)));
  $('orderBtn').addEventListener('click', placeOrder);
  $('sheetBtn').addEventListener('click', pressConfirm);

  await loadMenu();
  await loadOrders();
  openSocket(`/ws/customer?table=${encodeURIComponent(TABLE)}`, onMessage, (on) => {
    $('conn').className = 'conn ' + (on ? 'on' : 'off');
    $('conn').textContent = on ? '연결됨' : '다시 연결 중';
    if (on) loadOrders();
  });
}

function showView(name) {
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.view === name));
  $('view-menu').hidden = name !== 'menu';
  $('view-orders').hidden = name !== 'orders';
  renderCart();
}

// ---------- 메뉴 · 장바구니 ----------
async function loadMenu() {
  const r = await api('GET', '/api/menu');
  if (!r.ok) { toast('메뉴를 불러오지 못했어요'); return; }
  state.menu = Array.isArray(r.data) ? r.data : (r.data.menu || []);
  renderMenu();
}

function renderMenu() {
  $('menuList').innerHTML = state.menu.map((m) => {
    const q = state.cart[m.id] || 0;
    return `
      <li class="menu-item ${q ? '' : 'zero'}" data-id="${esc(m.id)}">
        <div class="menu-thumb">${esc(String(m.name || '').slice(0, 1))}</div>
        <div class="menu-body">
          <div class="menu-name">${esc(m.name)}</div>
          ${m.desc ? `<div class="menu-desc">${esc(m.desc)}</div>` : ''}
          <div class="menu-price num">${won(m.price)}</div>
        </div>
        <div class="stepper">
          <button class="minus" aria-label="빼기">−</button>
          <span class="qty num">${q}</span>
          <button class="plus" aria-label="담기">+</button>
        </div>
      </li>`;
  }).join('') || '<li class="empty">메뉴가 없어요</li>';

  $('menuList').querySelectorAll('.menu-item').forEach((li) => {
    const id = li.dataset.id;
    li.querySelector('.plus').onclick = () => changeQty(id, +1);
    li.querySelector('.minus').onclick = () => changeQty(id, -1);
  });
  renderCart();
}

function changeQty(id, d) {
  const q = Math.max(0, Math.min(20, (state.cart[id] || 0) + d));
  if (q) state.cart[id] = q; else delete state.cart[id];
  renderMenu();
}

function renderCart() {
  const ids = Object.keys(state.cart);
  const count = ids.reduce((s, id) => s + state.cart[id], 0);
  const total = ids.reduce((s, id) => {
    const m = state.menu.find((x) => String(x.id) === id);
    return s + (m ? m.price * state.cart[id] : 0);
  }, 0);
  $('cartCount').textContent = count;
  $('cartTotal').textContent = won(total);
  $('cartBar').hidden = count === 0 || $('view-menu').hidden;
}

async function placeOrder() {
  const items = Object.keys(state.cart).map((id) => {
    const m = state.menu.find((x) => String(x.id) === id);
    return { menu_id: m ? m.id : id, qty: state.cart[id] };
  });
  if (!items.length) return;
  $('orderBtn').disabled = true;
  const r = await api('POST', '/api/orders', { table: TABLE, items });
  $('orderBtn').disabled = false;
  if (!r.ok) { toast(r.status === 0 ? '서버에 연결할 수 없어요' : '주문을 넣지 못했어요'); return; }
  state.cart = {};
  renderMenu();
  toast('주문을 넣었어요');
  await loadOrders();
  showView('orders');
}

// ---------- 내 주문 ----------
async function loadOrders() {
  const r = await api('GET', `/api/orders?table=${encodeURIComponent(TABLE)}`);
  if (!r.ok) return;
  const list = Array.isArray(r.data) ? r.data : (r.data.tasks || []);
  state.tasks = {};
  list.forEach((t) => { state.tasks[t.task_id] = t; });
  renderOrders();
}

function renderOrders() {
  const list = Object.values(state.tasks).sort((a, b) => b.task_id - a.task_id);
  const active = list.filter((t) => !ENDED.includes(t.stage)
    && ((STAGES[t.kind] || STAGES.SERVE).step[t.stage] ?? 0) < 3).length;   // 손님 입장에서 끝난 것은 빼고
  $('activeCount').hidden = active === 0;
  $('activeCount').textContent = active;

  $('orderList').innerHTML = list.map((t) => {
    const def = STAGES[t.kind] || STAGES.SERVE;
    const bad = BAD_TEXT[t.stage];
    const idx = def.step[t.stage] ?? -1;
    const items = (t.items || []).map((i) => `${esc(i.name ?? i.menu_id)} ${i.qty}`).join(', ');
    const time = t.created_at ? new Date(t.created_at).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit' }) : '';
    return `
      <li class="order ${ENDED.includes(t.stage) || idx === 3 ? 'done' : ''}">
        <div class="order-head">
          <span class="order-title">${esc(def.title(t))}</span>
          <span class="order-time">${esc(time)}</span>
        </div>
        ${items ? `<div class="order-items">${items}</div>` : ''}
        <div class="order-status ${bad ? 'bad' : t.stage === 'AT_TABLE' ? 'arrived' : ''}">${esc(bad || def.text[t.stage] || t.stage)}</div>
        ${bad ? '' : `
        <div class="steps">${def.steps.map((_, i) => `<div class="step ${i <= idx ? 'on' : ''}"></div>`).join('')}</div>
        <div class="step-labels">${def.steps.map((s, i) => `<span class="${i === idx ? 'on' : ''}">${s}</span>`).join('')}</div>`}
        ${t.stage === 'COOKING' ? `<button class="btn btn-ghost cancel-btn" data-task="${esc(t.task_id)}">주문 취소</button>` : ''}
      </li>`;
  }).join('') || '<li class="empty">아직 주문이 없어요</li>';
  $('orderList').querySelectorAll('.cancel-btn').forEach((b) => { b.onclick = () => cancelOrder(b); });
}

// 조리 중일 때만 취소할 수 있다 (조리가 끝나면 서버가 409)
async function cancelOrder(btn) {
  if (!confirm('주문을 취소할까요?')) return;
  btn.disabled = true;
  const r = await api('POST', `/api/tasks/${encodeURIComponent(btn.dataset.task)}/cancel`);
  if (r.ok) { toast('주문을 취소했어요'); loadOrders(); }
  else if (r.status === 409) { toast('조리가 끝나서 취소할 수 없어요'); loadOrders(); }
  else { btn.disabled = false; toast(r.status === 0 ? '서버에 연결할 수 없어요' : '취소하지 못했어요'); }
}

// ---------- 서버 알림 ----------
function onMessage(msg) {
  switch (msg.type) {
    case 'task_update':
      if (state.tasks[msg.task_id]) {
        Object.assign(state.tasks[msg.task_id], msg);
        renderOrders();
      } else {
        loadOrders();
      }
      break;
    case 'confirm_needed':
      openSheet(msg);
      break;
    case 'confirm_closed':
      if (state.confirm && state.confirm.task_id === msg.task_id) closeSheet();
      break;
  }
}

// ---------- 도착 시트 ----------
const RING_LEN = 2 * Math.PI * 52;
let countdownTimer = null;

function openSheet(msg) {
  const text = CONFIRM_TEXT[msg.confirm];
  if (!text) return;                      // 고객 화면에 없는 확인 종류는 무시
  state.confirm = { task_id: msg.task_id, confirm: msg.confirm, left: msg.countdown_sec || 0 };
  $('sheetTitle').textContent = text.title;
  $('sheetSub').textContent = text.sub;
  $('sheetBtn').textContent = text.btn;
  $('sheetBtn').disabled = false;
  $('ringFg').style.strokeDasharray = RING_LEN;
  $('sheet').hidden = false;

  clearInterval(countdownTimer);
  const total = state.confirm.left;
  const ring = document.querySelector('.ring');
  if (total > 0) {
    ring.classList.remove('nocount');
    $('ringUnit').hidden = false;
    const draw = () => {
      $('ringNum').textContent = state.confirm.left;
      $('ringFg').style.strokeDashoffset = RING_LEN * (1 - state.confirm.left / total);
    };
    draw();
    countdownTimer = setInterval(() => {
      state.confirm.left -= 1;
      if (state.confirm.left <= 0) { closeSheet(); return; }   // 표시만 끝냄. 서버에는 아무것도 보내지 않음
      draw();
    }, 1000);
  } else {
    ring.classList.add('nocount');
    $('ringNum').textContent = '도착';
    $('ringUnit').hidden = true;
    $('ringFg').style.strokeDashoffset = 0;
  }
}

function closeSheet() {
  clearInterval(countdownTimer);
  state.confirm = null;
  $('sheet').hidden = true;
}

async function pressConfirm() {
  if (!state.confirm) return;
  const { task_id, confirm } = state.confirm;
  $('sheetBtn').disabled = true;
  const r = await api('POST', `/api/tasks/${encodeURIComponent(task_id)}/confirm`, { confirm });
  if (r.ok) { closeSheet(); toast('확인했어요. 감사합니다'); }
  else if (r.status === 409) closeSheet();
  else { $('sheetBtn').disabled = false; toast('다시 눌러주세요'); }
}
