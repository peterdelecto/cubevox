import './vendor/webawesome/components/button/button.js';
import './vendor/webawesome/components/progress-bar/progress-bar.js';
import './vendor/webawesome/components/badge/badge.js';
import './vendor/webawesome/components/spinner/spinner.js';
import * as M from './model.js';

const POLL_MS = 1000;
const BEAT_MS = 10000;
const IDLE_STEP = 'idle';

const $ = (id) => document.getElementById(id);

const S = {
  state: null,
  slots: null,      // what the page shows; the user's edit while dirty
  dirty: false,     // slots differ from the box, so polls must not overwrite them
  failed: false,    // last push failed; slots that differ from the box get a warn border
  busy: false,      // push in flight
  error: '',
  done: false,
  pick: null,       // keyboard pick: { from: 'bank', id } or { from: 'slot', index }
  drag: null,       // pointer drag in progress, same shape as pick
  pendingRender: false,
  offline: false,
  updateBusy: false,
  lastApp: null,
  boardSig: '',
  barSig: '',
};

/* ── helpers ─────────────────────────────────────────── */

function el(tag, cls, attrs, ...kids) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  for (const [k, v] of Object.entries(attrs || {})) node.setAttribute(k, v);
  for (const kid of kids) if (kid != null) node.append(kid);
  return node;
}

async function request(path, init) {
  const res = await fetch(path, init);
  return res.json();
}

const get = (path) => request(path);
const post = (path, body) => request(path, {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(body || {}),
});

const byId = () => M.cardIndex(S.state ? S.state.cards : []);
const boxSlots = () => M.normalizeSlots(S.state.layout.slots, S.state.layout.slots.length);
const connected = () => Boolean(S.state && S.state.connected);
const announce = (text) => { $('live').textContent = text; };

/* ── state in ────────────────────────────────────────── */

async function poll() {
  try {
    const st = await get('/api/state');
    if (S.lastApp && st.app !== S.lastApp) { location.reload(); return; }
    S.lastApp = st.app;
    S.state = st;
    S.offline = false;
    if (!S.slots || (!S.dirty && !S.busy)) S.slots = boxSlots();
  } catch (err) {
    console.error('state poll failed', err);
    S.offline = true;
  }
  render(false);
}

async function heartbeat() {
  try {
    await post('/api/heartbeat');
  } catch (err) {
    console.error('heartbeat failed', err);
  }
}

/* ── edits ───────────────────────────────────────────── */

function setSlots(next, focusKey) {
  S.slots = next;
  S.dirty = !M.sameSlots(next, boxSlots());
  if (S.failed && !S.dirty) S.failed = false;
  S.error = '';
  S.done = false;
  S.pick = null;
  render(true, focusKey);
}

function dropOn(src, index) {
  const next = M.applyDrop(S.slots, byId(), src, index);
  if (next) setSlots(next, `slot:${index}`);
  return next;
}

const pickName = (p) => {
  const id = p.from === 'bank' ? p.id : S.slots[p.index];
  const c = byId().get(id);
  return c ? c.name : id;
};

function clearPick(text) {
  if (!S.pick) return;
  S.pick = null;
  if (text) announce(text);
  render(true);
}

/* ── push and update ─────────────────────────────────── */

function canPush() {
  const m = M.meter(S.slots, byId(), S.state.limits);
  return connected() && !S.busy && !(m && m.over);
}

async function push() {
  if (!S.state || !canPush()) return;
  S.busy = true;
  S.error = '';
  S.done = false;
  render(true);
  try {
    const r = await post('/api/layout', { slots: S.slots });
    if (r.ok) {
      S.state.layout = r.layout;
      S.slots = M.normalizeSlots(r.layout.slots, r.layout.slots.length);
      S.dirty = false;
      S.failed = false;
      S.done = true;
    } else {
      S.error = r.error || 'The box refused the layout.';
      S.failed = true;
    }
  } catch (err) {
    console.error('push failed', err);
    S.error = 'The app did not answer. The layout was not sent.';
    S.failed = true;
  } finally {
    S.busy = false;
    render(true);
  }
}

async function startUpdate() {
  S.updateBusy = true;
  render(true);
  try {
    await post('/api/update');
  } catch (err) {
    console.error('update request failed', err);
  } finally {
    S.updateBusy = false;
    render(true);
  }
}

/* ── rendering ───────────────────────────────────────── */

function render(force, focusKey) {
  if (!S.state) { renderBar(); return; }
  if (S.drag) { S.pendingRender = true; return; }
  renderBoard(force, focusKey);
  renderBar();
}

function renderBoard(force, focusKey) {
  const cards = S.state.cards;
  const sig = JSON.stringify([
    cards.map((c) => c.id), S.slots, S.pick, S.failed ? boxSlots() : null, S.state.limits,
    connected(), S.busy, S.error, S.done,
  ]);
  if (!force && sig === S.boardSig) return;
  S.boardSig = sig;

  const active = document.activeElement;
  const key = focusKey || (active && active.dataset ? active.dataset.key : null);

  renderSlots();
  renderBank();
  renderMeter();

  if (key) {
    const target = document.querySelector(`[data-key="${key}"]`);
    if (target && target.tabIndex >= 0) target.focus();
  }
}

function cardFace(card) {
  return [
    el('span', 'card-name', null, card.name),
    el('span', 'card-knobs', null, ...(card.knobs || []).map((k) => el('span', 'knob', null, k.name))),
  ];
}

function renderSlots() {
  const map = byId();
  const box = S.failed ? boxSlots() : null;
  const host = $('slots');
  host.replaceChildren();

  S.slots.forEach((id, i) => {
    const empty = M.isEmpty(id);
    const card = empty ? null : map.get(id);
    const cls = ['slot'];
    cls.push(empty ? 'is-empty' : 'is-filled');
    if (S.pick) cls.push('is-armed');
    if (S.pick && S.pick.from === 'slot' && S.pick.index === i) cls.push('is-picked');
    if (box && box[i] !== id) cls.push('is-differs');

    const label = `Slot ${i + 1}, ${card ? card.name : empty ? 'empty' : id}`;
    const slot = el('div', cls.join(' '), {
      role: 'button', tabindex: '0', 'aria-label': label, 'data-key': `slot:${i}`,
    }, el('span', 'slot-num', null, String(i + 1)));
    slot.append(card ? el('span', 'card-face', null, ...cardFace(card)) : el('span', 'slot-empty', null, 'empty'));

    if (!empty) {
      slot.draggable = true;
      slot.addEventListener('dragstart', (e) => startDrag(e, { from: 'slot', index: i }, slot));
      slot.addEventListener('dragend', endDrag);
    }
    slot.addEventListener('dragover', (e) => slotOver(e, slot, i));
    slot.addEventListener('dragleave', (e) => { if (!slot.contains(e.relatedTarget)) clearOver(slot); });
    slot.addEventListener('drop', (e) => { e.preventDefault(); clearOver(slot); if (S.drag) dropOn(S.drag, i); });
    slot.addEventListener('keydown', (e) => slotKey(e, i, empty));
    host.append(slot);
  });
}

function renderBank() {
  const map = byId();
  const host = $('bank');
  host.replaceChildren();
  host.ondragover = (e) => {
    if (!S.drag || S.drag.from !== 'slot') return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';
    host.classList.add('is-over');
  };
  host.ondragleave = (e) => { if (!host.contains(e.relatedTarget)) host.classList.remove('is-over'); };
  host.ondrop = (e) => {
    e.preventDefault();
    host.classList.remove('is-over');
    if (S.drag && S.drag.from === 'slot') setSlots(M.emptySlot(S.slots, S.drag.index));
  };

  for (const c of M.bankCards(S.state.cards)) {
    const block = M.bankBlock(S.slots, map, c.id);
    const cls = ['card'];
    if (block === 'used') cls.push('is-used');
    if (block === 'reverb') cls.push('is-blocked');
    const picked = S.pick && S.pick.from === 'bank' && S.pick.id === c.id;
    if (picked) cls.push('is-picked');

    const attrs = { role: 'button', 'data-key': `bank:${c.id}`, 'aria-label': c.name };
    if (block) {
      attrs['aria-disabled'] = 'true';
      attrs.tabindex = '-1';
      if (block === 'reverb') attrs.title = 'one reverb per layout';
    } else {
      attrs.tabindex = '0';
      if (picked) attrs['aria-pressed'] = 'true';
    }
    const card = el('div', cls.join(' '), attrs, ...cardFace(c));
    if (!block) {
      card.draggable = true;
      card.addEventListener('dragstart', (e) => startDrag(e, { from: 'bank', id: c.id }, card));
      card.addEventListener('dragend', endDrag);
      card.addEventListener('keydown', (e) => bankKey(e, c));
    }
    host.append(card);
  }
}

function renderMeter() {
  const m = M.meter(S.slots, byId(), S.state.limits);
  const bar = $('meter');
  const text = $('meter-text');
  if (m) {
    bar.value = m.bar;
    bar.classList.toggle('is-over', m.over);
    text.textContent = m.text;
    text.classList.toggle('is-over', m.over);
  } else {
    bar.value = 0;
    text.textContent = '';
  }

  const btn = $('push');
  btn.loading = S.busy;
  btn.disabled = !canPush();
  btn.onclick = push;

  const note = $('push-note');
  let kind = '';
  let msg = '';
  if (S.error) { kind = 'is-error'; msg = S.error; }
  else if (S.done) { kind = 'is-ok'; msg = 'Pushed. The box has this layout.'; }
  else if (!connected()) msg = 'Connect the box to push';
  else if (m && m.over) { kind = 'is-error'; msg = 'Over the limit. Remove an effect to push.'; }
  note.className = `support push-note ${kind}`.trim();
  note.setAttribute('role', kind === 'is-error' ? 'alert' : 'status');
  note.textContent = msg;
}

function renderBar() {
  const st = S.state;
  const u = (st && st.update) || {};
  const sig = JSON.stringify([
    Boolean(st), st && st.connected, st && st.fw, st && st.app, u, S.offline, S.updateBusy,
  ]);
  if (sig === S.barSig) return;
  S.barSig = sig;

  const bar = $('bar');
  bar.replaceChildren();
  const left = el('div', 'bar-group');
  const right = el('div', 'bar-group bar-update');

  if (S.offline) {
    left.append(el('span', 'ink-danger', null, 'Lost contact with the app'));
  } else if (!st) {
    left.append(el('span', 'ink-dim', null, 'Starting'));
  } else if (st.connected) {
    const badge = el('wa-badge', null, { variant: 'success' }, 'Box connected');
    left.append(badge, el('span', 'ink-dim', null, `firmware ${M.vers(st.fw)}`));
  } else {
    left.append(el('span', 'ink-dim', null, 'Connect the box'));
  }
  if (st && st.app) left.append(el('span', 'ink-dim', null, `app ${M.vers(st.app)}`));

  const stepping = u.step && u.step !== IDLE_STEP;
  if (u.error) {
    right.append(el('span', 'ink-danger bar-text', { role: 'alert' }, u.error), actionButton('Retry'));
  } else if (stepping) {
    right.append(el('wa-spinner', null, { 'aria-label': 'Updating' }), el('span', 'ink-dim bar-text', null, u.step));
  } else if (u.available) {
    right.append(actionButton(`Update to ${M.vers(u.version)}`));
  }

  bar.append(left, right);
}

function actionButton(label) {
  const b = el('wa-button', null, { variant: 'brand', appearance: 'accent' }, label);
  b.loading = S.updateBusy;
  b.addEventListener('click', startUpdate);
  return b;
}

/* ── pointer drag ────────────────────────────────────── */

function startDrag(e, src, node) {
  S.drag = src;
  S.pick = null;
  e.dataTransfer.effectAllowed = 'move';
  e.dataTransfer.setData('text/plain', src.from === 'bank' ? src.id : String(src.index));
  requestAnimationFrame(() => node.classList.add('is-dragging'));
}

function endDrag() {
  S.drag = null;
  for (const n of document.querySelectorAll('.is-dragging, .is-over, .is-invalid')) {
    n.classList.remove('is-dragging', 'is-over', 'is-invalid');
  }
  if (S.pendingRender) { S.pendingRender = false; render(true); }
}

function slotOver(e, slot, i) {
  if (!S.drag) return;
  if (M.applyDrop(S.slots, byId(), S.drag, i)) {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';
    slot.classList.add('is-over');
    slot.classList.remove('is-invalid');
  } else {
    e.dataTransfer.dropEffect = 'none';
    slot.classList.add('is-invalid');
    slot.classList.remove('is-over');
  }
}

const clearOver = (slot) => slot.classList.remove('is-over', 'is-invalid');

/* ── keyboard ────────────────────────────────────────── */

function bankKey(e, card) {
  if (e.key !== 'Enter' && e.key !== ' ') return;
  e.preventDefault();
  if (S.pick && S.pick.from === 'bank' && S.pick.id === card.id) {
    clearPick(`${card.name} put down`);
    return;
  }
  S.pick = { from: 'bank', id: card.id };
  announce(`${card.name} picked up. Press Enter on a slot to place it, Escape to cancel.`);
  render(true, `bank:${card.id}`);
}

function slotKey(e, i, empty) {
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault();
    if (S.pick) {
      const name = pickName(S.pick);
      if (dropOn(S.pick, i)) announce(`${name} placed in slot ${i + 1}`);
      else announce(`${name} cannot go in slot ${i + 1}`);
    } else if (!empty) {
      S.pick = { from: 'slot', index: i };
      announce(`${pickName(S.pick)} picked up. Enter on another slot swaps, Escape cancels.`);
      render(true, `slot:${i}`);
    }
  } else if ((e.key === 'Backspace' || e.key === 'Delete') && !empty) {
    e.preventDefault();
    announce(`Slot ${i + 1} emptied`);
    setSlots(M.emptySlot(S.slots, i), `slot:${i}`);
  }
}

document.addEventListener('keydown', (e) => {
  if (e.key !== 'Escape') return;
  if (S.drag) return;  // the browser cancels a pointer drag itself
  clearPick('Cancelled');
});

/* ── start ───────────────────────────────────────────── */

async function loop() {
  await poll();
  setTimeout(loop, POLL_MS);
}

renderBar();
heartbeat();
setInterval(heartbeat, BEAT_MS);
loop();
