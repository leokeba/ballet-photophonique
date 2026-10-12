// Playing from the computer's keys (docs/DESIGN.md section 7), on the
// controller's own page: plain http://, so no Web MIDI there, and the
// element from the controller's flash. Maps keys 60, 61 and 72 to outputs 1,
// 2 and 3 for the test, then unmaps 61 and 72; outputs turn on and off.
//
//   node test/midi/e2e_typing.mjs http://<ip> shot.png
//
// Playwright comes from TesserUI's web/ (TESSERUI_DIR, default ../TesserUI).
import { createRequire } from 'node:module';

const tesserui = process.env.TESSERUI_DIR ?? new URL('../../../TesserUI', import.meta.url).pathname;
const require = createRequire(`${tesserui}/web/package.json`);
const { chromium } = require('@playwright/test');
const [node, shot] = process.argv.slice(2);

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`);
  if (!ok) failures++;
};
const get = async (p) => (await fetch(`${node}/api${p}`)).json();
const put = (p, v) => fetch(`${node}/api${p}`, { method: 'PUT', body: JSON.stringify(v) });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const near = (a, b, tol) => Math.abs(a - b) <= tol;
const out = (n) => get(`/outputs/${n}`);

const map = await get('/midi/keys/outputs');
map[60] = 1;
map[61] = 2;
map[72] = 3;
await put('/midi/keys/outputs', map);
await put('/outputs/off', null);

const browser = await chromium.launch();
const page = await (await browser.newContext({ viewport: { width: 1200, height: 900 } })).newPage();
page.on('pageerror', (e) => console.log('  page error:', e.message));
await page.goto(`${node}/`);
const el = page.locator('x-midi');
await el.waitFor({ timeout: 20000 });
await sleep(1500);
check(!(await page.evaluate(() => window.isSecureContext)), 'the page is not a secure context');
const status = await el.locator('.status').textContent();
check(/computer's keys/.test(status), `the element says to play with the computer's keys ("${status}")`);
check((await el.locator('button.typekeys').getAttribute('aria-pressed')) === 'true', 'computer keys on by default, with no MIDI input');
check((await el.locator('g.k[data-note="60"] .kbd').textContent()) === 'A', 'C4 labelled A');
console.log('  info:', (await el.locator('.typinfo').innerText()).replace(/\s+/g, ' '));

const kb = page.keyboard;
await kb.down('a');
await sleep(500);
let o = await out(1);
check(o.on && near(o.frequency, 261.6256, 1e-3) && near(o.duty, (100 * 100) / 127, 1e-3), `A plays C4 on output 1 at velocity 100 (${o.frequency} Hz, ${o.duty} %)`);
await kb.up('a');
await sleep(500);
check(!(await out(1)).on, 'releasing A turns it off');

await kb.press('c'); // velocity 100 → 80
await kb.down('w');
await sleep(500);
o = await out(2);
check(o.on && near(o.duty, (80 * 100) / 127, 1e-3), `C lowers the velocity: W plays C♯4 at 80 (${o.duty} %)`);
await kb.up('w');

await kb.press('x'); // octave up: A is C5
await kb.down('a');
await sleep(500);
o = await out(3);
check(o.on && near(o.frequency, 523.2511, 1e-3), `X raises the octave: A plays C5 on output 3 (${o.frequency} Hz)`);
await kb.press('z'); // octave down while A is held
await kb.up('a');
await sleep(500);
check(!(await out(3)).on, "releasing A after Z still releases C5, the note its key-down played");
const shownA = await el.locator('g.k[data-note="60"] .kbd').textContent();
check(shownA === 'A', `the labels follow the octave (C4: "${shownA}")`);

// Typing in a field plays nothing.
await el.locator('g.k[data-note="60"]').click();
await el.locator('input.freq').click();
await kb.down('a');
await sleep(400);
check(!(await out(1)).on, 'A typed in the frequency field plays nothing');
await kb.up('a');
await page.locator('body').click({ position: { x: 1100, y: 850 } });

// Shortcuts pass through.
await kb.down('Control');
await kb.down('a');
await sleep(300);
check(!(await out(1)).on, 'Ctrl+A plays nothing');
await kb.up('a');
await kb.up('Control');

// A key held when the window loses focus is released.
await kb.down('a');
await sleep(400);
check((await out(1)).on, 'A held');
await page.evaluate(() => window.dispatchEvent(new Event('blur')));
await sleep(500);
check(!(await out(1)).on, 'losing focus releases it');
await kb.up('a');

await page.locator('x-midi').screenshot({ path: shot });

// Off by the button.
await el.locator('button.typekeys').click();
await kb.down('a');
await sleep(400);
check(!(await out(1)).on, 'with Computer keys off, A plays nothing');
await kb.up('a');
check((await el.locator('g.k[data-note="60"] .kbd').isVisible()) === false, 'and the labels go');

await browser.close();
map[61] = 0;
map[72] = 0;
await put('/midi/keys/outputs', map);
console.log(`DONE fail=${failures}`);
process.exit(failures ? 1 : 0);
