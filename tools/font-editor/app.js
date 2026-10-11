/* Font editor UI. Font logic lives in font-core.js; this file wires it to the page. */

'use strict';

const F = window.FontCore;
const STORE_KEY = 'cubevox-font-editor-v1';
const MAX_UNDO = 200;
const DELETE_ARM_MS = 3000;
const SAVE_DEBOUNCE_MS = 400;
const POLL_MS = 2000;
const LIBRARY_URL = '/api/library';
const LIBRARY_PATH = 'fonts/cubevox-fonts.json';

// Served by serve.js the library lives in the repo file; opened as a file it lives in localStorage.
const SERVED = location.protocol.startsWith('http');

const $ = (id) => document.getElementById(id);

const state = {
  fonts: [],
  current: 0,
  code: 65,
  clipboard: null,
  undo: [],
  preview: 'CUBEVOX v0.01 Reverb 42%',
};

const font = () => state.fonts[state.current];
const glyph = () => font().glyphs[state.code];

// ── theme values for canvases ───────────────────────────────────

const css = getComputedStyle(document.documentElement);
const token = (name) => css.getPropertyValue(name).trim();
const tokenNum = (name) => Number.parseFloat(token(name));

const ink = {
  off: token('--oled-off'),
  on: token('--oled-on'),
  grid: token('--grid-line'),
};

// Sizes the backing store for the screen's pixel ratio and returns a context in CSS pixels.
function sizeCanvas(canvas, w, h) {
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  canvas.style.width = `${w}px`;
  canvas.style.height = `${h}px`;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.imageSmoothingEnabled = false;
  return ctx;
}

function drawGlyph(ctx, rows, x0, y0, scale) {
  ctx.fillStyle = ink.on;
  rows.forEach((row, y) => {
    for (let x = 0; x < row.length; x++) {
      if (row[x] === '#') ctx.fillRect(x0 + x * scale, y0 + y * scale, scale, scale);
    }
  });
}

// ── persistence and status ──────────────────────────────────────

// version is the file's last known version; null means the library never loaded.
const sync = { version: null, dirty: false, edits: 0, saving: false, timer: null, pollFailed: false };

function persist() {
  if (SERVED) return scheduleSave();
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(F.toLibrary(state.fonts)));
  } catch (err) {
    console.error('[font-editor] autosave failed', err);
    setStatus('Autosave failed. Use Save library to keep your work.', true);
  }
}

function restore() {
  const raw = localStorage.getItem(STORE_KEY);
  if (!raw) return false;
  try {
    const result = F.parseLibrary(JSON.parse(raw));
    if (result.error || !result.fonts.length) return false;
    state.fonts = result.fonts;
    return true;
  } catch (err) {
    console.error('[font-editor] stored library unreadable', err);
    return false;
  }
}

function setStatus(message, isError = false) {
  const el = $('status');
  el.textContent = message;
  el.classList.toggle('error', isError);
}

// ── served library ──────────────────────────────────────────────

async function requestJson(url, options) {
  const res = await fetch(url, options);
  const data = await res.json().catch(() => ({}));
  if (!res.ok && res.status !== 409) throw new Error(data.error || `${url} returned ${res.status}`);
  return { status: res.status, data };
}

async function fetchLibrary() {
  const { data } = await requestJson(LIBRARY_URL);
  const result = F.parseLibrary(data.library);
  if (result.error || !result.fonts.length) throw new Error(result.error || 'The library has no fonts.');
  return { version: data.version, fonts: result.fonts };
}

function scheduleSave() {
  if (sync.version === null) {
    setStatus('The library did not load, so edits are not saved. Start serve.js and reload.', true);
    return;
  }
  sync.dirty = true;
  sync.edits += 1;
  clearTimeout(sync.timer);
  sync.timer = setTimeout(save, SAVE_DEBOUNCE_MS);
}

async function save() {
  sync.timer = null;
  if (sync.saving) {
    sync.timer = setTimeout(save, SAVE_DEBOUNCE_MS);
    return;
  }
  sync.saving = true;
  const sent = sync.edits;
  const body = JSON.stringify(F.toLibrary(state.fonts), null, 1);
  try {
    const { status, data } = await requestJson(LIBRARY_URL, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json', 'If-Match': sync.version },
      body,
    });
    if (status === 409) {
      await resolveConflict(body);
      return;
    }
    sync.version = data.version;
    if (sync.edits === sent) sync.dirty = false;
    setStatus(`Saved to ${LIBRARY_PATH}`);
  } catch (err) {
    console.error('[font-editor] save failed', err);
    setStatus(`Save failed: ${err.message}`, true);
  } finally {
    sync.saving = false;
  }
}

// Replaces the fonts with the disk copy and keeps the selected font (by name) and glyph.
async function reloadFromDisk() {
  const { version, fonts } = await fetchLibrary();
  const name = font().name;
  const code = state.code;
  state.fonts = fonts;
  state.current = Math.max(0, fonts.findIndex((f) => f.name === name));
  state.code = font().glyphs[code] ? code : 65;
  state.undo = [];
  sync.version = version;
  sync.dirty = false;
  clearTimeout(sync.timer);
  sync.timer = null;
  renderAll();
}

// Parks the editor's copy in conflict.json, then shows the disk copy.
async function resolveConflict(ownCopy) {
  try {
    await requestJson('/api/conflict', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: ownCopy,
    });
    await reloadFromDisk();
    setStatus('Conflict: the file changed on disk. Your copy is in fonts/conflict.json.', true);
  } catch (err) {
    console.error('[font-editor] conflict handling failed', err);
    setStatus(`Conflict: the file changed on disk and your copy could not be parked: ${err.message}`, true);
  }
}

async function pollOnce() {
  try {
    const { data } = await requestJson(`${LIBRARY_URL}?versionOnly=1`);
    sync.pollFailed = false;
    if (sync.saving || data.version === sync.version) return;
    if (sync.dirty) {
      await resolveConflict(JSON.stringify(F.toLibrary(state.fonts), null, 1));
    } else {
      await reloadFromDisk();
      setStatus('Reloaded: file changed on disk');
    }
  } catch (err) {
    console.error('[font-editor] poll failed', err);
    if (!sync.pollFailed) setStatus(`Lost the editor server: ${err.message}`, true);
    sync.pollFailed = true;
  }
}

async function pollLoop() {
  await pollOnce();
  setTimeout(pollLoop, POLL_MS);
}

async function loadServed() {
  try {
    const { version, fonts } = await fetchLibrary();
    state.fonts = fonts;
    sync.version = version;
  } catch (err) {
    console.error('[font-editor] library load failed', err);
    state.fonts = [F.exampleFont()];
    setStatus(`Could not load ${LIBRARY_PATH}: ${err.message}. Showing the example font; edits are not saved.`, true);
  }
}

async function exportHeaderServed() {
  try {
    await requestJson('/api/header', {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain' },
      body: F.toCHeader(state.fonts),
    });
    setStatus('Wrote fonts/cubevox_fonts.h');
  } catch (err) {
    console.error('[font-editor] header export failed', err);
    setStatus(`Header export failed: ${err.message}`, true);
  }
}

// ── opened as a file ────────────────────────────────────────────

const EDITOR_URL = 'http://127.0.0.1:8765/';

// Hands this browser's copy to the running editor server (fonts/from-browser.json), then opens it.
async function moveToRepoEditor() {
  try {
    const res = await fetch(`${EDITOR_URL}api/import`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(F.toLibrary(state.fonts), null, 1),
    });
    if (!res.ok) throw new Error(`import returned ${res.status}`);
    location.href = EDITOR_URL;
  } catch (err) {
    console.error('[font-editor] move to repo editor failed', err);
    $('fileBannerText').textContent = 'The repo editor is not running. Double-click '
      + 'tools/font-editor/Font Editor.command, then press Move again.';
  }
}

function showFileBanner() {
  $('fileBanner').hidden = false;
  $('moveBtn').addEventListener('click', moveToRepoEditor);
}

function download(filename, text, type) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function uniqueName(base) {
  const taken = new Set(state.fonts.map((f) => f.name));
  if (!taken.has(base)) return base;
  for (let n = 2; ; n++) if (!taken.has(`${base} (${n})`)) return `${base} (${n})`;
}

// ── glyph edits ─────────────────────────────────────────────────

function pushUndo() {
  state.undo.push({ id: font().id, code: state.code, rows: glyph() });
  if (state.undo.length > MAX_UNDO) state.undo.shift();
}

function setGlyph(rows) {
  font().glyphs[state.code] = rows;
  persist();
  drawEditor();
  drawTile(state.code);
  drawPreview();
}

function editGlyph(transform) {
  pushUndo();
  setGlyph(transform(glyph()));
}

function undo() {
  while (state.undo.length) {
    const step = state.undo.pop();
    const index = state.fonts.findIndex((f) => f.id === step.id);
    if (index < 0 || !state.fonts[index].glyphs[step.code]) continue;
    const target = state.fonts[index];
    if (step.rows.length !== target.height || step.rows[0].length !== target.width) continue;
    state.current = index;
    state.code = step.code;
    target.glyphs[step.code] = step.rows;
    persist();
    renderAll();
    return;
  }
  setStatus('Nothing to undo.');
}

const actions = {
  clear: () => editGlyph(F.clear),
  invert: () => editGlyph(F.invert),
  left: () => editGlyph((r) => F.shift(r, -1, 0)),
  right: () => editGlyph((r) => F.shift(r, 1, 0)),
  up: () => editGlyph((r) => F.shift(r, 0, -1)),
  down: () => editGlyph((r) => F.shift(r, 0, 1)),
  copy: () => {
    state.clipboard = glyph();
    setStatus(`Copied ${F.charLabel(font(), state.code)}.`);
  },
  paste: () => {
    if (!state.clipboard) return setStatus('Copy a glyph first.', true);
    editGlyph(() => F.resize(state.clipboard, font().width, font().height));
  },
  undo,
};

// ── editor canvas ───────────────────────────────────────────────

const ZOOM_KEY = 'cubevox-font-editor-zoom';

// zoom is screen px per glyph pixel; null fits the glyph to --editor-px.
const editor = {
  cell: 1,
  painting: false,
  paintOn: true,
  last: null,
  zoom: Number.parseInt(localStorage.getItem(ZOOM_KEY), 10) || null,
};

function drawEditor() {
  const f = font();
  const canvas = $('editorCanvas');
  const fit = Math.max(4, Math.floor(tokenNum('--editor-px') / Math.max(f.width, f.height)));
  const cell = editor.zoom || fit;
  editor.cell = cell;
  $('zoom').value = cell;
  $('zoomValue').textContent = `${cell} px per pixel`;
  const w = f.width * cell + 1;
  const h = f.height * cell + 1;
  const ctx = sizeCanvas(canvas, w, h);

  ctx.fillStyle = ink.off;
  ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = ink.on;
  glyph().forEach((row, y) => {
    for (let x = 0; x < f.width; x++) {
      if (row[x] === '#') ctx.fillRect(x * cell + 1, y * cell + 1, cell - 1, cell - 1);
    }
  });
  ctx.fillStyle = ink.grid;
  for (let x = 0; x <= f.width; x++) ctx.fillRect(x * cell, 0, 1, h);
  for (let y = 0; y <= f.height; y++) ctx.fillRect(0, y * cell, w, 1);

  const label = F.charLabel(f, state.code);
  const shown = state.code >= F.SYMBOL_FIRST || state.code === 32 ? label : `"${label}"`;
  $('editorHeading').textContent =
    `Glyph ${shown}, code ${state.code} (0x${state.code.toString(16).toUpperCase()})`;

  const isSymbol = state.code >= F.SYMBOL_FIRST;
  $('symbolBox').hidden = !isSymbol;
  if (isSymbol) $('symbolName').value = f.symbolNames[state.code] || '';
}

function cellAt(event) {
  const rect = $('editorCanvas').getBoundingClientRect();
  const x = Math.floor((event.clientX - rect.left) / editor.cell);
  const y = Math.floor((event.clientY - rect.top) / editor.cell);
  const f = font();
  return x >= 0 && y >= 0 && x < f.width && y < f.height ? { x, y } : null;
}

// Paints every cell on the line from the last painted cell, so fast drags leave no gaps.
function paintAt(event) {
  const p = cellAt(event);
  if (!p) return;
  const from = editor.last || p;
  const steps = Math.max(Math.abs(p.x - from.x), Math.abs(p.y - from.y));
  let rows = glyph();
  for (let i = 0; i <= steps; i++) {
    const t = steps ? i / steps : 0;
    const x = Math.round(from.x + (p.x - from.x) * t);
    const y = Math.round(from.y + (p.y - from.y) * t);
    rows = F.setPixel(rows, x, y, editor.paintOn);
  }
  editor.last = p;
  if (rows.join() !== glyph().join()) setGlyph(rows);
}

function wireEditor() {
  const canvas = $('editorCanvas');
  $('zoom').addEventListener('input', (e) => {
    editor.zoom = Number.parseInt(e.target.value, 10);
    localStorage.setItem(ZOOM_KEY, String(editor.zoom));
    drawEditor();
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('pointerdown', (e) => {
    const p = cellAt(e);
    if (!p) return;
    const erase = e.button === 2 || e.altKey;
    editor.paintOn = erase ? false : !F.isOn(glyph(), p.x, p.y);
    editor.painting = true;
    editor.last = null;
    canvas.setPointerCapture(e.pointerId);
    pushUndo();
    paintAt(e);
  });
  canvas.addEventListener('pointermove', (e) => { if (editor.painting) paintAt(e); });
  const stop = () => { editor.painting = false; };
  canvas.addEventListener('pointerup', stop);
  canvas.addEventListener('pointercancel', stop);
}

// ── glyph map ───────────────────────────────────────────────────

const tiles = new Map();

function tileScale(f) {
  return Math.max(1, Math.floor(tokenNum('--tile-px') / Math.max(f.width, f.height)));
}

function drawTile(code) {
  const tile = tiles.get(code);
  if (!tile) return;
  const f = font();
  const s = tileScale(f);
  const ctx = sizeCanvas(tile.canvas, f.width * s, f.height * s);
  ctx.fillStyle = ink.off;
  ctx.fillRect(0, 0, f.width * s, f.height * s);
  drawGlyph(ctx, f.glyphs[code], 0, 0, s);
}

function renderMap() {
  const map = $('glyphMap');
  map.replaceChildren();
  tiles.clear();
  for (const code of F.codesOf(font())) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tile';
    button.setAttribute('aria-pressed', String(code === state.code));
    const canvas = document.createElement('canvas');
    const label = document.createElement('span');
    label.className = 'label';
    label.textContent = F.charLabel(font(), code);
    button.setAttribute('aria-label', `Edit ${label.textContent}`);
    button.append(canvas, label);
    button.addEventListener('click', () => selectGlyph(code));
    map.append(button);
    tiles.set(code, { button, canvas, label });
    drawTile(code);
  }
}

function selectGlyph(code) {
  tiles.get(state.code)?.button.setAttribute('aria-pressed', 'false');
  state.code = code;
  tiles.get(code)?.button.setAttribute('aria-pressed', 'true');
  drawEditor();
}

function stepGlyph(delta) {
  const codes = F.codesOf(font());
  const i = codes.indexOf(state.code);
  const next = codes[Math.min(codes.length - 1, Math.max(0, i + delta))];
  selectGlyph(next);
  tiles.get(next)?.button.scrollIntoView({ block: 'nearest' });
}

// ── preview ─────────────────────────────────────────────────────

function drawPreview() {
  const f = font();
  const scale = tokenNum('--preview-scale');
  const W = tokenNum('--oled-w');
  const H = tokenNum('--oled-h');
  const ctx = sizeCanvas($('previewCanvas'), W * scale, H * scale);
  ctx.fillStyle = ink.off;
  ctx.fillRect(0, 0, W * scale, H * scale);

  let x = 0;
  let y = 0;
  for (const ch of state.preview) {
    if (x + f.width > W) { x = 0; y += f.height + 1; }
    if (y + f.height > H) break;
    const rows = f.glyphs[ch.codePointAt(0)];
    if (rows) drawGlyph(ctx, rows, x * scale, y * scale, scale);
    x += f.width + f.spacing;
  }
}

// ── font list and settings ──────────────────────────────────────

function renderFontList() {
  const list = $('fontList');
  list.replaceChildren();
  state.fonts.forEach((f, i) => {
    const li = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.setAttribute('aria-current', String(i === state.current));
    const name = document.createElement('span');
    name.textContent = f.name;
    const size = document.createElement('span');
    size.className = 'size';
    size.textContent = `${f.width} x ${f.height}`;
    button.append(name, size);
    button.addEventListener('click', () => selectFont(i));
    li.append(button);
    list.append(li);
  });
}

function renderSettings() {
  const f = font();
  $('fontName').value = f.name;
  $('fontName').removeAttribute('aria-invalid');
  $('nameError').textContent = '';
  $('fontWidth').value = f.width;
  $('fontHeight').value = f.height;
  $('fontSpacing').value = f.spacing;
  updateSizeHint();
  $('deleteBtn').disabled = state.fonts.length < 2;
  disarmDelete();
  $('newError').textContent = '';
  $('newName').removeAttribute('aria-invalid');
}

function selectFont(index) {
  state.current = index;
  if (!font().glyphs[state.code]) state.code = 65;
  renderAll();
}

function renderAll() {
  renderFontList();
  renderSettings();
  renderMap();
  drawEditor();
  drawPreview();
}

function nameProblem(name, ignoreIndex) {
  if (!name.trim()) return 'Give the font a name.';
  const clash = state.fonts.some((f, i) => i !== ignoreIndex && f.name === name.trim());
  return clash ? 'Another font already has this name.' : '';
}

function readSize(input) {
  const n = Number.parseInt(input.value, 10);
  return Number.isInteger(n) && n >= 1 && n <= F.MAX_SIZE ? n : null;
}

function updateSizeHint() {
  const f = font();
  const w = readSize($('fontWidth'));
  const h = readSize($('fontHeight'));
  const hint = $('sizeHint');
  hint.classList.remove('warn');
  if (w === null || h === null) {
    hint.textContent = `Width and height run from 1 to ${F.MAX_SIZE} px.`;
    hint.classList.add('warn');
  } else if (w < f.width || h < f.height) {
    hint.textContent = 'Shrinking cuts pixels off the right and bottom of every glyph.';
    hint.classList.add('warn');
  } else {
    hint.textContent = '';
  }
}

let deleteTimer = null;

function disarmDelete() {
  clearTimeout(deleteTimer);
  const btn = $('deleteBtn');
  btn.classList.remove('armed');
  btn.textContent = 'Delete font';
}

function wireSettings() {
  $('fontName').addEventListener('input', (e) => {
    const problem = nameProblem(e.target.value, state.current);
    $('nameError').textContent = problem;
    e.target.setAttribute('aria-invalid', String(Boolean(problem)));
    if (problem) return;
    font().name = e.target.value.trim();
    persist();
    renderFontList();
  });

  $('fontWidth').addEventListener('input', updateSizeHint);
  $('fontHeight').addEventListener('input', updateSizeHint);
  $('applySize').addEventListener('click', () => {
    const w = readSize($('fontWidth'));
    const h = readSize($('fontHeight'));
    if (w === null || h === null) return updateSizeHint();
    F.resizeFont(font(), w, h);
    state.undo = state.undo.filter((s) => s.id !== font().id);
    persist();
    renderAll();
    setStatus(`${font().name} is now ${w} x ${h} px.`);
  });

  $('fontSpacing').addEventListener('input', (e) => {
    const n = Number.parseInt(e.target.value, 10);
    if (!Number.isInteger(n) || n < 0 || n > F.MAX_SPACING) return;
    font().spacing = n;
    persist();
    drawPreview();
  });

  $('duplicateBtn').addEventListener('click', () => {
    const copy = F.parseLibrary(F.toLibrary([font()])).fonts[0];
    copy.name = uniqueName(`${font().name} copy`);
    state.fonts.splice(state.current + 1, 0, copy);
    state.current += 1;
    persist();
    renderAll();
  });

  $('deleteBtn').addEventListener('click', (e) => {
    const btn = e.currentTarget;
    if (!btn.classList.contains('armed')) {
      btn.classList.add('armed');
      btn.textContent = 'Click again to delete';
      deleteTimer = setTimeout(disarmDelete, DELETE_ARM_MS);
      return;
    }
    const name = font().name;
    state.fonts.splice(state.current, 1);
    state.current = Math.min(state.current, state.fonts.length - 1);
    persist();
    renderAll();
    setStatus(`Deleted ${name}.`);
  });

  $('newName').addEventListener('input', () => {
    $('newError').textContent = '';
    $('newName').removeAttribute('aria-invalid');
  });

  $('createBtn').addEventListener('click', () => {
    const name = $('newName').value;
    const w = readSize($('newWidth'));
    const h = readSize($('newHeight'));
    let problem = nameProblem(name, -1);
    if (!problem && (w === null || h === null)) problem = `Width and height run from 1 to ${F.MAX_SIZE} px.`;
    $('newError').textContent = problem;
    $('newName').setAttribute('aria-invalid', String(Boolean(problem)));
    if (problem) return;
    state.fonts.push(F.makeFont(name.trim(), w, h, 1));
    state.current = state.fonts.length - 1;
    state.code = 65;
    $('newName').value = '';
    persist();
    renderAll();
    setStatus(`Created ${name.trim()}.`);
  });

  $('addSymbol').addEventListener('click', () => {
    const code = F.addSymbol(font());
    if (code === null) return setStatus('This font already uses every symbol code up to 255.', true);
    persist();
    state.code = code;
    renderMap();
    drawEditor();
    $('symbolName').focus();
  });

  $('symbolName').addEventListener('input', (e) => {
    const name = e.target.value.trim() || `symbol ${state.code}`;
    font().symbolNames[state.code] = name;
    tiles.get(state.code).label.textContent = name;
    persist();
  });

  $('deleteSymbol').addEventListener('click', () => {
    F.removeSymbol(font(), state.code);
    state.code = 65;
    persist();
    renderMap();
    drawEditor();
  });

  $('previewText').value = state.preview;
  $('previewText').addEventListener('input', (e) => {
    state.preview = e.target.value;
    drawPreview();
  });
}

// ── load, save, export ──────────────────────────────────────────

function wireFiles() {
  $('saveBtn').addEventListener('click', () => {
    download('cubevox-fonts.json', JSON.stringify(F.toLibrary(state.fonts), null, 1), 'application/json');
    setStatus(`Saved ${state.fonts.length} fonts to cubevox-fonts.json.`);
  });

  $('exportBtn').addEventListener('click', () => {
    if (SERVED) return exportHeaderServed();
    download('cubevox_fonts.h', F.toCHeader(state.fonts), 'text/plain');
    setStatus(`Exported ${state.fonts.length} fonts to cubevox_fonts.h.`);
  });

  $('loadBtn').addEventListener('click', () => $('fileInput').click());
  $('fileInput').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    let result;
    try {
      result = F.parseLibrary(JSON.parse(await file.text()));
    } catch (err) {
      console.error('[font-editor] load failed', err);
      result = { error: 'This file is not valid JSON.' };
    }
    if (result.error) return setStatus(`${file.name}: ${result.error}`, true);
    for (const f of result.fonts) {
      f.name = uniqueName(f.name);
      state.fonts.push(f);
    }
    state.current = state.fonts.length - result.fonts.length;
    persist();
    renderAll();
    setStatus(`Loaded ${result.fonts.length} fonts from ${file.name}.`);
  });
}

// ── keyboard ────────────────────────────────────────────────────

function wireKeys() {
  document.addEventListener('keydown', (e) => {
    if (e.target.closest('input')) return;
    const mod = e.metaKey || e.ctrlKey;
    if (mod && e.key === 'z') { e.preventDefault(); undo(); return; }
    if (mod && e.key === 'c') { e.preventDefault(); actions.copy(); return; }
    if (mod && e.key === 'v') { e.preventDefault(); actions.paste(); return; }
    if (e.key === 'ArrowLeft') { e.preventDefault(); stepGlyph(-1); }
    if (e.key === 'ArrowRight') { e.preventDefault(); stepGlyph(1); }
  });
}

// ── start ───────────────────────────────────────────────────────

async function start() {
  if (SERVED) {
    $('saveBtn').textContent = 'Export copy';
    $('loadBtn').textContent = 'Import';
    await loadServed();
  } else {
    if (!restore()) state.fonts = [F.exampleFont()];
    showFileBanner();
  }
  wireAll();
  renderAll();
  if (SERVED) setTimeout(pollLoop, POLL_MS);
}

function wireAll() {
  document.querySelectorAll('[data-action]').forEach((btn) =>
    btn.addEventListener('click', () => actions[btn.dataset.action]()));
  wireEditor();
  wireSettings();
  wireFiles();
  wireKeys();
}

start().catch((err) => {
  console.error('[font-editor] start failed', err);
  setStatus(`Start failed: ${err.message}`, true);
});
