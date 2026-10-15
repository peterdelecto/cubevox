// Pure layout rules. No DOM. Mirrors what the box rejects so the page can prevent it.

export const EMPTY = 'empty';
const REVERB_GROUP = 'reverb';
const PERCENT = 100;

export const isEmpty = (id) => !id || id === EMPTY;

export function cardIndex(cards) {
  const byId = new Map();
  for (const c of cards || []) byId.set(c.id, c);
  return byId;
}

// Cards the bank offers: every card except `empty`.
export const bankCards = (cards) => (cards || []).filter((c) => c.id !== EMPTY);

export function normalizeSlots(slots, count) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(isEmpty(slots && slots[i]) ? EMPTY : slots[i]);
  return out;
}

export const sameSlots = (a, b) => a.length === b.length && a.every((id, i) => id === b[i]);

// Id of the reverb in the layout, or null.
export function placedReverb(slots, byId) {
  for (const id of slots) {
    const c = byId.get(id);
    if (c && c.group === REVERB_GROUP) return id;
  }
  return null;
}

// Why a bank card cannot be placed: 'used', or null when it can.
export function bankBlock(slots, byId, id) {
  return slots.includes(id) ? 'used' : null;
}

// Result of dropping `src` on slot `i`, or null when the drop is invalid.
// src is { from: 'bank', id } or { from: 'slot', index }.
// A reverb dropped from the bank replaces the reverb already placed, so the
// layout keeps one reverb without a clearing step.
export function applyDrop(slots, byId, src, i) {
  const next = slots.slice();
  if (src.from === 'slot') {
    if (src.index === i) return next;
    [next[i], next[src.index]] = [next[src.index], next[i]];
    return next;
  }
  const c = byId.get(src.id);
  if (!c || bankBlock(slots, byId, src.id)) return null;
  if (c.group === REVERB_GROUP) {
    const other = placedReverb(slots, byId);
    if (other) next[slots.indexOf(other)] = EMPTY;
  }
  next[i] = src.id;
  return next;
}

export function emptySlot(slots, i) {
  const next = slots.slice();
  next[i] = EMPTY;
  return next;
}

// Meter: the fixed term counts in both sums, each sum is read against its own limit.
export function meter(slots, byId, limits) {
  if (!limits || !limits.mean || !limits.worst) return null;
  let mean = limits.fixed || 0;
  let worst = limits.fixed || 0;
  for (const id of slots) {
    const c = byId.get(id);
    if (!c || !c.cost) continue;
    mean += c.cost.avg;
    worst += c.cost.worst;
  }
  const raw = Math.max(mean / limits.mean, worst / limits.worst) * PERCENT;
  const over = raw > PERCENT;
  const pct = over ? Math.ceil(raw) : Math.round(raw);
  const text = over ? `${pct} %, over the limit` : `${pct} % used, ${PERCENT - pct} % left`;
  return { pct, over, text, bar: Math.min(PERCENT, pct) };
}

// "v2.00.08" stays, "2.00.08" gains a v, other strings (fixture) pass through.
export const vers = (s) => (/^\d/.test(s || '') ? `v${s}` : s || '');
