/* Font model, glyph transforms, library load/save and C export.
   Runs in the browser (window.FontCore) and in Node (require). */

(function (root) {
  'use strict';

  const ASCII_FIRST = 32;
  const ASCII_LAST = 126;
  const SYMBOL_FIRST = 128;
  const SYMBOL_LAST = 255;
  const MAX_SIZE = 32;
  const MAX_SPACING = 8;
  const LIBRARY_FORMAT = 'cubevox-font-library';

  // A glyph is an array of row strings, '#' for a lit pixel and '.' for dark.

  const blank = (w, h) => Array.from({ length: h }, () => '.'.repeat(w));

  const newId = () => Math.random().toString(36).slice(2, 10);

  function makeFont(name, width, height, spacing = 1) {
    const glyphs = {};
    for (let c = ASCII_FIRST; c <= ASCII_LAST; c++) glyphs[c] = blank(width, height);
    return { id: newId(), name, width, height, spacing, glyphs, symbolNames: {} };
  }

  const isOn = (rows, x, y) => rows[y][x] === '#';

  function setPixel(rows, x, y, on) {
    return rows.map((row, i) =>
      i === y ? row.slice(0, x) + (on ? '#' : '.') + row.slice(x + 1) : row);
  }

  // Keeps the top-left corner; grows with dark pixels, shrinks by cutting right and bottom.
  function resize(rows, w, h) {
    return Array.from({ length: h }, (_, y) => ((rows[y] || '') + '.'.repeat(w)).slice(0, w));
  }

  // Pixels pushed past an edge are dropped, not wrapped.
  function shift(rows, dx, dy) {
    const h = rows.length;
    const w = h ? rows[0].length : 0;
    return Array.from({ length: h }, (_, y) => {
      let s = '';
      for (let x = 0; x < w; x++) {
        const sx = x - dx;
        const sy = y - dy;
        const inside = sx >= 0 && sx < w && sy >= 0 && sy < h;
        s += inside && rows[sy][sx] === '#' ? '#' : '.';
      }
      return s;
    });
  }

  const invert = (rows) =>
    rows.map((row) => [...row].map((c) => (c === '#' ? '.' : '#')).join(''));

  const clear = (rows) => blank(rows.length ? rows[0].length : 0, rows.length);

  const codesOf = (font) => Object.keys(font.glyphs).map(Number).sort((a, b) => a - b);

  function nextSymbolCode(font) {
    const used = codesOf(font).filter((c) => c >= SYMBOL_FIRST);
    const code = used.length ? used[used.length - 1] + 1 : SYMBOL_FIRST;
    return code <= SYMBOL_LAST ? code : null;
  }

  function addSymbol(font) {
    const code = nextSymbolCode(font);
    if (code === null) return null;
    font.glyphs[code] = blank(font.width, font.height);
    font.symbolNames[code] = `symbol ${code}`;
    return code;
  }

  function removeSymbol(font, code) {
    if (code < SYMBOL_FIRST) return false;
    delete font.glyphs[code];
    delete font.symbolNames[code];
    return true;
  }

  function resizeFont(font, w, h) {
    for (const code of codesOf(font)) font.glyphs[code] = resize(font.glyphs[code], w, h);
    font.width = w;
    font.height = h;
  }

  function charLabel(font, code) {
    if (code >= SYMBOL_FIRST) return font.symbolNames[code] || `symbol ${code}`;
    if (code === 32) return 'space';
    return String.fromCharCode(code);
  }

  const hex2 = (n) => n.toString(16).toUpperCase().padStart(2, '0');

  // ── library load / save ──────────────────────────────────────

  function toLibrary(fonts) {
    return {
      format: LIBRARY_FORMAT,
      version: 1,
      fonts: fonts.map((f) => ({
        name: f.name,
        width: f.width,
        height: f.height,
        spacing: f.spacing,
        symbolNames: { ...f.symbolNames },
        glyphs: Object.fromEntries(codesOf(f).map((c) => [c, f.glyphs[c]])),
      })),
    };
  }

  const clampInt = (v, lo, hi, fallback) => {
    const n = Number.parseInt(v, 10);
    return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : fallback;
  };

  // Returns { fonts } or { error }. Bad glyph rows are padded or cut to the font size.
  function parseLibrary(obj) {
    if (!obj || obj.format !== LIBRARY_FORMAT || !Array.isArray(obj.fonts)) {
      return { error: 'This file is not a cubevox font library.' };
    }
    const fonts = [];
    for (const raw of obj.fonts) {
      if (!raw || typeof raw.name !== 'string' || !raw.glyphs) {
        return { error: 'A font in this file has no name or no glyphs.' };
      }
      const w = clampInt(raw.width, 1, MAX_SIZE, 8);
      const h = clampInt(raw.height, 1, MAX_SIZE, 8);
      const font = makeFont(raw.name, w, h, clampInt(raw.spacing, 0, MAX_SPACING, 1));
      for (const [key, rows] of Object.entries(raw.glyphs)) {
        const code = Number(key);
        const valid = Number.isInteger(code) && code >= ASCII_FIRST && code <= SYMBOL_LAST && code !== 127;
        if (!valid || !Array.isArray(rows)) continue;
        const clean = rows.map((r) => String(r).replace(/[^#]/g, '.'));
        font.glyphs[code] = resize(clean, w, h);
      }
      for (const [key, name] of Object.entries(raw.symbolNames || {})) {
        if (font.glyphs[key] && Number(key) >= SYMBOL_FIRST) font.symbolNames[key] = String(name);
      }
      fonts.push(font);
    }
    return { fonts };
  }

  // ── C export ─────────────────────────────────────────────────

  // SH1106 page layout: ceil(h / 8) pages top first, one byte per column, bit 0 on top.
  function glyphBytes(rows, w, h) {
    const out = [];
    for (let p = 0; p < Math.ceil(h / 8); p++) {
      for (let x = 0; x < w; x++) {
        let b = 0;
        for (let bit = 0; bit < 8; bit++) {
          const y = p * 8 + bit;
          if (y < h && rows[y][x] === '#') b |= 1 << bit;
        }
        out.push(b);
      }
    }
    return out;
  }

  function identFor(name) {
    const words = name.replace(/[^A-Za-z0-9]+/g, ' ').trim().split(/\s+/).filter(Boolean);
    let id = words.map((p) => p[0].toUpperCase() + p.slice(1)).join('') || 'Font';
    if (/^[0-9]/.test(id)) id = 'F' + id;
    return id;
  }

  function commentLabel(font, code) {
    const label = charLabel(font, code).replace(/[\r\n]+/g, ' ');
    return code >= SYMBOL_FIRST || code === 32 ? label : `'${label}'`;
  }

  function fontBlock(font, ident) {
    const codes = codesOf(font);
    const perGlyph = font.width * Math.ceil(font.height / 8);
    const lines = [
      `// ${font.name.replace(/[\r\n]+/g, ' ')}: ${font.width} x ${font.height} px, ` +
        `${font.spacing} px spacing, ${codes.length} glyphs`,
      `namespace ${ident} {`,
      `constexpr uint8_t kWidth = ${font.width};`,
      `constexpr uint8_t kHeight = ${font.height};`,
      `constexpr uint8_t kSpacing = ${font.spacing};`,
      `constexpr uint16_t kGlyphCount = ${codes.length};`,
      `constexpr uint16_t kBytesPerGlyph = ${perGlyph};`,
      `constexpr uint8_t kCodes[kGlyphCount] = {`,
    ];
    for (let i = 0; i < codes.length; i += 16) {
      lines.push('  ' + codes.slice(i, i + 16).map((c) => `0x${hex2(c)}`).join(', ') + ',');
    }
    lines.push('};', 'constexpr uint8_t kGlyphs[kGlyphCount][kBytesPerGlyph] = {');
    for (const code of codes) {
      const bytes = glyphBytes(font.glyphs[code], font.width, font.height);
      lines.push(`  {${bytes.map((b) => `0x${hex2(b)}`).join(', ')}},  // 0x${hex2(code)} ${commentLabel(font, code)}`);
    }
    lines.push('};', `}  // namespace ${ident}`);
    return lines.join('\n');
  }

  function toCHeader(fonts) {
    const used = new Set();
    const blocks = fonts.map((font) => {
      let ident = identFor(font.name);
      for (let n = 2; used.has(ident); n++) ident = `${identFor(font.name)}${n}`;
      used.add(ident);
      return fontBlock(font, ident);
    });
    return [
      '// cubevox fonts, generated by tools/font-editor. Edit the .json library and re-export.',
      '//',
      '// Each glyph is ceil(kHeight / 8) pages, top page first. A page holds one byte per',
      '// column, left to right, with bit 0 as the top pixel (SH1106 page layout).',
      '// kCodes ascends, so ASCII 32..126 sit at index code - 32 and symbols follow.',
      '',
      '#pragma once',
      '#include <cstdint>',
      '',
      'namespace cubevox::fonts {',
      '',
      blocks.join('\n\n'),
      '',
      '}  // namespace cubevox::fonts',
      '',
    ].join('\n');
  }

  // ── example font: classic 5 x 7 glyphs in a 5 x 8 cell ─────────

  // Column bytes, bit 0 on top, for ASCII 32..126.
  const CLASSIC_COLUMNS = [
    [0x00, 0x00, 0x00, 0x00, 0x00], [0x00, 0x00, 0x5F, 0x00, 0x00], [0x00, 0x07, 0x00, 0x07, 0x00],
    [0x14, 0x7F, 0x14, 0x7F, 0x14], [0x24, 0x2A, 0x7F, 0x2A, 0x12], [0x23, 0x13, 0x08, 0x64, 0x62],
    [0x36, 0x49, 0x56, 0x20, 0x50], [0x00, 0x05, 0x03, 0x00, 0x00], [0x00, 0x1C, 0x22, 0x41, 0x00],
    [0x00, 0x41, 0x22, 0x1C, 0x00], [0x14, 0x08, 0x3E, 0x08, 0x14], [0x08, 0x08, 0x3E, 0x08, 0x08],
    [0x00, 0x50, 0x30, 0x00, 0x00], [0x08, 0x08, 0x08, 0x08, 0x08], [0x00, 0x60, 0x60, 0x00, 0x00],
    [0x20, 0x10, 0x08, 0x04, 0x02], [0x3E, 0x51, 0x49, 0x45, 0x3E], [0x00, 0x42, 0x7F, 0x40, 0x00],
    [0x42, 0x61, 0x51, 0x49, 0x46], [0x21, 0x41, 0x45, 0x4B, 0x31], [0x18, 0x14, 0x12, 0x7F, 0x10],
    [0x27, 0x45, 0x45, 0x45, 0x39], [0x3C, 0x4A, 0x49, 0x49, 0x30], [0x01, 0x71, 0x09, 0x05, 0x03],
    [0x36, 0x49, 0x49, 0x49, 0x36], [0x06, 0x49, 0x49, 0x29, 0x1E], [0x00, 0x36, 0x36, 0x00, 0x00],
    [0x00, 0x56, 0x36, 0x00, 0x00], [0x08, 0x14, 0x22, 0x41, 0x00], [0x14, 0x14, 0x14, 0x14, 0x14],
    [0x00, 0x41, 0x22, 0x14, 0x08], [0x02, 0x01, 0x51, 0x09, 0x06], [0x32, 0x49, 0x79, 0x41, 0x3E],
    [0x7E, 0x11, 0x11, 0x11, 0x7E], [0x7F, 0x49, 0x49, 0x49, 0x36], [0x3E, 0x41, 0x41, 0x41, 0x22],
    [0x7F, 0x41, 0x41, 0x22, 0x1C], [0x7F, 0x49, 0x49, 0x49, 0x41], [0x7F, 0x09, 0x09, 0x01, 0x01],
    [0x3E, 0x41, 0x41, 0x51, 0x32], [0x7F, 0x08, 0x08, 0x08, 0x7F], [0x00, 0x41, 0x7F, 0x41, 0x00],
    [0x20, 0x40, 0x41, 0x3F, 0x01], [0x7F, 0x08, 0x14, 0x22, 0x41], [0x7F, 0x40, 0x40, 0x40, 0x40],
    [0x7F, 0x02, 0x04, 0x02, 0x7F], [0x7F, 0x04, 0x08, 0x10, 0x7F], [0x3E, 0x41, 0x41, 0x41, 0x3E],
    [0x7F, 0x09, 0x09, 0x09, 0x06], [0x3E, 0x41, 0x51, 0x21, 0x5E], [0x7F, 0x09, 0x19, 0x29, 0x46],
    [0x46, 0x49, 0x49, 0x49, 0x31], [0x01, 0x01, 0x7F, 0x01, 0x01], [0x3F, 0x40, 0x40, 0x40, 0x3F],
    [0x1F, 0x20, 0x40, 0x20, 0x1F], [0x7F, 0x20, 0x18, 0x20, 0x7F], [0x63, 0x14, 0x08, 0x14, 0x63],
    [0x03, 0x04, 0x78, 0x04, 0x03], [0x61, 0x51, 0x49, 0x45, 0x43], [0x00, 0x7F, 0x41, 0x41, 0x00],
    [0x02, 0x04, 0x08, 0x10, 0x20], [0x00, 0x41, 0x41, 0x7F, 0x00], [0x04, 0x02, 0x01, 0x02, 0x04],
    [0x40, 0x40, 0x40, 0x40, 0x40], [0x00, 0x01, 0x02, 0x04, 0x00], [0x20, 0x54, 0x54, 0x54, 0x78],
    [0x7F, 0x48, 0x44, 0x44, 0x38], [0x38, 0x44, 0x44, 0x44, 0x20], [0x38, 0x44, 0x44, 0x48, 0x7F],
    [0x38, 0x54, 0x54, 0x54, 0x18], [0x08, 0x7E, 0x09, 0x01, 0x02], [0x08, 0x14, 0x54, 0x54, 0x3C],
    [0x7F, 0x08, 0x04, 0x04, 0x78], [0x00, 0x44, 0x7D, 0x40, 0x00], [0x20, 0x40, 0x44, 0x3D, 0x00],
    [0x00, 0x7F, 0x10, 0x28, 0x44], [0x00, 0x41, 0x7F, 0x40, 0x00], [0x7C, 0x04, 0x18, 0x04, 0x78],
    [0x7C, 0x08, 0x04, 0x04, 0x78], [0x38, 0x44, 0x44, 0x44, 0x38], [0x7C, 0x14, 0x14, 0x14, 0x08],
    [0x08, 0x14, 0x14, 0x18, 0x7C], [0x7C, 0x08, 0x04, 0x04, 0x08], [0x48, 0x54, 0x54, 0x54, 0x20],
    [0x04, 0x3F, 0x44, 0x40, 0x20], [0x3C, 0x40, 0x40, 0x20, 0x7C], [0x1C, 0x20, 0x40, 0x20, 0x1C],
    [0x3C, 0x40, 0x30, 0x40, 0x3C], [0x44, 0x28, 0x10, 0x28, 0x44], [0x0C, 0x50, 0x50, 0x50, 0x3C],
    [0x44, 0x64, 0x54, 0x4C, 0x44], [0x00, 0x08, 0x36, 0x41, 0x00], [0x00, 0x00, 0x7F, 0x00, 0x00],
    [0x00, 0x41, 0x36, 0x08, 0x00], [0x08, 0x04, 0x08, 0x10, 0x08],
  ];

  function fromColumns(cols, h) {
    return Array.from({ length: h }, (_, y) =>
      cols.map((b) => ((b >> y) & 1 ? '#' : '.')).join(''));
  }

  function exampleFont() {
    const font = makeFont('Classic 5x8', 5, 8, 1);
    CLASSIC_COLUMNS.forEach((cols, i) => { font.glyphs[ASCII_FIRST + i] = fromColumns(cols, 8); });
    return font;
  }

  const api = {
    ASCII_FIRST, ASCII_LAST, SYMBOL_FIRST, SYMBOL_LAST, MAX_SIZE, MAX_SPACING,
    makeFont, exampleFont, isOn, setPixel, resize, shift, invert, clear,
    codesOf, addSymbol, removeSymbol, resizeFont, charLabel,
    toLibrary, parseLibrary, glyphBytes, toCHeader,
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.FontCore = api;
})(typeof window !== 'undefined' ? window : globalThis);
