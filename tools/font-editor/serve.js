#!/usr/bin/env node
/* Serves the font editor and reads/writes the font library in fonts/.
   Usage: node tools/font-editor/serve.js [--no-open]  (port 8765, FONT_EDITOR_PORT overrides) */

'use strict';

const fs = require('fs');
const http = require('http');
const path = require('path');
const { execFile } = require('child_process');
const FontCore = require('./font-core.js');

const ROOT = __dirname;
const FONTS_DIR = path.join(ROOT, 'fonts');
const LIBRARY = path.join(FONTS_DIR, 'cubevox-fonts.json');
const CONFLICT = path.join(FONTS_DIR, 'conflict.json');
const HEADER = path.join(FONTS_DIR, 'cubevox_fonts.h');
const FROM_BROWSER = path.join(FONTS_DIR, 'from-browser.json');
const PORT = Number(process.env.FONT_EDITOR_PORT) || 8765;
const MAX_BODY = 8 * 1024 * 1024;

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
};

class HttpError extends Error {
  constructor(status, message, extra = {}) {
    super(message);
    this.status = status;
    this.extra = extra;
  }
}

const versionOf = (file) => String(fs.statSync(file).mtimeMs);

function send(res, status, body, type = 'application/json; charset=utf-8', headers = {}) {
  const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store', ...headers });
  res.end(data);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY) {
        reject(new HttpError(413, 'Body too large.'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

// Returns the library's font count, or throws a 400.
function checkLibrary(text) {
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch (err) {
    throw new HttpError(400, `Body is not valid JSON: ${err.message}`);
  }
  const result = FontCore.parseLibrary(parsed);
  if (result.error) throw new HttpError(400, result.error);
  return result.fonts.length;
}

// Writes through a temp file in the same directory so readers never see a partial file.
function writeAtomic(file, text) {
  const tmp = path.join(path.dirname(file), `.${path.basename(file)}.${process.pid}.tmp`);
  fs.writeFileSync(tmp, text);
  fs.renameSync(tmp, file);
}

function logWrite(file, count) {
  const rel = path.relative(path.join(ROOT, '..', '..'), file);
  console.log(`${new Date().toLocaleTimeString()} wrote ${rel}${count === null ? '' : ` (${count} fonts)`}`);
}

function getLibrary(req, res, url) {
  const version = versionOf(LIBRARY);
  if (url.searchParams.get('versionOnly')) return send(res, 200, { version }, undefined, { ETag: `"${version}"` });
  const library = JSON.parse(fs.readFileSync(LIBRARY, 'utf8'));
  send(res, 200, { version, library }, undefined, { ETag: `"${version}"` });
}

async function putLibrary(req, res) {
  const text = await readBody(req);
  const count = checkLibrary(text);
  const expected = req.headers['if-match'];
  const current = versionOf(LIBRARY);
  if (expected !== undefined && expected.replace(/"/g, '') !== current) {
    throw new HttpError(409, 'The library changed on disk.', { version: current });
  }
  writeAtomic(LIBRARY, text);
  logWrite(LIBRARY, count);
  const version = versionOf(LIBRARY);
  send(res, 200, { version }, undefined, { ETag: `"${version}"` });
}

async function putCopy(req, res, file, validate) {
  const text = await readBody(req);
  const count = validate ? checkLibrary(text) : null;
  writeAtomic(file, text);
  logWrite(file, count);
  send(res, 200, { ok: true });
}

function serveStatic(req, res, url) {
  let rel;
  try {
    rel = decodeURIComponent(url.pathname);
  } catch (err) {
    throw new HttpError(400, 'Bad path.');
  }
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.resolve(ROOT, `.${rel}`);
  if (file !== ROOT && !file.startsWith(ROOT + path.sep)) throw new HttpError(403, 'Forbidden.');
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) throw new HttpError(404, 'Not found.');
  send(res, 200, fs.readFileSync(file), TYPES[path.extname(file)] || 'application/octet-stream');
}

// The editor opened as a file (origin "null") may only hand its browser copy to /api/import.
const FILE_ORIGIN_CORS = {
  'Access-Control-Allow-Origin': 'null',
  'Access-Control-Allow-Methods': 'PUT',
  'Access-Control-Allow-Headers': 'Content-Type',
};

async function route(req, res) {
  const url = new URL(req.url, 'http://localhost');
  const key = `${req.method} ${url.pathname}`;
  if (key === 'OPTIONS /api/import') return send(res, 204, '', 'text/plain', FILE_ORIGIN_CORS);
  if (key === 'PUT /api/import') {
    const text = await readBody(req);
    const count = checkLibrary(text);
    writeAtomic(FROM_BROWSER, text);
    logWrite(FROM_BROWSER, count);
    return send(res, 200, { ok: true }, undefined, FILE_ORIGIN_CORS);
  }
  if (key === 'GET /api/library') return getLibrary(req, res, url);
  if (key === 'PUT /api/library') return putLibrary(req, res);
  if (key === 'PUT /api/conflict') return putCopy(req, res, CONFLICT, true);
  if (key === 'PUT /api/header') return putCopy(req, res, HEADER, false);
  if (url.pathname.startsWith('/api/')) throw new HttpError(404, 'Unknown API route.');
  if (req.method !== 'GET' && req.method !== 'HEAD') throw new HttpError(405, 'Method not allowed.');
  return serveStatic(req, res, url);
}

const server = http.createServer((req, res) => {
  route(req, res).catch((err) => {
    if (!(err instanceof HttpError)) console.error('[font-editor] request failed', err);
    const status = err instanceof HttpError ? err.status : 500;
    send(res, status, { error: err.message, ...(err.extra || {}) });
  });
});

const URL_ROOT = `http://127.0.0.1:${PORT}/`;

function openEditor() {
  if (process.platform !== 'darwin' || process.argv.includes('--no-open')) return;
  execFile('open', [URL_ROOT], (err) => { if (err) console.error('[font-editor] open failed', err); });
}

// A second launch finds the port taken; it opens the running editor and exits.
server.on('error', (err) => {
  if (err.code !== 'EADDRINUSE') throw err;
  console.log(`Font editor already running at ${URL_ROOT}`);
  openEditor();
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`Font editor at ${URL_ROOT}`);
  openEditor();
});
