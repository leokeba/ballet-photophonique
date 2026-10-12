// End to end (docs/DESIGN.md section 10): a virtual MIDI keyboard
// (keyboard.py) → Chromium with Web MIDI, on the launcher page with x-midi
// (served by serve.py on localhost, a secure context, /api proxied to the
// controller: HTTP only, as the Pages launcher reaches it by address) → the
// controller. Checks the outputs through the API, then edits the key map
// through the element. Maps keys 60, 62, 64, 67 and 69 to outputs 1 to 5
// (67 at 1000 Hz), and leaves them so; outputs turn on and off.
//
//   python3 test/midi/serve.py 8130 http://<ip> pages/index.html \
//       ../TesserUI/web/dist/ui.js web/elements/midi.js &
//   node test/midi/e2e_midi.mjs http://<ip> 'http://localhost:8130/?api=/api&ws=' shot.png [--headed]
//
// Playwright comes from TesserUI's web/ (TESSERUI_DIR, default ../TesserUI);
// python-rtmidi through uv.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { createInterface } from 'node:readline';

const tesserui = process.env.TESSERUI_DIR ?? new URL('../../../TesserUI', import.meta.url).pathname;
const require = createRequire(`${tesserui}/web/package.json`);
const { chromium } = require('@playwright/test');
const [node, pageUrl, shot] = process.argv.slice(2);
const headed = process.argv.includes('--headed');
const dir = new URL('.', import.meta.url).pathname;

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};
const get = async (p) => (await fetch(`${node}/api${p}`)).json();
const put = (p, v) => fetch(`${node}/api${p}`, { method: 'PUT', body: JSON.stringify(v) });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const near = (a, b, tol) => Math.abs(a - b) <= tol;

// The test's key map.
const map = Array(128).fill(0);
[60, 62, 64, 67, 69].forEach((n, i) => (map[n] = i + 1));
await put('/midi/keys/outputs', map);
await put('/midi/keys/frequencies/67', 1000);

// The keyboard first, so the page finds it.
const kb = spawn('uv', ['run', '-q', '--with', 'python-rtmidi', 'python', `${dir}keyboard.py`], { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = createInterface({ input: kb.stdout });
const next = () => new Promise((r) => lines.once('line', r));
if ((await next()) !== 'ready') throw new Error('no virtual keyboard');
const play = async (cmd) => {
  kb.stdin.write(`${cmd}\n`);
  await next();
};

const browser = await chromium.launch({ headless: !headed });
const context = await browser.newContext();
await context.grantPermissions(['midi', 'midi-sysex'], { origin: new URL(pageUrl).origin });
// The Pages launcher reaching a controller by address: Chrome's Local Network
// Access, where this Playwright knows it.
await context.grantPermissions(['local-network-access'], { origin: new URL(pageUrl).origin }).catch(() => {});
const page = await context.newPage();
page.on('console', (m) => m.type() === 'error' && console.log('  console:', m.text()));
await page.goto(pageUrl);
const el = page.locator('x-midi');
await el.waitFor({ timeout: 20000 });
const status = el.locator('.status');
await page.waitForFunction(() => document.querySelector('body') !== null);
try {
  await status.filter({ hasText: 'Playing from' }).waitFor({ timeout: 10000 });
} catch {}
const text = await status.textContent();
check(/Photophonique Test/.test(text), `the element plays from the virtual keyboard ("${text}")`);
const options = await el.locator('select.inputs option').allTextContents();
console.log('  inputs:', options.join(', '));

// Notes.
await put('/midi/bend/target', 'frequency');
await play('on 60 100');
await sleep(400);
let o1 = await get('/outputs/1');
check(o1.on && near(o1.frequency, 261.6256, 1e-3) && near(o1.duty, (100 * 100) / 127, 1e-3), `C4 → output 1 on at ${o1.frequency} Hz, ${o1.duty} %`);
await play('on 67 64');
await sleep(400);
let o4 = await get('/outputs/4');
check(o4.on && near(o4.frequency, 1000, 1e-6), `G4 → output 4 at its own 1000 Hz (${o4.frequency})`);
await play('bend 16383');
await sleep(400);
o1 = await get('/outputs/1');
o4 = await get('/outputs/4');
check(near(o1.frequency, 261.6256 * 2 ** (2 / 12), 1e-2) && near(o4.frequency, 1000 * 2 ** (2 / 12), 1e-2), `full bend: +2 semitones on both (${o1.frequency}, ${o4.frequency})`);
// A burst: the element batches what comes while a request is out.
for (const n of [62, 64, 69]) await play(`on ${n} 90`);
await sleep(600);
const voices = await get('/midi/keys/voices');
check(voices[1] === 62 && voices[2] === 64 && voices[4] === 69, `a burst of three notes arrives (voices ${JSON.stringify(voices)})`);
await sleep(1500); // the page polls once a second
const sounding = await el.locator('g.k.sounds').count();
check(sounding >= 4, `the element shows the sounding keys (${sounding})`);
await page.locator('x-midi').screenshot({ path: shot });
await play('bend 8192');
await play('off 60');
await sleep(400);
o1 = await get('/outputs/1');
check(!o1.on, 'C4 released: output 1 off');
await play('cc 123 0');
await sleep(400);
const on = await Promise.all([2, 3, 4, 5].map(async (n) => (await get(`/outputs/${n}/on`))));
check(on.every((v) => v === false), `all notes off: ${JSON.stringify(on)}`);

// Editing the map through the element.
await el.locator('g.k[data-note="72"]').click();
await el.locator('.outs button[data-output="2"]').click();
await sleep(1000);
let outs = await get('/midi/keys/outputs');
check(outs[72] === 2, `C5 assigned to output 2 by a click (${outs[72]})`);
await el.locator('g.k[data-note="74"]').click({ modifiers: ['Shift'] });
await el.locator('.outs button[data-output="5"]').click();
await sleep(1000);
outs = await get('/midi/keys/outputs');
check([72, 73, 74].every((n) => outs[n] === 5), `C5–D5 assigned to output 5 by a shift-click range (${outs.slice(72, 75)})`);
await el.locator('g.k[data-note="72"]').click();
await el.locator('input.freq').fill('555.5');
await el.locator('input.freq').press('Enter');
await sleep(1000);
let f = await get('/midi/keys/frequencies/72');
check(f === 555.5, `C5's frequency overridden to 555.5 Hz (${f})`);
await el.locator('button.reset').click();
await sleep(1000);
f = await get('/midi/keys/frequencies/72');
check(f === 0, `back to the note's own (${f})`);
// Leave the test keys unmapped.
await el.locator('g.k[data-note="72"]').click();
await el.locator('g.k[data-note="74"]').click({ modifiers: ['Shift'] });
await el.locator('.outs button[data-output="0"]').click();
await sleep(800);

await browser.close();
kb.stdin.end();
console.log(`DONE fail=${failures}`);
process.exit(failures ? 1 : 0);
