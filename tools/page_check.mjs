#!/usr/bin/env node
// Hand check of the Mac app page, spec 2026-10-15 "Verification" item 3, run in headless Chrome.
// Builds the app, runs it on the fake box, drives the page over the DevTools protocol and
// asserts each spec behaviour. Drags go through Chrome's real drag pipeline.
//
//   node tools/page_check.mjs [outDir]      screenshots land in outDir (default build/page-check)
//
// Needs Google Chrome and Go. Never opens a window.

import { spawn, execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const out = resolve(process.argv[2] || `${root}/build/page-check`);
const chrome = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const appPort = 48611;
const devPort = 9333;
const appUrl = `http://127.0.0.1:${appPort}/`;
mkdirSync(out, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const children = [];
const kill = (p) => { if (p && p.exitCode === null) p.kill('SIGTERM'); };
process.on('exit', () => children.forEach(kill));

let failures = 0;
function check(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failures++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
}

/* ── processes ───────────────────────────────────────── */

function build(version, bin) {
  execFileSync('go', ['build', '-ldflags', `-X main.version=${version}`, '-o', bin, '.'], { cwd: `${root}/app`, stdio: 'inherit' });
}

async function startApp(bin, ...flags) {
  const p = spawn(bin, ['--no-open', '--port', String(appPort), ...flags], { stdio: 'ignore' });
  children.push(p);
  for (let i = 0; i < 50; i++) {
    await sleep(100);
    try { await fetch(`${appUrl}api/state`); return p; } catch { /* not up yet */ }
  }
  throw new Error('app did not come up');
}

async function stopApp(p) {
  kill(p);
  await new Promise((r) => p.on('exit', r));
}

async function startChrome() {
  const p = spawn(chrome, [
    '--headless=new', `--remote-debugging-port=${devPort}`, `--user-data-dir=${out}/profile`,
    '--window-size=1280,900', '--no-first-run', 'about:blank',
  ], { stdio: 'ignore' });
  children.push(p);
  for (let i = 0; i < 50; i++) {
    await sleep(200);
    try {
      const list = await (await fetch(`http://127.0.0.1:${devPort}/json`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
  }
  throw new Error('chrome did not come up');
}

/* ── devtools protocol ───────────────────────────────── */

const ws = new WebSocket(await startChrome());
await new Promise((r) => (ws.onopen = r));
let seq = 0;
const pending = new Map();
const listeners = [];
ws.onmessage = (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  else if (msg.method) listeners.forEach((l) => l(msg));
};
const send = (method, params = {}) => new Promise((res, rej) => {
  const n = ++seq;
  pending.set(n, (m) => (m.error ? rej(new Error(`${method}: ${m.error.message}`)) : res(m.result)));
  ws.send(JSON.stringify({ id: n, method, params }));
});
const once = (method) => new Promise((res) => listeners.push((m) => m.method === method && res(m.params)));

const pageErrors = [];
listeners.push((m) => {
  if (m.method === 'Runtime.exceptionThrown') pageErrors.push(m.params.exceptionDetails.exception?.description);
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') pageErrors.push(m.params.args.map((a) => a.value || a.description).join(' '));
});

async function evalJs(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
  return r.result.value;
}
async function shot(name) {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(`${out}/${name}.png`, Buffer.from(r.data, 'base64'));
}
const center = (sel) => evalJs(`(() => { const r = document.querySelector('${sel}').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
const mouse = (type, x, y, extra = {}) => send('Input.dispatchMouseEvent', { type, x, y, button: 'left', ...extra });
async function click(sel) {
  const [x, y] = await center(sel);
  await mouse('mouseMoved', x, y);
  await mouse('mousePressed', x, y, { clickCount: 1 });
  await mouse('mouseReleased', x, y, { clickCount: 1 });
}
async function key(k, vk) {
  for (const type of ['keyDown', 'keyUp']) await send('Input.dispatchKeyEvent', { type, key: k, code: k, windowsVirtualKeyCode: vk });
  await sleep(100);
}

// Real drag: Chrome intercepts the drag it would start, we deliver it to the target.
async function drag(fromSel, toSel) {
  const [sx, sy] = await center(fromSel);
  const [tx, ty] = await center(toSel);
  const intercepted = once('Input.dragIntercepted');
  await mouse('mouseMoved', sx, sy);
  await mouse('mousePressed', sx, sy, { clickCount: 1 });
  await mouse('mouseMoved', sx + 10, sy + 10);
  await mouse('mouseMoved', tx, ty);
  const { data } = await intercepted;
  for (const type of ['dragEnter', 'dragOver', 'drop']) await send('Input.dispatchDragEvent', { type, x: tx, y: ty, data });
  await mouse('mouseReleased', tx, ty, { clickCount: 1 });
  await sleep(150);
}

/* ── page readers ────────────────────────────────────── */

const slots = () => evalJs(`[...document.querySelectorAll('.slot')].map(s => s.classList.contains('is-empty') ? 'empty' : s.querySelector('.card-name').textContent)`);
const used = () => evalJs(`[...document.querySelectorAll('.card.is-used')].map(c => c.dataset.key)`);
const meter = () => evalJs(`({ text: document.getElementById('meter-text').textContent, over: document.getElementById('meter').classList.contains('is-over'), pushDisabled: document.getElementById('push').disabled, note: document.getElementById('push-note').textContent })`);
const bar = () => evalJs(`document.getElementById('bar').innerText.replace(/\\s+/g, ' ').trim()`);
const live = () => evalJs(`document.getElementById('live').textContent`);
const count = (sel) => evalJs(`document.querySelectorAll('${sel}').length`);
const empties = (list) => list.filter((s) => s === 'empty').length;

/* ── checks ──────────────────────────────────────────── */

const bin = `${out}/cubevox-app`;
build('dev', bin);
let app = await startApp(bin, '--fake');

await send('Page.enable');
await send('Runtime.enable');
await send('Input.setInterceptDrags', { enabled: true });
await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url: appUrl });
await once('Page.loadEventFired');
await sleep(2500);

// 1. initial render on the fixture
const initial = await slots();
check('eight slots, all filled by the fixture', [initial.length, empties(initial)], [8, 0]);
check('placed cards greyed in the bank', (await used()).length, 8);
check('meter under the limit, push enabled', { over: (await meter()).over, pushDisabled: (await meter()).pushDisabled }, { over: false, pushDisabled: false });
check('bar shows connected box', (await bar()).startsWith('Box connected firmware'), true);
await shot('01-initial');

// 2. slot card back to the bank empties the slot
await drag('[data-key="slot:6"]', '#bank');
check('slot 7 emptied by drag to bank', (await slots())[6], 'empty');

// 3. reverb replace: dropping the other reverb empties the placed one
const reverbSlot = initial.indexOf('Spring Reverb');
await drag('[data-key="bank:reverb_chasm"]', '[data-key="slot:6"]');
check('chasm placed in slot 7', (await slots())[6], 'Chasm Reverb');
check('spring reverb slot emptied', (await slots())[reverbSlot], 'empty');
check('chasm greyed, spring free', [(await used()).includes('bank:reverb_chasm'), (await used()).includes('bank:reverb_spring')], [true, false]);
await shot('02-reverb-replace');

// 4. drop on a filled slot swaps
const before = await slots();
await drag('[data-key="slot:1"]', '[data-key="slot:2"]');
check('slots 2 and 3 swapped', (await slots()).slice(1, 3), [before[2], before[1]]);
check('greyed card not draggable', await evalJs(`document.querySelector('.card.is-used').draggable`), false);

// 5. push error shows the box's words and marks the slots that differ
await drag('[data-key="slot:6"]', '#bank');
await drag('[data-key="bank:gate"]', '[data-key="slot:6"]');
await drag('[data-key="bank:reverb_spring"]', `[data-key="slot:${reverbSlot}"]`);
await evalJs(`window.__f = window.fetch; window.fetch = (u, i) => (String(u).endsWith('/api/layout') ? Promise.resolve(new Response(JSON.stringify({ ok: false, error: 'two reverb cards' }), { headers: { 'Content-Type': 'application/json' } })) : window.__f(u, i))`);
await click('#push');
await sleep(400);
check('push error note in the box\'s words', (await meter()).note, 'two reverb cards');
check('two slots marked as differing from the box', await count('.slot.is-differs'), 2);
await shot('03-push-error');

// 6. real push, then read-back
await evalJs(`window.fetch = window.__f`);
await click('#push');
await sleep(800);
check('push ok note', (await meter()).note, 'Pushed. The box has this layout.');
const pushed = await slots();
await sleep(1500);
check('read-back survives the next poll', await slots(), pushed);
check('no slot marked as differing', await count('.slot.is-differs'), 0);
await shot('04-pushed');

// 7. keyboard: Backspace empties, Enter picks, Enter places, Escape cancels
await evalJs(`document.querySelector('[data-key="slot:6"]').focus()`);
await key('Backspace', 8);
check('backspace empties slot 7', (await slots())[6], 'empty');
await evalJs(`document.querySelector('[data-key="bank:gate"]').focus()`);
await key('Enter', 13);
check('enter arms every slot', await count('.slot.is-armed'), 8);
check('pick announced', (await live()).startsWith('Gate picked up.'), true);
await evalJs(`document.querySelector('[data-key="slot:6"]').focus()`);
await key('Enter', 13);
check('enter places the pick', (await slots())[6], 'Gate');
check('place announced', await live(), 'Gate placed in slot 7');
await evalJs(`document.querySelector('[data-key="bank:reverb_chasm"]').focus()`);
await key('Enter', 13);
await key('Escape', 27);
check('escape clears the pick', await count('.slot.is-armed'), 0);

// 8. over the limit: the fixture cards cannot exceed 100 % in eight slots, so lower the limits
await evalJs(`window.fetch = async (u, i) => { const r = await window.__f(u, i); if (!String(u).endsWith('/api/state')) return r; const j = await r.json(); j.limits = { mean: 40, worst: 50 }; return new Response(JSON.stringify(j), { headers: { 'Content-Type': 'application/json' } }); }`);
await sleep(1500);
const over = await meter();
check('meter red and push disabled over the limit', [over.over, over.pushDisabled, over.text.endsWith('over the limit')], [true, true, true]);
check('over-limit note', over.note, 'Over the limit. Remove an effect to push.');
check('note is an alert', await evalJs(`document.getElementById('push-note').getAttribute('role')`), 'alert');
await shot('05-meter-over');
await evalJs(`window.fetch = window.__f`);
await sleep(1500);
check('meter back under the limit', (await meter()).over, false);

check('no page errors', pageErrors, []);

// 9. bar states: app gone, app without a box, older app offered an update
// The page logs each failed poll while the app is down; those are the expected errors here.
await stopApp(app);
await sleep(2500);
check('bar after the app exits', (await bar()).startsWith('Lost contact with the app'), true);
await shot('06-lost-contact');

app = await startApp(bin);
await sleep(2500);
check('bar without a box', (await bar()).startsWith('Connect the box'), true);
check('push disabled without a box', [(await meter()).pushDisabled, (await meter()).note], [true, 'Connect the box to push']);
await shot('07-no-box');
await stopApp(app);

const oldBin = `${out}/cubevox-app-old`;
build('v0.00.01', oldBin);
app = await startApp(oldBin, '--fake');
await sleep(4000);
check('older app offers the update', /Update to v\d/.test(await bar()), true);
await shot('08-update-offered');
await stopApp(app);

check('no page errors beyond the polls while the app was down', pageErrors.filter((e) => !e.startsWith('state poll failed')), []);
ws.close();
console.log(failures ? `${failures} check(s) failed` : 'all page checks passed');
process.exit(failures ? 1 : 0);
