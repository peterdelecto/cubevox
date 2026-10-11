#!/usr/bin/env node
/* Font proof sheets and lint report for a cubevox font library.
   Usage: node proof.js <library.json> --font "<name>" [--compare "<name>"]
          [--compare-lib <other.json>] [--out <dir>] [--pairs] [--stem 1|2|3|4] [--set digits|all]
          [--recipe] [--recipe-gate]
          [--screens "<5x7>,<9x12>,<16x24>"] [--trace "<9x12 source>"]
   --pairs exits 1 when a required pair is too close (blur or bloom); the bars scale with cell area.
   --recipe lints corners, inner chamfers and diagonals for stems of 3 px or more; --recipe-gate exits 1 on any finding.
   --stem sets the expected stroke width for the STEM flag and thin lint. --set digits limits the closest-pairs scan.
   --trace checks that each glyph is the source font's glyph scaled up and writes trace.png; exit 1 if any differs by more than 2 px.
   --screens writes screens.png (knob, menu and slot mockups, one font per column).
   Writes grid.png, strings.png, oled-N.png (+ oled-compare-N.png) and lint.txt. */

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const core = require('./font-core.js');

const GRID_SCALE = 10;
const GRID_PER_ROW = 16;
const STRING_SCALE = 6;
const OLED_W = 128;
const OLED_H = 64;
const OLED_SCALE = 4;
const LINE_GAP = 2;
const MIN_STEM_RUN = 4;
const CLOSER_BLUR = 15;
const PAIR_MIN_BLUR = 12;
const PAIR_MIN_BLOOM = 10;
const PAIR_REF_AREA = 9 * 12;

// At 1 px strokes the blur smears every stroke over three columns, so pairs gate on raw
// pixel difference instead: 16 % of the cell, the share the 9 x 12 blur bar implies.
const PAIR_MIN_RAW_SHARE = 0.16;
const DIGITS_SET = '0123456789-+.:%MSDBHZTK';
const SCREEN_GAP = 4;
const TRACE_SCALE = 4;
const TRACE_MAX_DIFF = 2;

// Source col/row -> target cols/rows for a 9x12 glyph scaled to 16x24; the 8-col map is for glyphs with no ink in col 8.
const TRACE_COLS_8 = [[0, 1], [2], [3, 4, 5], [6, 7], [8], [9, 10, 11], [12], [13, 14], [15]];
const TRACE_COLS_9 = [[0, 1], [2], [3, 4], [5, 6], [7], [8, 9], [10, 11], [12], [13, 14]];
const TRACE_ROWS = [[0, 1], [2], [3, 4], [5, 6, 7], [8, 9], [10], [11, 12], [13, 14, 15], [16, 17], [18, 19, 20], [21], [22, 23]];
const THIN_IGNORE = '\'",.';
const CLOSEST_COUNT = 15;

const TEST_STRINGS = [
  'HOHOHO OHO',
  'HAMBURGEFONSTIV',
  'THE QUICK BROWN FOX',
  'JUMPS OVER 0123456789',
  '0O Q8B 5S 2Z 1I G6 UV MN EF',
  'THRESHOLD  DECAY',
  'SEMITONES  RESPONSE',
  'SLAPBACK  DISTORTION',
  'UNISON  REVERB  MIX',
  '!"#&\'(),./:',
  '-12  +5  250 MS',
  'THRESHOLD DECAY',
  'SEMITONES -12',
  'DISTORTION',
  '1I! 0OQD 8B3 5S 2Z 17',
];

const REQUIRED_PAIRS = ('0O 0D 0Q 0C OQ OD DQ CG OC 8B 83 86 80 89 BR BE PR PF EF 5S 56 2Z 17 1I 1L 1! I! IL 7T ' +
  'MN HN MW NW HM UV VY KX XY 3B 69 G6 GC').split(' ');
const SYMMETRIC = 'AHMOTUVWXY08';
const CAPS_DIGITS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';

const COLORS = {
  bg: [22, 22, 26],
  darkPixel: [40, 40, 48],
  gridLine: [14, 14, 18],
  outline: [64, 64, 76],
  lit: [236, 236, 240],
  litCompare: [255, 176, 64],
  faintBox: [78, 78, 92],
  targetOnly: [96, 160, 255],
  oledLit: [170, 255, 255],
  oledBg: [0, 0, 0],
};

function fail(msg) {
  console.error(`proof: ${msg}`);
  process.exit(1);
}

// ── arguments ────────────────────────────────────────────────

function parseArgs(argv) {
  const args = { lib: null, font: null, compare: null, compareLib: null, out: null, pairs: false, stem: 2, set: 'all', screens: null, trace: null, recipe: false, recipeGate: false };
  const flags = { '--font': 'font', '--compare': 'compare', '--compare-lib': 'compareLib', '--out': 'out', '--stem': 'stem', '--set': 'set', '--screens': 'screens', '--trace': 'trace' };
  for (let i = 0; i < argv.length; i++) {
    const key = flags[argv[i]];
    if (argv[i] === '--pairs') {
      args.pairs = true;
    } else if (argv[i] === '--recipe') {
      args.recipe = true;
    } else if (argv[i] === '--recipe-gate') {
      args.recipe = true;
      args.recipeGate = true;
    } else if (key) {
      if (i + 1 >= argv.length) fail(`${argv[i]} needs a value`);
      args[key] = argv[++i];
      if (key === 'stem') args.stem = Number(args.stem);
    } else if (argv[i].startsWith('--')) {
      fail(`unknown option ${argv[i]}`);
    } else if (!args.lib) {
      args.lib = argv[i];
    } else {
      fail(`unexpected argument ${argv[i]}`);
    }
  }
  if (![1, 2, 3, 4].includes(args.stem)) fail('--stem must be 1, 2, 3 or 4');
  if (args.recipe && args.stem < 3) fail('--recipe needs --stem 3 or 4');
  if (!['digits', 'all'].includes(args.set)) fail('--set must be digits or all');
  return args;
}

function loadFonts(file) {
  let obj;
  try {
    obj = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (err) {
    fail(`cannot read ${file}: ${err.message}`);
  }
  const lib = core.parseLibrary(obj);
  if (lib.error) fail(`${file}: ${lib.error}`);
  return lib.fonts;
}

function pickFont(fonts, name, file) {
  const font = fonts.find((f) => f.name === name);
  if (!font) fail(`no font named "${name}" in ${file}; have: ${fonts.map((f) => `"${f.name}"`).join(', ')}`);
  return font;
}

// ── PNG encoder ──────────────────────────────────────────────

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buf) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'latin1');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

function encodePng(width, height, rgb) {
  const stride = width * 3;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    rgb.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

class Canvas {
  constructor(width, height, color) {
    this.width = width;
    this.height = height;
    this.buf = Buffer.alloc(width * height * 3);
    this.fill(0, 0, width, height, color);
  }

  // Clipped solid rectangle.
  fill(x, y, w, h, color) {
    const x0 = Math.max(0, x);
    const y0 = Math.max(0, y);
    const x1 = Math.min(this.width, x + w);
    const y1 = Math.min(this.height, y + h);
    for (let yy = y0; yy < y1; yy++) {
      for (let xx = x0; xx < x1; xx++) {
        const i = (yy * this.width + xx) * 3;
        this.buf[i] = color[0];
        this.buf[i + 1] = color[1];
        this.buf[i + 2] = color[2];
      }
    }
  }

  // 1 px rectangle outline.
  outline(x, y, w, h, color) {
    this.fill(x, y, w, 1, color);
    this.fill(x, y + h - 1, w, 1, color);
    this.fill(x, y, 1, h, color);
    this.fill(x + w - 1, y, 1, h, color);
  }

  save(file) {
    fs.writeFileSync(file, encodePng(this.width, this.height, this.buf));
  }
}

// ── glyph helpers ────────────────────────────────────────────

const hasInk = (rows) => rows.some((r) => r.includes('#'));
const lit = (rows, x, y) => y >= 0 && y < rows.length && x >= 0 && x < rows[y].length && rows[y][x] === '#';
const codeOf = (ch) => ch.charCodeAt(0);

function glyphOf(font, code) {
  return font.glyphs[code] || null;
}

// Ink bounds, or null for a blank glyph.
function bounds(rows) {
  let top = Infinity, bottom = -1, left = Infinity, right = -1;
  rows.forEach((row, y) => {
    for (let x = 0; x < row.length; x++) {
      if (row[x] !== '#') continue;
      top = Math.min(top, y);
      bottom = Math.max(bottom, y);
      left = Math.min(left, x);
      right = Math.max(right, x);
    }
  });
  return bottom < 0 ? null : { top, bottom, left, right };
}

function mode(values, fallback = null) {
  if (!values.length) return fallback;
  const counts = new Map();
  for (const v of values) counts.set(v, (counts.get(v) || 0) + 1);
  let best = null;
  for (const [v, n] of counts) {
    if (best === null || n > counts.get(best) || (n === counts.get(best) && v < best)) best = v;
  }
  return best;
}

function histogram(values) {
  const counts = new Map();
  for (const v of values) counts.set(v, (counts.get(v) || 0) + 1);
  return [...counts.entries()].sort((a, b) => a[0] - b[0]).map(([v, n]) => `${v}x${n}`).join(' ');
}

function runsOf(line) {
  const runs = [];
  let n = 0;
  for (const c of line) {
    if (c === '#') n++;
    else if (n) { runs.push(n); n = 0; }
  }
  if (n) runs.push(n);
  return runs;
}

const columnOf = (rows, x) => rows.map((r) => r[x]).join('');

// Rows of column x that sit in a vertical run of at least MIN_STEM_RUN.
function longRunRows(rows, x) {
  const col = columnOf(rows, x);
  const set = new Set();
  let start = -1;
  for (let y = 0; y <= col.length; y++) {
    const on = y < col.length && col[y] === '#';
    if (on && start < 0) start = y;
    if (!on && start >= 0) {
      if (y - start >= MIN_STEM_RUN) for (let k = start; k < y; k++) set.add(k);
      start = -1;
    }
  }
  return set;
}

// Widths of stems: groups of adjacent columns that share a long vertical run.
function stemWidths(rows) {
  const w = rows.length ? rows[0].length : 0;
  const widths = [];
  let prev = null;
  let width = 0;
  for (let x = 0; x <= w; x++) {
    const cur = x < w ? longRunRows(rows, x) : new Set();
    let shared = 0;
    if (prev) for (const y of cur) if (prev.has(y)) shared++;
    if (cur.size && prev && prev.size && shared >= MIN_STEM_RUN) {
      width++;
    } else {
      if (width) widths.push(width);
      width = cur.size ? 1 : 0;
    }
    prev = cur;
  }
  return widths;
}

function loneAndCheckers(rows) {
  let lone = 0;
  let checker = 0;
  for (let y = 0; y < rows.length; y++) {
    for (let x = 0; x < rows[y].length; x++) {
      if (lit(rows, x, y) &&
          !lit(rows, x - 1, y) && !lit(rows, x + 1, y) && !lit(rows, x, y - 1) && !lit(rows, x, y + 1)) lone++;
      const a = lit(rows, x, y), b = lit(rows, x + 1, y), c = lit(rows, x, y + 1), d = lit(rows, x + 1, y + 1);
      if ((a && d && !b && !c) || (b && c && !a && !d)) checker++;
    }
  }
  return { lone, checker };
}

// Lit pixels that sit in no fully lit n x n block (none for n = 1).
function thinPixels(rows, n) {
  const thin = [];
  if (n < 2) return thin;
  const full = (bx, by) => {
    for (let dy = 0; dy < n; dy++) for (let dx = 0; dx < n; dx++) if (!lit(rows, bx + dx, by + dy)) return false;
    return true;
  };
  const covered = (x, y) => {
    for (let by = y - n + 1; by <= y; by++) for (let bx = x - n + 1; bx <= x; bx++) if (full(bx, by)) return true;
    return false;
  };
  for (let y = 0; y < rows.length; y++) {
    for (let x = 0; x < rows[y].length; x++) if (lit(rows, x, y) && !covered(x, y)) thin.push(`${x},${y}`);
  }
  return thin;
}

// Pixels whose mirror position inside the ink box differs.
function asymmetry(rows) {
  const b = bounds(rows);
  if (!b) return 0;
  let diff = 0;
  for (let y = b.top; y <= b.bottom; y++) {
    for (let x = b.left; x <= b.right; x++) {
      if (lit(rows, x, y) !== lit(rows, b.left + b.right - x, y)) diff++;
    }
  }
  return diff;
}

const flat = (rows) => rows.map((r) => [...r].map((c) => (c === '#' ? 1 : 0)));

const KERNEL = [[1, 2, 1], [2, 4, 2], [1, 2, 1]];

// Gaussian-like 3x3 blur, weights sum 16, cell edges read as dark.
function blurred(rows) {
  return blurGrid(flat(rows));
}

function blurGrid(g) {
  return g.map((row, y) => row.map((_, x) => {
    let sum = 0;
    for (let dy = -1; dy <= 1; dy++) {
      for (let dx = -1; dx <= 1; dx++) sum += KERNEL[dy + 1][dx + 1] * ((g[y + dy] && g[y + dy][x + dx]) || 0);
    }
    return sum / 16;
  }));
}

// OLED glow: lit pixels dilated 1 px (plus shape) inside a 1-cell dark margin, then blurred.
function bloomed(rows) {
  const w = rows.length ? rows[0].length : 0;
  const g = flat(rows);
  const at = (x, y) => (g[y] && g[y][x]) || 0;
  const grown = [];
  for (let y = -1; y <= rows.length; y++) {
    const line = [];
    for (let x = -1; x <= w; x++) line.push(at(x, y) || at(x - 1, y) || at(x + 1, y) || at(x, y - 1) || at(x, y + 1));
    grown.push(line);
  }
  return blurGrid(grown);
}

function hamming(a, b) {
  return sumDiff(a, b, (p, q) => (p !== q ? 1 : 0));
}

// Sum of absolute differences between blurred cells.
function blurDistance(a, b) {
  return sumDiff(a, b, (p, q) => Math.abs(p - q));
}

function sumDiff(a, b, fn) {
  let d = 0;
  const h = Math.max(a.length, b.length);
  for (let y = 0; y < h; y++) {
    const w = Math.max((a[y] || []).length, (b[y] || []).length);
    for (let x = 0; x < w; x++) d += fn((a[y] || [])[x] || 0, (b[y] || [])[x] || 0);
  }
  return d;
}

// ── PNG renderers ────────────────────────────────────────────

function drawGridCell(cv, rows, ox, oy, litColor) {
  const w = rows[0].length;
  const h = rows.length;
  cv.outline(ox, oy, w * GRID_SCALE + 2, h * GRID_SCALE + 2, COLORS.outline);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const px = ox + 1 + x * GRID_SCALE;
      const py = oy + 1 + y * GRID_SCALE;
      cv.fill(px, py, GRID_SCALE, GRID_SCALE, COLORS.gridLine);
      cv.fill(px, py, GRID_SCALE - 1, GRID_SCALE - 1, rows[y][x] === '#' ? litColor : COLORS.darkPixel);
    }
  }
}

function renderGrid(font, cmp, file) {
  const codes = new Set(core.codesOf(font).filter((c) => hasInk(font.glyphs[c])));
  if (cmp) for (const c of core.codesOf(cmp)) if (hasInk(cmp.glyphs[c])) codes.add(c);
  const list = [...codes].sort((a, b) => a - b);
  const gap = 2;
  const pairGap = 8;
  const margin = 8;
  const cellW = (f) => f.width * GRID_SCALE + 2;
  const cellH = (f) => f.height * GRID_SCALE + 2;
  const unitW = cellW(font) + (cmp ? cellW(cmp) + gap : 0);
  const unitH = Math.max(cellH(font), cmp ? cellH(cmp) : 0);
  const cols = Math.min(GRID_PER_ROW, Math.max(1, list.length));
  const rowCount = Math.max(1, Math.ceil(list.length / GRID_PER_ROW));
  const cv = new Canvas(margin * 2 + cols * unitW + (cols - 1) * pairGap,
    margin * 2 + rowCount * unitH + (rowCount - 1) * pairGap, COLORS.bg);
  list.forEach((code, i) => {
    let ox = margin + (i % GRID_PER_ROW) * (unitW + pairGap);
    const oy = margin + Math.floor(i / GRID_PER_ROW) * (unitH + pairGap);
    if (cmp) {
      drawGridCell(cv, glyphOf(cmp, code) || core.makeFont('', cmp.width, cmp.height).glyphs[32], ox, oy, COLORS.litCompare);
      ox += cellW(cmp) + gap;
    }
    drawGridCell(cv, font.glyphs[code] || core.makeFont('', font.width, font.height).glyphs[32], ox, oy, COLORS.lit);
  });
  cv.save(file);
  return list.length;
}

const advance = (font) => font.width + font.spacing;
const lineWidth = (font, str) => str.length * advance(font) - font.spacing;

// Draws one string; blank non-space glyphs get a faint box.
function drawString(cv, font, str, ox, oy, scale, litColor) {
  [...str].forEach((ch, i) => {
    const code = codeOf(ch);
    const rows = glyphOf(font, code);
    const gx = ox + i * advance(font) * scale;
    if (!rows || !hasInk(rows)) {
      if (code !== 32) cv.outline(gx, oy, font.width * scale, font.height * scale, COLORS.faintBox);
      return;
    }
    for (let y = 0; y < font.height; y++) {
      for (let x = 0; x < font.width; x++) {
        if (rows[y][x] === '#') cv.fill(gx + x * scale, oy + y * scale, scale, scale, litColor);
      }
    }
  });
}

function renderStrings(font, cmp, file) {
  const margin = 16;
  const lineGap = 2 * STRING_SCALE;
  const blockGap = 5 * STRING_SCALE;
  const fonts = cmp ? [cmp, font] : [font];
  const blockH = fonts.reduce((s, f) => s + f.height * STRING_SCALE, 0) + (fonts.length - 1) * lineGap;
  const width = Math.max(...TEST_STRINGS.map((s) => Math.max(...fonts.map((f) => lineWidth(f, s))))) * STRING_SCALE;
  const cv = new Canvas(width + margin * 2, margin * 2 + TEST_STRINGS.length * (blockH + blockGap) - blockGap, COLORS.bg);
  TEST_STRINGS.forEach((str, i) => {
    let y = margin + i * (blockH + blockGap);
    fonts.forEach((f) => {
      drawString(cv, f, str, margin, y, STRING_SCALE, f === font ? COLORS.lit : COLORS.litCompare);
      y += f.height * STRING_SCALE + lineGap;
    });
  });
  cv.save(file);
}

// Wraps each string at the last whole glyph that fits in 128 px, then fills pages of whole rows.
function renderOled(font, filePrefix) {
  const perRow = Math.max(1, Math.floor((OLED_W + font.spacing) / advance(font)));
  const rowsPerPage = Math.max(1, Math.floor((OLED_H + LINE_GAP) / (font.height + LINE_GAP)));
  const lines = [];
  for (const str of TEST_STRINGS) {
    for (let i = 0; i < str.length; i += perRow) lines.push(str.slice(i, i + perRow));
  }
  const pages = Math.ceil(lines.length / rowsPerPage);
  for (let p = 0; p < pages; p++) {
    const cv = new Canvas(OLED_W * OLED_SCALE, OLED_H * OLED_SCALE, COLORS.oledBg);
    lines.slice(p * rowsPerPage, (p + 1) * rowsPerPage).forEach((line, row) => {
      const y0 = row * (font.height + LINE_GAP);
      [...line].forEach((ch, i) => {
        const rows = glyphOf(font, codeOf(ch));
        if (!rows) return;
        for (let y = 0; y < font.height; y++) {
          for (let x = 0; x < font.width; x++) {
            if (rows[y][x] !== '#') continue;
            const px = (i * advance(font) + x) * OLED_SCALE;
            const py = (y0 + y) * OLED_SCALE;
            cv.fill(px, py, OLED_SCALE - 1, OLED_SCALE - 1, COLORS.oledLit);
          }
        }
      });
    });
    cv.save(`${filePrefix}-${p + 1}.png`);
  }
  return { pages, rows: lines.length };
}

// ── screens ──────────────────────────────────────────────────

// Draws str into a 128 x 64 pixel grid; missing glyphs draw nothing and are recorded.
function putText(grid, font, str, x0, y0, warn, invert) {
  if (lineWidth(font, str) > OLED_W) warn(`overflow: "${str}" is ${lineWidth(font, str)} px wide in ${font.name}`);
  [...str].forEach((ch, i) => {
    const rows = glyphOf(font, codeOf(ch));
    if (!rows || !hasInk(rows)) {
      if (ch !== ' ') warn(`missing glyph '${ch}' in ${font.name}`);
      return;
    }
    for (let y = 0; y < font.height; y++) {
      for (let x = 0; x < font.width; x++) {
        const gx = x0 + i * advance(font) + x;
        const gy = y0 + y;
        if (rows[y][x] === '#' && gx >= 0 && gx < OLED_W && gy >= 0 && gy < OLED_H) grid[gy][gx] = invert ? 0 : 1;
      }
    }
  });
}

function rect(grid, x, y, w, h, value) {
  for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) grid[yy][xx] = value;
}

function knobScreen([small, mid, big], warn) {
  const grid = Array.from({ length: OLED_H }, () => new Array(OLED_W).fill(0));
  putText(grid, small, 'SLOT 3  UNISON', 0, 0, warn);
  putText(grid, mid, 'DEPTH', 0, 12, warn);
  putText(grid, big, '-6.5 DB', 0, 30, warn);
  rect(grid, 0, 58, OLED_W, 4, 1);
  rect(grid, 1, 59, OLED_W - 2, 2, 0);
  rect(grid, 1, 59, Math.round((OLED_W - 2) * 0.4), 2, 1);
  return grid;
}

function menuScreen([small], warn) {
  const grid = Array.from({ length: OLED_H }, () => new Array(OLED_W).fill(0));
  ['EQ', 'OUTPUT', 'LAYOUT', 'UPDATE', 'BRIGHTNESS', 'TUNER REF 440', 'ABOUT', 'BACK'].forEach((str, row) => {
    // The band starts 1 px above the row so inverted text has a lit margin on both sides.
    if (row === 2) rect(grid, 0, row * 8 - 1, OLED_W, 9, 1);
    putText(grid, small, str, 0, row * 8, warn, row === 2);
  });
  return grid;
}

function slotScreen([, mid], warn) {
  const grid = Array.from({ length: OLED_H }, () => new Array(OLED_W).fill(0));
  ['1 GATE', '2 AUTOTUNE', '3 OCTAVE', '4 UNISON'].forEach((str, row) => putText(grid, mid, str, 0, row * 16, warn));
  return grid;
}

// Three mockups side by side, drawn like the OLED pages.
function renderScreens(fonts, file, out) {
  const seen = new Set();
  const warn = (msg) => { if (!seen.has(msg)) { seen.add(msg); out.push(`WARN screens: ${msg}`); } };
  const grids = [knobScreen(fonts, warn), menuScreen(fonts, warn), slotScreen(fonts, warn)];
  const side = OLED_W * OLED_SCALE;
  const gap = SCREEN_GAP * OLED_SCALE;
  const cv = new Canvas(grids.length * side + (grids.length + 1) * gap, OLED_H * OLED_SCALE + 2 * gap, COLORS.bg);
  grids.forEach((grid, n) => {
    const ox = gap + n * (side + gap);
    cv.fill(ox, gap, side, OLED_H * OLED_SCALE, COLORS.oledBg);
    grid.forEach((row, y) => row.forEach((on, x) => {
      if (on) cv.fill(ox + x * OLED_SCALE, gap + y * OLED_SCALE, OLED_SCALE - 1, OLED_SCALE - 1, COLORS.oledLit);
    }));
  });
  cv.save(file);
}

// ── recipe ───────────────────────────────────────────────────

const RECIPE_ROUND = 2;
const RECIPE_FOUR_STEP = 3;
const RECIPE_FOUR_STEP_CHARS = '0';
const CORNERS = [['TL', 1, 1], ['TR', -1, 1], ['BL', 1, -1], ['BR', -1, -1]];

// Dark pixels of the stem x stem block at a corner must form the triangle i + j < t, where t is row 0's leading dark run.
function cornerChamfer(rows, cx, cy, sx, sy, n) {
  let t = 0;
  while (t < n && !lit(rows, cx + sx * t, cy)) t++;
  for (let j = 0; j < n; j++) {
    for (let i = 0; i < n; i++) if (lit(rows, cx + sx * i, cy + sy * j) === (i + j < t)) return -1;
  }
  return t;
}

// Convex outer corners where a bar and a stem of thickness >= n both run on past the corner block, with the corner shape named.
function glyphCorners(rows, n) {
  const found = [];
  for (const [name, sx, sy] of CORNERS) {
    for (let cy = 0; cy < rows.length; cy++) {
      for (let cx = 0; cx < rows[cy].length; cx++) {
        let edge = true;
        for (let k = 0; k <= n; k++) if (lit(rows, cx - sx, cy + sy * k) || lit(rows, cx + sx * k, cy - sy)) edge = false;
        if (!edge || !lit(rows, cx + sx * (n - 1), cy) || !lit(rows, cx, cy + sy * (n - 1))) continue;
        let joins = true;
        for (let k = 0; k < n; k++) if (!lit(rows, cx + sx * n, cy + sy * k) || !lit(rows, cx + sx * k, cy + sy * n)) joins = false;
        if (!joins) continue;
        found.push({ name, sx, sy, cx, cy, t: cornerChamfer(rows, cx, cy, sx, sy, n) });
      }
    }
  }
  return found;
}

// Runs [x0, x1] of one row.
function rowRuns(rows, y) {
  const runs = [];
  let start = -1;
  const w = rows[y] ? rows[y].length : 0;
  for (let x = 0; x <= w; x++) {
    if (lit(rows, x, y) && start < 0) start = x;
    if (!lit(rows, x, y) && start >= 0) { runs.push([start, x - 1]); start = -1; }
  }
  return runs;
}

const overlaps = (a, b) => a[0] <= b[1] && b[0] <= a[1];

// Rows where a run slides sideways with both ends moving the same way, with the run length.
function diagonalRows(rows) {
  const found = [];
  for (let y = 0; y < rows.length; y++) {
    for (const run of rowRuns(rows, y)) {
      const shifted = [y - 1, y + 1].some((ny) => rowRuns(rows, ny).some((o) => overlaps(run, o) &&
        run[0] !== o[0] && run[0] - o[0] === run[1] - o[1]));
      if (shifted) found.push({ y, len: run[1] - run[0] + 1 });
    }
  }
  return found;
}

// Recipe findings for one glyph, as report lines.
function recipeFindings(font, code, n) {
  const rows = font.glyphs[code];
  const who = tag(font, code);
  const out = [];
  const fourStepOk = RECIPE_FOUR_STEP_CHARS.includes(String.fromCharCode(code));
  for (const c of glyphCorners(rows, n)) {
    const ok = c.t === 0 || c.t === RECIPE_ROUND || (c.t === RECIPE_FOUR_STEP && fourStepOk);
    if (!ok) out.push(`corner ${who} ${c.name} at ${c.cx},${c.cy}: ${c.t < 0 ? 'irregular' : `chamfer ${c.t}`}`);
    if (c.t === RECIPE_ROUND && !lit(rows, c.cx + c.sx * n, c.cy + c.sy * n)) {
      out.push(`inner chamfer ${who} ${c.name} at ${c.cx},${c.cy}: pixel ${c.cx + c.sx * n},${c.cy + c.sy * n} dark`);
    }
  }
  for (const d of diagonalRows(rows)) {
    if (d.len !== n && d.len !== n + 1) out.push(`diagonal ${who} row ${d.y}: run length ${d.len}`);
  }
  return out;
}

function lintRecipe(font, drawn, out, args) {
  out.push('');
  out.push(`-- recipe (stem ${args.stem}: corners square or round-${RECIPE_ROUND}, inner chamfer lit, diagonal runs ${args.stem} or ${args.stem + 1}) --`);
  const findings = drawn.flatMap((code) => recipeFindings(font, code, args.stem));
  out.push(...(findings.length ? findings.map((f) => `${args.recipeGate ? 'FAIL' : 'WARN'} recipe ${f}`) : ['none']));
  return findings;
}

// ── trace ────────────────────────────────────────────────────

const traceCols = (src) => (src.some((row) => row[8] === '#') ? TRACE_COLS_9 : TRACE_COLS_8);

// Source cells that are lit when at least half of their mapped target pixels are lit.
function shrink(target, src) {
  const cols = traceCols(src);
  return src.map((row, y) => [...row].map((_, x) => {
    const cells = TRACE_ROWS[y].flatMap((ty) => cols[x].map((tx) => lit(target, tx, ty)));
    return cells.filter(Boolean).length * 2 >= cells.length ? 1 : 0;
  }));
}

// Source glyph drawn as its mapped target blocks, as a target-sized lit test.
function expandSource(src) {
  const cols = traceCols(src);
  const grid = new Set();
  src.forEach((row, y) => [...row].forEach((c, x) => {
    if (c === '#') for (const ty of TRACE_ROWS[y]) for (const tx of cols[x]) grid.add(`${tx},${ty}`);
  }));
  return grid;
}

// Per-glyph trace results for codes drawn in both fonts.
function traceGlyphs(font, src) {
  return core.codesOf(font).filter((c) => hasInk(font.glyphs[c]) && glyphOf(src, c) && hasInk(src.glyphs[c])).map((code) => {
    const rows = src.glyphs[code];
    const shrunk = shrink(font.glyphs[code], rows);
    const cells = [];
    rows.forEach((row, y) => [...row].forEach((c, x) => { if ((c === '#' ? 1 : 0) !== shrunk[y][x]) cells.push(`${x},${y}`); }));
    return { code, cells };
  });
}

function lintTrace(font, src, out) {
  const results = traceGlyphs(font, src);
  out.push('');
  out.push(`-- trace vs ${src.name} --`);
  const failing = [];
  for (const r of results) {
    out.push(`${tag(font, r.code).padEnd(6)} diff ${r.cells.length}${r.cells.length ? ': ' + r.cells.join(' ') : ''}`);
    if (r.cells.length > TRACE_MAX_DIFF) failing.push(`FAIL trace ${tag(font, r.code)} diff ${r.cells.length}`);
  }
  return { results, failing };
}

// Per glyph: mapped source, target, overlay (source only, target only, both).
function renderTrace(font, src, results, file) {
  const gap = 4;
  const pad = 8;
  const pw = font.width * TRACE_SCALE;
  const ph = font.height * TRACE_SCALE;
  const unitW = pw * 3 + gap * 2;
  const perRow = 4;
  const cv = new Canvas(pad * 2 + perRow * unitW + (perRow - 1) * pad,
    pad * 2 + Math.max(1, Math.ceil(results.length / perRow)) * (ph + pad) - pad, COLORS.bg);
  results.forEach((r, i) => {
    const ox = pad + (i % perRow) * (unitW + pad);
    const oy = pad + Math.floor(i / perRow) * (ph + pad);
    const mapped = expandSource(src.glyphs[r.code]);
    const target = font.glyphs[r.code];
    for (let k = 0; k < 3; k++) cv.fill(ox + k * (pw + gap), oy, pw, ph, COLORS.darkPixel);
    for (let y = 0; y < font.height; y++) {
      for (let x = 0; x < font.width; x++) {
        const a = mapped.has(`${x},${y}`);
        const b = lit(target, x, y);
        const at = (k, color) => cv.fill(ox + k * (pw + gap) + x * TRACE_SCALE, oy + y * TRACE_SCALE, TRACE_SCALE, TRACE_SCALE, color);
        if (a) at(0, COLORS.litCompare);
        if (b) at(1, COLORS.lit);
        if (a || b) at(2, a && b ? COLORS.lit : a ? COLORS.litCompare : COLORS.targetOnly);
      }
    }
  });
  cv.save(file);
}

// ── lint ─────────────────────────────────────────────────────

const label = (font, code) => core.charLabel(font, code);
const tag = (font, code) => (code >= core.SYMBOL_FIRST ? label(font, code) : `'${label(font, code)}'`);

function glyphInfo(font, code) {
  const rows = font.glyphs[code];
  const b = bounds(rows);
  return { code, rows, b, bearL: b.left, bearR: font.width - 1 - b.right };
}

function lintGlyphs(font, out, args) {
  const drawn = core.codesOf(font).filter((c) => hasInk(font.glyphs[c]));
  const info = new Map(drawn.map((c) => [c, glyphInfo(font, c)]));
  const refCodes = [...CAPS_DIGITS].map(codeOf).filter((c) => info.has(c));
  const modeTop = mode(refCodes.map((c) => info.get(c).b.top));
  const modeBottom = mode(refCodes.map((c) => info.get(c).b.bottom));
  const modeBear = mode(refCodes.map((c) => info.get(c).bearL + info.get(c).bearR));
  const stems = new Map(drawn.map((c) => [c, stemWidths(font.glyphs[c])]));
  const modeStem = mode(refCodes.flatMap((c) => stems.get(c)));
  const wantStem = args.stem;
  const isRef = new Set(refCodes);

  out.push(`== ${font.name}: ${font.width} x ${font.height} px, spacing ${font.spacing} px, ${drawn.length} drawn glyphs ==`);
  out.push(`modal cap top row ${modeTop}, baseline row ${modeBottom}, bearing sum ${modeBear} px, stem width ${modeStem} px (A-Z, 0-9), expected stem ${wantStem} px`);
  out.push('');
  out.push('-- per glyph: bounds, bearings, stems, runs, lone pixels, checkerboards --');

  const flagCount = { vertical: 0, bearing: 0, stem: 0 };
  let loneTotal = 0;
  let checkerTotal = 0;
  const flagged = [];
  for (const code of drawn) {
    const g = info.get(code);
    const flags = [];
    if (isRef.has(code)) {
      if (g.b.top !== modeTop || g.b.bottom !== modeBottom) { flags.push('VERT'); flagCount.vertical++; }
      if (Math.abs(g.bearL + g.bearR - modeBear) > 1) { flags.push('BEAR'); flagCount.bearing++; }
      if (stems.get(code).some((w) => w !== wantStem)) { flags.push('STEM'); flagCount.stem++; }
    }
    const vRuns = [];
    for (let x = 0; x < font.width; x++) vRuns.push(...runsOf(columnOf(g.rows, x)).filter((n) => n >= MIN_STEM_RUN));
    const hRuns = g.rows.flatMap(runsOf);
    const { lone, checker } = loneAndCheckers(g.rows);
    loneTotal += lone;
    checkerTotal += checker;
    if (flags.length) flagged.push(`${tag(font, code)} ${flags.join('+')}`);
    out.push(
      `${tag(font, code).padEnd(6)} t${g.b.top} b${g.b.bottom} l${g.b.left} r${g.b.right}` +
      ` | bear ${g.bearL}/${g.bearR}` +
      ` | stems [${stems.get(code).join(',')}]` +
      ` | v>=${MIN_STEM_RUN} {${histogram(vRuns)}} h {${histogram(hRuns)}}` +
      ` | lone ${lone} chk ${checker}${flags.length ? ' | ' + flags.join(' ') : ''}`);
  }

  out.push('');
  out.push('-- symmetry (pixels differing from left-right mirror in the ink box) --');
  out.push([...SYMMETRIC].filter((ch) => info.has(codeOf(ch)))
    .map((ch) => `${ch}=${asymmetry(font.glyphs[codeOf(ch)])}`).join('  '));

  out.push('');
  out.push(`-- thin (WARN: lit pixel in no fully lit ${wantStem} x ${wantStem} block${wantStem < 2 ? '; off at stem 1' : ''}) --`);
  const thinLines = drawn.filter((c) => !THIN_IGNORE.includes(String.fromCharCode(c)))
    .map((c) => ({ c, thin: thinPixels(font.glyphs[c], wantStem) })).filter((t) => t.thin.length)
    .map((t) => `WARN thin ${tag(font, t.c)} ${t.thin.length} px at ${t.thin.join(' ')}`);
  out.push(...(thinLines.length ? thinLines : ['none']));

  out.push('');
  out.push('-- summary --');
  out.push(`flags: ${flagged.length ? flagged.join(', ') : 'none'}`);
  out.push(`VERT ${flagCount.vertical}, BEAR ${flagCount.bearing}, STEM ${flagCount.stem}; lone pixels ${loneTotal}, checkerboard 2x2 ${checkerTotal}`);
  return drawn;
}

// Distance table keyed by "ab" code pair for one font.
function distanceFn(font) {
  const cache = new Map();
  const entry = (code) => {
    if (!cache.has(code)) {
      const rows = glyphOf(font, code);
      cache.set(code, rows && hasInk(rows) ? { raw: flat(rows), blur: blurred(rows), bloom: bloomed(rows) } : null);
    }
    return cache.get(code);
  };
  return (a, b) => {
    const ea = entry(a);
    const eb = entry(b);
    return ea && eb ? { raw: hamming(ea.raw, eb.raw), blur: blurDistance(ea.blur, eb.blur), bloom: blurDistance(ea.bloom, eb.bloom) } : null;
  };
}

// A lowercase letter drawn identical to its capital is the same glyph, not a confusable pair.
const isCaseCopy = (font, code) =>
  code >= 97 && code <= 122 && String(glyphOf(font, code)) === String(glyphOf(font, code - 32));

// Returns the required pairs that fall under the blur or bloom floor.
function lintConfusables(font, cmp, allDrawn, out, args) {
  const inSet = (code) => args.set === 'all' || DIGITS_SET.includes(String.fromCharCode(code));
  const drawn = allDrawn.filter((code) => !isCaseCopy(font, code) && hasInk(font.glyphs[code]) && inSet(code));
  const dist = distanceFn(font);
  const cdist = cmp ? distanceFn(cmp) : null;
  const fmt = (d) => (d ? `raw ${String(d.raw).padStart(3)} blur ${d.blur.toFixed(1).padStart(5)} bloom ${d.bloom.toFixed(1).padStart(5)}` : 'n/a');
  const line = (a, b) => {
    const d = dist(a, b);
    const name = `${label(font, a)}${label(font, b)}`.padEnd(4);
    if (!cdist) return `${name} ${fmt(d)}`;
    const c = cdist(a, b);
    return `${name} ${font.name}: ${fmt(d)} | ${cmp.name}: ${fmt(c)}`;
  };

  out.push('');
  out.push(`-- ${CLOSEST_COUNT} closest pairs by blurred distance (3x3 weighted blur, unthresholded), ties by raw --`);
  const pairs = [];
  for (let i = 0; i < drawn.length; i++) {
    for (let j = i + 1; j < drawn.length; j++) {
      const d = dist(drawn[i], drawn[j]);
      pairs.push({ a: drawn[i], b: drawn[j], ...d });
    }
  }
  pairs.sort((p, q) => p.blur - q.blur || p.raw - q.raw);
  pairs.slice(0, CLOSEST_COUNT).forEach((p) => out.push(line(p.a, p.b)));

  out.push('');
  const area = (font.width * font.height) / PAIR_REF_AREA;
  const minBlur = PAIR_MIN_BLUR * area;
  const minBloom = PAIR_MIN_BLOOM * area;
  const minRaw = Math.ceil(PAIR_MIN_RAW_SHARE * font.width * font.height);
  const bars = args.stem === 1
    ? `raw ${minRaw} px (stem 1: blur and bloom not gated)`
    : `blur ${minBlur.toFixed(1)}, bloom ${minBloom.toFixed(1)}`;
  out.push(`-- required pairs${args.pairs ? ` (bars: ${bars})` : ''} --`);
  const failing = [];
  for (const pair of REQUIRED_PAIRS) {
    const d = dist(codeOf(pair[0]), codeOf(pair[1]));
    if (!d) continue;
    out.push(line(codeOf(pair[0]), codeOf(pair[1])));
    const fails = args.stem === 1 ? d.raw < minRaw : d.blur < minBlur || d.bloom < minBloom;
    if (fails) failing.push(`FAIL ${pair} raw ${d.raw} blur ${d.blur.toFixed(1)} bloom ${d.bloom.toFixed(1)}`);
  }

  if (cdist) {
    out.push('');
    out.push(`-- letters/digits under blur ${CLOSER_BLUR} and closer than in ${cmp.name} --`);
    const refs = [...CAPS_DIGITS].map(codeOf);
    const closer = [];
    for (let i = 0; i < refs.length; i++) {
      for (let j = i + 1; j < refs.length; j++) {
        const d = dist(refs[i], refs[j]);
        const c = cdist(refs[i], refs[j]);
        if (d && c && d.blur < CLOSER_BLUR && d.blur < c.blur) closer.push(line(refs[i], refs[j]));
      }
    }
    out.push(...(closer.length ? closer : ['none']));
    if (font.width !== cmp.width || font.height !== cmp.height) {
      out.push(`note: cell sizes differ (${cmp.width} x ${cmp.height} vs ${font.width} x ${font.height}); distances scale with pixel count.`);
    }
  }
  return failing;
}

// ── main ─────────────────────────────────────────────────────

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.lib) fail('usage: proof.js <library.json> --font "<name>" [--compare "<name>"] [--compare-lib <file>] [--out <dir>] [--pairs] [--stem N] [--set digits|all] [--screens "a,b,c"] [--trace "<source>"]');
  if (!args.font) fail('--font is required');

  const fonts = loadFonts(args.lib);
  const font = pickFont(fonts, args.font, args.lib);
  let cmp = null;
  if (args.compare || args.compareLib) {
    const pool = args.compareLib ? loadFonts(args.compareLib) : fonts;
    cmp = pickFont(pool, args.compare || args.font, args.compareLib || args.lib);
  }

  const outDir = path.resolve(args.out || path.join(__dirname, 'proofs'));
  fs.mkdirSync(outDir, { recursive: true });

  const gridCount = renderGrid(font, cmp, path.join(outDir, 'grid.png'));
  renderStrings(font, cmp, path.join(outDir, 'strings.png'));
  const oled = renderOled(font, path.join(outDir, 'oled'));
  if (cmp) renderOled(cmp, path.join(outDir, 'oled-compare'));

  const out = [];
  const drawn = lintGlyphs(font, out, args);
  const failing = lintConfusables(font, cmp, drawn, out, args);
  if (args.pairs) out.push('', ...(failing.length ? failing : ['all required pairs pass']));
  const recipeFound = args.recipe ? lintRecipe(font, drawn, out, args) : [];
  let traceFail = [];
  if (args.trace) {
    const src = pickFont(fonts, args.trace, args.lib);
    const t = lintTrace(font, src, out);
    out.push(...(t.failing.length ? t.failing : ['all traced glyphs within tolerance']));
    renderTrace(font, src, t.results, path.join(outDir, 'trace.png'));
    traceFail = t.failing;
  }
  if (args.screens) {
    const names = args.screens.split(',').map((n) => n.trim());
    if (names.length !== 3) fail('--screens needs three font names');
    renderScreens(names.map((n) => pickFont(fonts, n, args.lib)), path.join(outDir, 'screens.png'), out);
  }
  out.push('');
  out.push(`grid.png: ${gridCount} glyphs; oled-N.png: ${oled.rows} rows on ${oled.pages} pages`);

  fs.writeFileSync(path.join(outDir, 'lint.txt'), out.join('\n') + '\n');
  console.log(out.join('\n'));
  console.log(`\nwrote ${outDir}`);
  if ((args.pairs && failing.length) || traceFail.length || (args.recipeGate && recipeFound.length)) process.exitCode = 1;
}

main();
