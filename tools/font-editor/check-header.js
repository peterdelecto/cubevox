#!/usr/bin/env node
/* Fails unless fonts/cubevox_fonts.h is the header the font editor exports from fonts/cubevox-fonts.json.
   The firmware and the OLED goldens both come from the header, so a hand edit or a stale export would
   pass every other test. Usage: node tools/font-editor/check-header.js */

'use strict';

const fs = require('fs');
const path = require('path');
const FontCore = require('./font-core.js');

const FONTS_DIR = path.join(__dirname, 'fonts');
const LIBRARY = path.join(FONTS_DIR, 'cubevox-fonts.json');
const HEADER = path.join(FONTS_DIR, 'cubevox_fonts.h');

function fail(message) {
  console.error(`[ERROR] ${message}`);
  process.exit(1);
}

let library;
try {
  library = FontCore.parseLibrary(JSON.parse(fs.readFileSync(LIBRARY, 'utf8')));
} catch (err) {
  fail(`${LIBRARY} does not read as JSON: ${err.message}`);
}
if (library.error) fail(`${LIBRARY}: ${library.error}`);

const want = FontCore.toCHeader(library.fonts).split('\n');
const have = fs.readFileSync(HEADER, 'utf8').split('\n');
const line = want.findIndex((text, i) => text !== have[i]);
if (line >= 0 || have.length !== want.length) {
  const at = line >= 0 ? line : Math.min(have.length, want.length);
  fail(`cubevox_fonts.h differs from what cubevox-fonts.json exports, first at line ${at + 1}. ` +
       'Open the font editor (node tools/font-editor/serve.js) and export the header.');
}
console.log(`[fonts] cubevox_fonts.h matches cubevox-fonts.json (${library.fonts.length} fonts)`);
