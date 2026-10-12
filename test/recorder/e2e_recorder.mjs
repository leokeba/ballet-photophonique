// End to end (docs/DESIGN.md section 10): x-recorder in Chromium, on the
// launcher page (served by serve.py, or serial_bridge.py over USB), driving
// the piano roll with the mouse and keys and checking the controller through
// its API. Makes a recording "E2E" and deletes it at the end; uses unmapped
// keys (50–58 must map to no output), so no output turns on.
//
//   node test/recorder/e2e_recorder.mjs http://<node> 'http://localhost:8131/?api=/api&ws=' shot.png [--headed]
//
// Playwright comes from TesserUI's web/ (TESSERUI_DIR, default ../TesserUI).
import { createRequire } from 'node:module';

const tesserui = process.env.TESSERUI_DIR ?? new URL('../../../TesserUI', import.meta.url).pathname;
const require = createRequire(`${tesserui}/web/package.json`);
const { chromium } = require('@playwright/test');
const [node, pageUrl, shot] = process.argv.slice(2);
const headed = process.argv.includes('--headed');

let failures = 0;
const check = (ok, what, extra = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${what}${ok ? '' : ` ${extra}`}`);
  if (!ok) failures++;
};
const api = async (p, v) => {
  const r = await fetch(`${node}/api${p}`, v === undefined ? {} : { method: 'PUT', body: JSON.stringify(v) });
  return r.json();
};
const Z = 0.15; // the zoom, px per ms
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const notes = async () => (await api('/recorder/notes')).map(({ t, d, n, v }) => ({ t, d, n, v }));
// Waits for the controller to show `want` (polled pages lag).
const until = async (fn, ms = 6000) => {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v || Date.now() > end) return v;
    await sleep(200);
  }
};

const outputs = await api('/midi/keys/outputs');
if (outputs.slice(50, 59).some((o) => o)) {
  console.log('keys 50–58 are mapped to outputs: unmap them first');
  process.exit(2);
}
for (const name of ['E2E', 'E2E renamed']) await api(`/recorder/recordings/${encodeURIComponent(name)}/delete`, null);
await api('/recorder/new', 'E2E');
await api('/recorder/length', 1500); // its end on screen

const browser = await chromium.launch({ headless: !headed });
const page = await browser.newPage({ viewport: { width: 1400, height: 1000 } });
page.on('pageerror', (e) => check(false, 'no page error', e.message));
page.on('dialog', (d) => d.accept(d.type() === 'prompt' ? 'E2E renamed' : undefined));
await page.addInitScript(() => {
  localStorage.setItem('x-recorder:zoom', '0.15'); // 150 px a second: the end in a narrow card
  localStorage.setItem('x-recorder:grid', '0.25'); // 1/16: 125 ms at 120 BPM
});
await page.goto(pageUrl);
await page.getByText('Record', { exact: true }).first().click();
const rec = page.locator('x-recorder');
await rec.waitFor();
await until(async () => (await rec.evaluate((e) => e.value.recording)) === 'E2E');
check((await rec.evaluate((e) => e.value.recording)) === 'E2E', 'the element shows the new recording');

const main = rec.locator('svg.main');
const box = async () => main.boundingBox();
// The roll's geometry, read from what it draws: a row is 9 px; a C's label
// in the pitch column gives that row's place.
const rowOf = async (n) => {
  const label = rec.locator('svg.keys text', { hasText: /^C\d$/ }).first();
  const text = await label.textContent();
  const c = (Number(text.slice(1)) + 1) * 12;
  const y = Number(await label.getAttribute('y')) - 7.5; // the label's baseline, near the row's bottom
  return y + (c - n) * 9 + 4.5;
};
const at = async (ms, n) => {
  const b = await box();
  return { x: b.x + ms * Z, y: b.y + (await rowOf(n)) };
};

// A double click adds a note on the grid, a grid step long (an empty
// recording shows C3 to C5).
let p = await at(1010, 53);
await page.mouse.dblclick(p.x, p.y);
let got = await until(async () => {
  const ns = await notes();
  return ns.length === 1 && ns[0];
});
check(got && got.t === 1000 && got.n === 53 && got.d === 125 && got.v === 100, 'double-click adds a note', JSON.stringify(got));
await until(async () => (await rec.evaluate((e) => e.value.notes[0]?.id)) > 0);

// Dragging it moves it: +250 ms, two semitones up.
p = await at(1060, 53);
await page.mouse.move(p.x, p.y);
await page.mouse.down();
await page.mouse.move(p.x + 125 * Z, p.y - 9, { steps: 5 });
await page.mouse.move(p.x + 250 * Z, p.y - 18, { steps: 5 });
await page.mouse.up();
got = await until(async () => (await notes()).find((x) => x.t === 1250 && x.n === 55));
check(!!got, 'dragging moves it on the grid', JSON.stringify(await notes()));

// Its right edge resizes it.
await until(async () => (await rec.evaluate((e) => e.value.notes[0]?.t)) === 1250);
p = await at(1250 + 125 - 2, 55);
await page.mouse.move(p.x, p.y);
await page.mouse.down();
await page.mouse.move(p.x + 250 * Z, p.y, { steps: 5 });
await page.mouse.up();
got = await until(async () => (await notes()).find((x) => x.d === 375));
await until(async () => (await rec.evaluate((e) => e.value.notes[0]?.d)) === 375);
check(!!got, 'its edge resizes it', JSON.stringify(await notes()));

// Velocity of the selection.
await rec.locator('.vel').fill('40');
await rec.locator('.vel').press('Enter');
got = await until(async () => (await notes()).find((x) => x.v === 40));
check(!!got, 'the velocity field sets the selection', JSON.stringify(await notes()));

// Delete removes the selection.
p = await at(1300, 55);
await page.mouse.click(p.x, p.y);
await page.keyboard.press('Delete');
got = await until(async () => (await notes()).length === 0);
check(got, 'Delete removes it');

// Quantize the selection, then undo.
await api('/recorder/edit', { add: [{ t: 130, d: 240, n: 50 }, { t: 610, d: 100, n: 53 }, { t: 990, d: 30, n: 57 }] });
await until(async () => (await rec.evaluate((e) => e.value.notes.length)) === 3);
await rec.locator('.roll').focus();
await page.keyboard.press('ControlOrMeta+a');
await rec.locator('.quantize').click();
got = await until(async () => (await notes()).map((x) => x.t).join() === '125,625,1000');
check(got, 'Quantize snaps the selection', JSON.stringify(await notes()));
await until(async () => (await rec.evaluate((e) => e.value.notes.map((x) => x.t).join())) === '125,625,1000');
await rec.locator('.roll').focus();
await page.keyboard.press('ControlOrMeta+z');
got = await until(async () => (await notes()).map((x) => x.t).join() === '130,610,990');
check(got, 'Ctrl/Cmd+Z undoes it', JSON.stringify(await notes()));

// Arrows move the selection.
// Notes added by the undo are selectable once the controller gave their ids.
await until(async () => {
  const ns = await rec.evaluate((e) => e.value.notes);
  return ns.map((x) => x.t).join() === '130,610,990' && ns.every((x) => x.id > 0);
});
p = await at(140, 50);
await page.mouse.click(p.x, p.y);
await page.keyboard.press('ArrowUp');
got = await until(async () => (await notes()).some((x) => x.t === 130 && x.n === 51));
check(got, 'an arrow moves it a semitone', JSON.stringify(await notes()));

// A rectangle selects; then Delete.
const b = await box();
await page.mouse.move(b.x + 500 * Z, b.y + (await rowOf(54)));
await page.mouse.down();
await page.mouse.move(b.x + 650 * Z, b.y + (await rowOf(52)), { steps: 6 });
await page.mouse.up();
await page.keyboard.press('Delete');
got = await until(async () => (await notes()).length === 2);
check(got, 'a rectangle selects (the note at 610)', JSON.stringify(await notes()));

// The bend lane: a double click adds a point above the centre.
const lane = Number(await rec.locator('svg.main .lane').getAttribute('y'));
await page.mouse.dblclick(b.x + 300 * Z + 1, b.y + lane + 10);
got = await until(async () => {
  const bs = await api('/recorder/bends');
  return bs.length === 1 && bs[0];
});
check(got && got.t === 250 && got.v > 4000, 'the bend lane adds a point', JSON.stringify(got));

// The end's handle sets the length.
await page.mouse.move(b.x + 1500 * Z, b.y + 10);
await page.mouse.down();
await page.mouse.move(b.x + 2000 * Z, b.y + 10, { steps: 8 });
await page.mouse.up();
got = await until(async () => (await api('/recorder/length')) === 2000);
check(got, 'the handle sets the length', String(await api('/recorder/length')));

// Play: replay runs (unmapped keys), then stops at the end.
await rec.locator('.play').click();
got = await until(async () => (await api('/recorder/state')) === 'playing', 3000);
check(got, 'Play plays');
got = await until(async () => (await api('/recorder/state')) === 'stopped', 5000);
check(got, 'and stops at the end');
await page.screenshot({ path: shot, fullPage: true });

// A take from the computer's keys (x-midi, an octave down: A, S and D play
// 48, 50 and 52), with their timing.
await api('/recorder/overdub', false);
await rec.locator('.rec').click();
got = await until(async () => (await api('/recorder/state')) === 'armed');
check(got, 'Record arms a new take');
const take = await api('/recorder/recording');
await page.locator('body').click({ position: { x: 5, y: 5 } }); // the keys go to the page, not a field
await page.keyboard.press('KeyZ');
await page.keyboard.down('KeyA');
await sleep(400);
await page.keyboard.up('KeyA');
await sleep(200);
await page.keyboard.down('KeyS');
await page.keyboard.down('KeyD');
await sleep(300);
await page.keyboard.up('KeyS');
await page.keyboard.up('KeyD');
await page.keyboard.press('KeyX');
await sleep(300);
await rec.locator('.stop').click();
got = await until(async () => (await api('/recorder/state')) === 'stopped');
const played = await notes();
console.log('  the take:', JSON.stringify(played));
// Within a batch, the spacing is the keys' (S and D pressed together); from
// one request to the next, the transport's latency adds (over the serial
// bridge, where the page's polls share the line, up to hundreds of ms).
check(
  played.length === 3 && played[0].n === 48 && played[0].t === 0 && played[0].d >= 380 &&
    played[1].n === 50 && played[2].n === 52 && Math.abs(played[2].t - played[1].t) < 30 &&
    Math.abs(played[1].d - 300) < 60 && Math.abs(played[2].d - 300) < 60,
  'the computer keys record with their timing',
  JSON.stringify(played),
);
await api(`/recorder/recordings/${encodeURIComponent(take)}/delete`, null);
await api('/recorder/recording', 'E2E');
await until(async () => (await rec.evaluate((e) => e.value.recording)) === 'E2E');

// Rename and delete, through the dialogs.
await rec.locator('.rename').click();
got = await until(async () => (await api('/recorder/recording')) === 'E2E renamed');
check(got, 'Rename renames it');
await until(async () => (await rec.evaluate((e) => e.value.recording)) === 'E2E renamed');
await rec.locator('.delete').click();
got = await until(async () => (await api('/recorder/recordings')).every((r) => !r.name.startsWith('E2E')));
check(got, 'Delete deletes it');

await browser.close();
console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
