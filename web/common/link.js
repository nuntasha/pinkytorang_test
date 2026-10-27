// 핑키토랑 공통: 서버(Main Service)와의 연결
// - 보내는 것은 전부 HTTP (api)
// - 받는 것은 전부 WebSocket (openSocket). 끊기면 3초 뒤 다시 연결
// - 서버 주소는 페이지를 연 주소와 같다 (Main Service가 web/ 폴더를 그대로 서빙)

async function api(method, path, body) {
  try {
    const res = await fetch(path, {
      method,
      headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    let data = null;
    try { data = await res.json(); } catch (_) { /* 본문 없음 */ }
    return { status: res.status, ok: res.ok, data };
  } catch (e) {
    return { status: 0, ok: false, data: null };   // 서버에 닿지 못함
  }
}

function openSocket(path, onMessage, onState) {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + path;
  let ws = null;
  const connect = () => {
    ws = new WebSocket(url);
    ws.onopen = () => onState && onState(true);
    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (_) { return; }
      onMessage(msg);
    };
    ws.onclose = () => {
      onState && onState(false);
      setTimeout(connect, 3000);
    };
    ws.onerror = () => ws.close();
  };
  connect();
}

let toastTimer = null;
function toast(text) {
  let el = document.querySelector('.toast');
  if (!el) {
    el = document.createElement('div');
    el.className = 'toast';
    document.body.appendChild(el);
  }
  el.textContent = text;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 2200);
}

function won(n) {
  return Number(n || 0).toLocaleString('ko-KR') + '원';
}

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
