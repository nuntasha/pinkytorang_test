// 핑키토랑 입구 키오스크
// - 인원을 고르면 POST /api/guide. 서버가 인원에 맞는 빈 테이블을 고르고 입구 로봇이 안내한다
// - 빈 테이블이 없으면 대기열. 2초마다 GET /api/guide/<id> 로 순번 · 테이블을 물어본다
// - 판단하지 않는다. 화면은 서버가 준 table / queue_pos / stage 를 그대로 보여 준다

const $ = (id) => document.getElementById(id);
const state = { party: 2, maxParty: 6, taskId: null, poll: null, resetTimer: null };
const ENDED = ['DONE', 'FAILED', 'CANCELED'];

$('minusBtn').onclick = () => setParty(state.party - 1);
$('plusBtn').onclick = () => setParty(state.party + 1);
$('guideBtn').onclick = requestGuide;
$('homeBtn').onclick = reset;
$('cancelBtn').onclick = cancelWait;

openSocket('/ws/store', () => {}, (on) => {     // 연결 표시만 (데이터는 HTTP로 물어봄)
  $('conn').className = 'conn ' + (on ? 'on' : 'off');
  $('conn').textContent = on ? '' : '서버 연결 중';
});

loadMaxParty();
setParty(2);

async function loadMaxParty() {
  const r = await api('GET', '/api/tables');
  if (!r.ok) return;
  const seats = (r.data.tables || []).map((t) => t.seats);
  if (seats.length) state.maxParty = Math.max(...seats);
  setParty(state.party);
}

function setParty(n) {
  state.party = Math.max(1, Math.min(state.maxParty, n));
  $('partyNum').textContent = state.party;
  $('minusBtn').disabled = state.party <= 1;
  $('plusBtn').disabled = state.party >= state.maxParty;
  $('partyHint').textContent = state.party >= state.maxParty ? `최대 ${state.maxParty}명까지 한 테이블에 앉을 수 있어요` : '';
}

async function requestGuide() {
  $('guideBtn').disabled = true;
  const r = await api('POST', '/api/guide', { party: state.party });
  $('guideBtn').disabled = false;
  if (!r.ok) {
    if (r.data && r.data.error === 'party_too_large') toast(`최대 ${state.maxParty}명까지 가능해요. 직원을 불러 주세요`);
    else toast(r.status === 0 ? '서버에 연결할 수 없어요' : '요청하지 못했어요. 직원을 불러 주세요');
    return;
  }
  state.taskId = r.data.task_id;
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

  if (ENDED.includes(g.stage) || g.stage === 'AT_TABLE' || g.stage === 'TO_DOCK') {   // 안내가 끝났거나 끝나 감
    if (g.stage === 'CANCELED' || g.stage === 'FAILED') {
      badge.className = 'result-badge wait';
      badge.textContent = '!';
      $('resultTitle').textContent = g.stage === 'CANCELED' ? '안내를 취소했어요' : '안내하지 못했어요';
      $('resultSub').textContent = '직원을 불러 주세요';
    }
    $('cancelBtn').hidden = true;
    stopPolling();
    autoReset(6);
    return;
  }

  if (!g.table) {                                     // 만석 -> 대기
    badge.className = 'result-badge wait';
    badge.textContent = `${g.queue_pos}번`;
    $('resultTitle').textContent = '지금은 자리가 없어요';
    $('resultSub').textContent = '자리가 나면 이 화면에 알려 드릴게요. 잠시만 기다려 주세요';
    $('cancelBtn').hidden = false;
    return;
  }

  badge.className = 'result-badge';
  badge.textContent = g.table;
  $('cancelBtn').hidden = true;
  if (g.stage === 'TO_TABLE') {
    $('resultTitle').textContent = `${g.table} 자리로 안내할게요`;
    $('resultSub').textContent = '로봇을 따라가 주세요';
    stopPolling();       // 다음 손님이 쓸 수 있게 잠시 뒤 처음 화면으로
    autoReset(8);
  } else {                                            // 테이블은 정해졌고 입구 로봇을 기다림
    $('resultTitle').textContent = `${g.table} 자리가 준비됐어요`;
    $('resultSub').textContent = '안내 로봇이 곧 출발해요';
  }
}

async function cancelWait() {
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
  $('resultView').hidden = true;
  $('pickView').hidden = false;
  setParty(2);
}
