// <x-recorder>: the recorder (docs/DESIGN.md sections 5.6 and 7). It draws
// /recorder, {state, position, recording, loop, overdub, length, bpm, beats,
// notes[], bends[], recordings[]}, as:
//
// - a transport: the recording to work on, New, Rename, Delete; Record,
//   Play, Stop, Loop, Overdub; the position, run on the browser's clock
//   between the controller's announcements;
// - a piano roll: notes coloured by their key's output (/midi/keys/outputs),
//   a beat and bar grid at the recording's tempo, its length as a handle, a
//   playhead, and a pitch bend lane under the notes.
//
// Editing: click a note (shift-click adds to the selection), drag on empty
// space to select a rectangle, Ctrl or Cmd+A selects all; drag notes to move
// them (on the grid unless Alt is held), drag a note's right edge to resize;
// double-click empty space for a note, a note to delete it; Delete removes
// the selection, the arrows move it (Shift: an octave). The bend lane: drag
// its points, double-click to add or delete one. Ctrl or Cmd+Z undoes. Each
// gesture is one /recorder/edit (split under the request limit), shown at
// once and reconciled with what the controller announces.

const COLORS = ['#e4572e', '#f3a712', '#29bf12', '#00a5cf', '#9b5de5', '#ef476f', '#118ab2', '#8d6e63'];
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const BLACK = new Set([1, 3, 6, 8, 10]);
// Grid steps in beats (a beat is a quarter note).
const GRIDS = [
  ['Off', 0],
  ['1/4', 1],
  ['1/8', 1 / 2],
  ['1/8T', 1 / 3],
  ['1/16', 1 / 4],
  ['1/16T', 1 / 6],
  ['1/32', 1 / 8],
];
const RULER = 20;
const ROW = 9;
const LANE = 64;
const GAP = 6;
const KEYS = 40; // the pitch column's width
const MAX_BODY = 3500; // under TesserAPI's 4 KB request limit
const STORE_GRID = 'x-recorder:grid';
const STORE_ZOOM = 'x-recorder:zoom';

const noteName = (n) => `${NAMES[n % 12]}${Math.floor(n / 12) - 1}`;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const time = (ms) => {
  ms = Math.max(0, Math.round(ms));
  const m = Math.floor(ms / 60000);
  const s = ((ms % 60000) / 1000).toFixed(3).padStart(6, '0');
  return `${m}:${s}`;
};
const local = (key) => ({
  get: () => {
    try {
      return localStorage.getItem(key);
    } catch {
      return null;
    }
  },
  set: (v) => {
    try {
      localStorage.setItem(key, v);
    } catch {}
  },
});
const gridChoice = local(STORE_GRID);
const zoomChoice = local(STORE_ZOOM);

class Recorder extends HTMLElement {
  #value = { notes: [], bends: [], recordings: [] };
  #schema = null;
  #disabled = false;
  #selected = new Set(); // note and bend point ids (one id space)
  #preview = null; // id → {t, d, n} or {t, v}, during a drag
  #drag = null;
  #marquee = null; // {x0, y0, x1, y1}
  #undo = []; // earlier {notes, bends}
  #zoom = 0.1; // px per ms
  #grid = 1 / 4; // beats, 0 off
  #velocity = 100;
  #outputs = []; // each note's output, from /midi/keys/outputs
  #range = [48, 84]; // pitches shown, low and high
  #base = { pos: 0, at: 0 }; // the announced position, and when it came
  #raf = 0;
  #poll = 0;
  #last = null; // the last press, {x, y, at}: a double click is two

  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).innerHTML = `
      <style>
        :host { display: block; }
        .bar { display: flex; flex-wrap: wrap; gap: 6px 8px; align-items: center; margin-bottom: 8px; }
        select, input, button { font: inherit; color: var(--text, inherit); background: var(--surface, #fff);
          border: 1px solid var(--border, #d9dde3); border-radius: 6px; padding: 3px 8px; }
        button { cursor: pointer; }
        button[aria-pressed="true"] { background: var(--accent, #2f5bd3); border-color: var(--accent, #2f5bd3); color: #fff; }
        button:disabled, input:disabled, select:disabled { opacity: .5; cursor: default; }
        .rec { color: var(--danger, #c62f2f); font-weight: 600; }
        .rec.on { background: var(--danger, #c62f2f); border-color: var(--danger, #c62f2f); color: #fff; }
        .rec.armed { animation: blink 1s steps(2) infinite; }
        @keyframes blink { 50% { opacity: .45; } }
        .sep { width: 1px; align-self: stretch; background: var(--border, #d9dde3); }
        .pos { font-variant-numeric: tabular-nums; color: var(--muted, #5d6675); min-width: 12em; }
        input[type=number] { width: 5em; }
        label { display: inline-flex; gap: 4px; align-items: center; color: var(--muted, #5d6675); }
        .roll { display: flex; border: 1px solid var(--border, #d9dde3); border-radius: 6px; overflow: hidden;
          outline: none; background: var(--surface, #fff); }
        .roll:focus-visible { border-color: var(--accent, #2f5bd3); }
        .scroll { overflow-x: auto; flex: 1; }
        svg { display: block; user-select: none; touch-action: none; }
        .keys { flex: none; border-right: 1px solid var(--border, #d9dde3); }
        .keys text { font: 8px system-ui, sans-serif; fill: var(--muted, #5d6675); }
        .blackrow { fill: var(--text, #1a1d23); opacity: .05; }
        .beat { stroke: var(--border, #d9dde3); stroke-width: .5; }
        .barline { stroke: var(--muted, #5d6675); stroke-width: .6; opacity: .5; }
        .sub { stroke: var(--border, #d9dde3); stroke-width: .5; stroke-dasharray: 1 2; }
        .ruler { fill: var(--surface-2, #eceef2); }
        .ruler text { font: 9px system-ui, sans-serif; fill: var(--muted, #5d6675); }
        .past { fill: var(--text, #1a1d23); opacity: .06; pointer-events: none; }
        .note { stroke: rgba(0,0,0,.35); stroke-width: .6; cursor: grab; }
        .note.out { opacity: .4; }
        .note.sel { stroke: var(--text, #1a1d23); stroke-width: 1.6; }
        .lane { fill: var(--surface-2, #eceef2); }
        .centre { stroke: var(--border, #d9dde3); stroke-dasharray: 3 3; }
        .bendline { fill: none; stroke: var(--accent, #2f5bd3); stroke-width: 1.4; }
        .point { fill: var(--accent, #2f5bd3); stroke: var(--surface, #fff); stroke-width: 1; cursor: grab; }
        .point.sel { stroke: var(--text, #1a1d23); stroke-width: 2; }
        .end { fill: var(--danger, #c62f2f); cursor: ew-resize; }
        .endline { stroke: var(--danger, #c62f2f); stroke-width: 1; opacity: .6; pointer-events: none; }
        .head { stroke: var(--ok, #1f9d55); stroke-width: 1.5; pointer-events: none; }
        .marquee { fill: var(--accent, #2f5bd3); fill-opacity: .1; stroke: var(--accent, #2f5bd3); stroke-dasharray: 3 2; pointer-events: none; }
        .hint { color: var(--muted, #5d6675); font-size: .85em; margin-top: 6px; }
        .empty { color: var(--muted, #5d6675); padding: 16px; }
      </style>
      <div class="bar">
        <label>Recording <select class="clip"></select></label>
        <button class="new" title="A new, empty recording">New</button>
        <button class="rename">Rename</button>
        <button class="delete">Delete</button>
        <span class="sep"></span>
        <button class="rec" title="Record: a new take, or onto this recording with Overdub on">● Record</button>
        <button class="play">▶ Play</button>
        <button class="stop">■ Stop</button>
        <button class="loop" aria-pressed="false" title="Replay starts over at the end">Loop</button>
        <button class="overdub" aria-pressed="false" title="Record onto this recording instead of a new take">Overdub</button>
        <span class="pos"></span>
      </div>
      <div class="bar">
        <label>Tempo <input class="bpm" type="number" min="20" max="300" step="0.1"> BPM</label>
        <label><input class="beats" type="number" min="1" max="16" step="1"> beats a bar</label>
        <label>Grid <select class="grid"></select></label>
        <button class="quantize" title="The selection's starts (or every note's) to the grid">Quantize</button>
        <label>Velocity <input class="vel" type="number" min="1" max="127" step="1"></label>
        <span class="sep"></span>
        <button class="zout" title="Zoom out (Ctrl or Cmd and the wheel)">−</button>
        <button class="zin" title="Zoom in">+</button>
        <button class="undo" title="Undo (Ctrl or Cmd+Z)">Undo</button>
      </div>
      <div class="roll" tabindex="0">
        <svg class="keys" width="${KEYS}"></svg>
        <div class="scroll"><svg class="main"></svg></div>
      </div>
      <div class="hint">Double-click for a note, drag to move, its right edge to resize; Delete removes, arrows move, Shift-click or drag to select. The lane under the notes is the pitch bend.</div>`;
    const $ = (s) => this.shadowRoot.querySelector(s);
    this.$ = $;
    const grid = $('.grid');
    for (const [label, beats] of GRIDS) grid.append(new Option(label, String(beats)));
    const g = gridChoice.get();
    if (g !== null && GRIDS.some(([, b]) => b === Number(g))) this.#grid = Number(g);
    grid.value = String(this.#grid);
    const z = Number(zoomChoice.get() ?? 0);
    if (z > 0) this.#zoom = clamp(z, 0.005, 2);

    $('.clip').addEventListener('change', (e) => this.#set('recording', e.target.value));
    $('.new').addEventListener('click', () => this.#create());
    $('.rename').addEventListener('click', () => this.#rename());
    $('.delete').addEventListener('click', () => this.#remove());
    $('.rec').addEventListener('click', () => this.#call('record'));
    $('.play').addEventListener('click', () => this.#call('play'));
    $('.stop').addEventListener('click', () => this.#call('stop'));
    $('.loop').addEventListener('click', () => this.#set('loop', !this.#value.loop));
    $('.overdub').addEventListener('click', () => this.#set('overdub', !this.#value.overdub));
    $('.bpm').addEventListener('change', (e) => this.#set('bpm', +e.target.value));
    $('.beats').addEventListener('change', (e) => this.#set('beats', +e.target.value));
    grid.addEventListener('change', (e) => {
      this.#grid = +e.target.value;
      gridChoice.set(String(this.#grid));
      this.#paint();
    });
    $('.quantize').addEventListener('click', () => this.#quantize());
    $('.vel').addEventListener('change', (e) => this.#setVelocity(+e.target.value));
    $('.zin').addEventListener('click', () => this.#zoomBy(1.5));
    $('.zout').addEventListener('click', () => this.#zoomBy(1 / 1.5));
    $('.undo').addEventListener('click', () => this.#undoLast());

    const main = $('.main');
    main.addEventListener('pointerdown', (e) => this.#down(e));
    main.addEventListener('pointermove', (e) => this.#move(e));
    main.addEventListener('pointerup', (e) => this.#up(e));
    main.addEventListener('pointercancel', () => this.#cancel());
    $('.scroll').addEventListener(
      'wheel',
      (e) => {
        if (!e.ctrlKey && !e.metaKey) return;
        e.preventDefault();
        this.#zoomBy(e.deltaY < 0 ? 1.25 : 0.8, e.clientX);
      },
      { passive: false },
    );
    $('.roll').addEventListener('keydown', (e) => this.#key(e));
  }

  connectedCallback() {
    this.#fetchOutputs();
    this.#poll = setInterval(() => this.#fetchOutputs(), 5000);
    this.#tick();
  }

  disconnectedCallback() {
    clearInterval(this.#poll);
    cancelAnimationFrame(this.#raf);
  }

  set value(v) {
    if (!v || typeof v !== 'object') return;
    const was = this.#value;
    this.#value = { notes: [], bends: [], recordings: [], ...v };
    if (v.position !== was.position || v.state !== was.state || v.recording !== was.recording) {
      this.#base = { pos: v.position ?? 0, at: performance.now() };
    }
    if (v.recording !== was.recording) {
      this.#selected.clear();
      this.#undo = [];
    }
    if (!this.#drag) this.#fit();
    this.#paint();
  }
  get value() {
    return this.#value;
  }
  set schema(s) {
    this.#schema = s;
  }
  set disabled(d) {
    this.#disabled = !!d;
    this.#paint();
  }
  set label(_) {}
  set error(_) {}

  // The controller.

  #call(path, arg = null) {
    if (!this.tesser) return Promise.resolve();
    return Promise.resolve(this.tesser.call(path, arg)).catch((e) => {
      this.tesser?.toast?.(`${path}: ${e?.message ?? e?.error ?? e}`, 'warn');
      throw e;
    });
  }

  #set(path, v) {
    this.tesser?.set(path, v);
  }

  #fetchOutputs() {
    try {
      const o = this.tesser?.get('/midi/keys/outputs');
      if (Array.isArray(o)) {
        this.#outputs = o;
        this.#paint();
      }
    } catch {}
  }

  #create() {
    const names = new Set((this.#value.recordings ?? []).map((r) => r.name));
    let n = 1;
    while (names.has(`Recording ${n}`)) n++;
    const name = prompt('Name of the new recording', `Recording ${n}`);
    if (name) this.#call('new', name).catch(() => {});
  }

  #rename() {
    const from = this.#value.recording;
    if (!from) return;
    const to = prompt(`Rename “${from}” to`, from);
    if (to && to !== from) this.#call(`recordings/${from}/rename`, to).catch(() => {});
  }

  #remove() {
    const name = this.#value.recording;
    if (name && confirm(`Delete “${name}”?`)) this.#call(`recordings/${name}/delete`).catch(() => {});
  }

  // Edits: applied here at once, then sent in pieces under the request limit
  // (ids are stable, so the pieces needn't arrive together).
  async #edit(edit, snapshot = true) {
    if (!this.tesser || this.#disabled) return;
    if (snapshot) this.#remember();
    this.#applyLocally(edit);
    this.#paint();
    for (const piece of this.#split(edit)) {
      try {
        await this.#call('edit', piece);
      } catch {
        return;
      }
    }
  }

  #remember() {
    this.#undo.push({ notes: this.#value.notes.map((n) => ({ ...n })), bends: this.#value.bends.map((b) => ({ ...b })) });
    if (this.#undo.length > 30) this.#undo.shift();
  }

  #undoLast() {
    const last = this.#undo.pop();
    if (!last) return;
    // Everything replaced by what was: new ids, so the selection goes.
    this.#selected.clear();
    this.#edit(
      {
        remove: [...this.#value.notes.map((n) => n.id), ...this.#value.bends.map((b) => b.id)],
        add: last.notes.map(({ t, d, n, v }) => ({ t, d, n, v })),
        addBends: last.bends.map(({ t, v }) => ({ t, v })),
      },
      false,
    );
  }

  #applyLocally(edit) {
    const gone = new Set(edit.remove ?? []);
    let notes = this.#value.notes.filter((n) => !gone.has(n.id)).map((n) => ({ ...n }));
    let bends = this.#value.bends.filter((b) => !gone.has(b.id)).map((b) => ({ ...b }));
    for (const s of edit.set ?? []) Object.assign(notes.find((n) => n.id === s.id) ?? bends.find((b) => b.id === s.id) ?? {}, s);
    // Ids the controller will give; until then, none to select.
    for (const a of edit.add ?? []) notes.push({ id: -1, v: 100, ...a });
    for (const a of edit.addBends ?? []) bends.push({ id: -1, ...a });
    notes = notes.sort((a, b) => a.t - b.t || a.n - b.n);
    bends = bends.sort((a, b) => a.t - b.t);
    this.#value = { ...this.#value, notes, bends };
  }

  *#split(edit) {
    if (JSON.stringify(edit).length <= MAX_BODY) {
      yield edit;
      return;
    }
    let piece = {};
    let size = 2;
    for (const key of ['remove', 'set', 'add', 'addBends']) {
      for (const item of edit[key] ?? []) {
        const n = JSON.stringify(item).length + 1;
        if (size + n + key.length + 4 > MAX_BODY && Object.keys(piece).length) {
          yield piece;
          piece = {};
          size = 2;
        }
        (piece[key] ??= []).push(item);
        size += n + (piece[key].length === 1 ? key.length + 4 : 0);
      }
    }
    if (Object.keys(piece).length) yield piece;
  }

  #quantize() {
    if (!this.#grid) return;
    const ids = this.#selectedNotes().map((n) => n.id);
    this.#remember();
    this.#call('quantize', ids.length ? { grid: this.#grid, ids } : { grid: this.#grid }).catch(() => {});
  }

  #setVelocity(v) {
    if (!(v >= 1 && v <= 127)) return;
    this.#velocity = Math.round(v);
    const sel = this.#selectedNotes();
    if (sel.length) this.#edit({ set: sel.map((n) => ({ id: n.id, v: this.#velocity })) });
  }

  // Geometry.

  get #beatMs() {
    return 60000 / (this.#value.bpm || 120);
  }
  get #stepMs() {
    return this.#grid ? this.#grid * this.#beatMs : 0;
  }
  #snap(ms, free) {
    const s = this.#stepMs;
    return !s || free ? Math.max(0, Math.round(ms)) : Math.max(0, Math.round(Math.round(ms / s) * s));
  }
  get #notesH() {
    return (this.#range[1] - this.#range[0] + 1) * ROW;
  }
  get #laneTop() {
    return RULER + this.#notesH + GAP;
  }
  get #height() {
    return this.#laneTop + LANE;
  }
  #y(n) {
    return RULER + (this.#range[1] - n) * ROW;
  }
  #pitchAt(y) {
    return clamp(this.#range[1] - Math.floor((y - RULER) / ROW), 0, 127);
  }
  #bendY(v) {
    return this.#laneTop + LANE / 2 - (v / 8192) * (LANE / 2 - 5);
  }
  #bendAt(y) {
    return clamp(Math.round(((this.#laneTop + LANE / 2 - y) / (LANE / 2 - 5)) * 8192), -8192, 8191);
  }
  #point(e) {
    const r = this.$('.main').getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  // The pitches shown: those used, and at least two octaves.
  #fit() {
    const ns = this.#value.notes.map((n) => n.n);
    let lo = ns.length ? Math.min(...ns) - 4 : 48;
    let hi = ns.length ? Math.max(...ns) + 4 : 72;
    while (hi - lo < 24) {
      lo--;
      hi++;
    }
    this.#range = [clamp(lo, 0, 127), clamp(hi, 0, 127)];
  }

  #zoomBy(f, clientX) {
    const scroll = this.$('.scroll');
    const r = scroll.getBoundingClientRect();
    const at = clientX === undefined ? r.width / 2 : clientX - r.left;
    const ms = (scroll.scrollLeft + at) / this.#zoom;
    this.#zoom = clamp(this.#zoom * f, 0.005, 2);
    zoomChoice.set(String(this.#zoom));
    this.#paint();
    scroll.scrollLeft = ms * this.#zoom - at;
  }

  // Notes and points as drawn: with a drag's preview.
  #notes() {
    const p = this.#preview;
    return p ? this.#value.notes.map((n) => (p.has(n.id) ? { ...n, ...p.get(n.id) } : n)) : this.#value.notes;
  }
  #bends() {
    const p = this.#preview;
    return p ? this.#value.bends.map((b) => (p.has(b.id) ? { ...b, ...p.get(b.id) } : b)) : this.#value.bends;
  }
  #selectedNotes() {
    return this.#value.notes.filter((n) => this.#selected.has(n.id));
  }

  // What is under a point, by coordinates: the drawing is rebuilt on each
  // change, so a double click's second click may land on a new element.
  #hit(x, y) {
    const z = this.#zoom;
    const length = this.#value.length ?? 0;
    if (this.#value.recording && y < RULER && Math.abs(x - length * z) <= 8) return { end: true };
    if (y >= this.#laneTop) {
      const b = this.#bends().findLast((p) => Math.hypot(p.t * z - x, this.#bendY(p.v) - y) <= 6);
      return b ? { id: b.id, bend: true } : null;
    }
    const n = this.#notes().findLast((q) => {
      const nx = q.t * z;
      const ny = this.#y(q.n);
      return x >= nx && x <= nx + Math.max(2, q.d * z) && y >= ny && y < ny + ROW;
    });
    if (!n) return null;
    const w = Math.max(2, n.d * z);
    return { id: n.id, edge: w > 10 && x > n.t * z + w - 6 };
  }

  // Pointer gestures.

  #down(e) {
    if (this.#disabled || e.button !== 0) return;
    e.preventDefault(); // no text selected by a drag
    this.$('.roll').focus({ preventScroll: true });
    const { x, y } = this.#point(e);
    // Double clicks from the presses themselves: the drawing is rebuilt on
    // each press, and a shape gone from under the pointer gets no dblclick.
    const last = this.#last;
    this.#last = { x, y, at: e.timeStamp };
    if (last && e.timeStamp - last.at < 400 && Math.hypot(x - last.x, y - last.y) < 5) {
      this.#last = null;
      e.preventDefault();
      this.#double(e, x, y);
      return;
    }
    const target = this.#hit(x, y);
    const svg = this.$('.main');
    if (target?.end) {
      this.#drag = { kind: 'length', x, length: this.#value.length };
    } else if (target && target.id >= 0) {
      const id = target.id;
      const bend = target.bend;
      if (e.shiftKey) {
        this.#selected.has(id) ? this.#selected.delete(id) : this.#selected.add(id);
      } else if (!this.#selected.has(id)) {
        this.#selected = new Set([id]);
      }
      if (!this.#selected.has(id)) {
        this.#paint();
        return;
      }
      if (bend) {
        const items = this.#value.bends.filter((b) => this.#selected.has(b.id));
        this.#drag = { kind: 'bend', x, y, anchor: this.#value.bends.find((b) => b.id === id), items };
      } else {
        const items = this.#selectedNotes();
        this.#drag = { kind: target.edge ? 'resize' : 'move', x, y, anchor: this.#value.notes.find((n) => n.id === id), items };
      }
    } else {
      // Empty space: a selection rectangle.
      if (!e.shiftKey) this.#selected.clear();
      this.#drag = { kind: 'marquee', x, y, kept: new Set(this.#selected) };
      this.#marquee = { x0: x, y0: y, x1: x, y1: y };
    }
    svg.setPointerCapture(e.pointerId);
    this.#paint();
  }

  #move(e) {
    const d = this.#drag;
    if (!d) return;
    const { x, y } = this.#point(e);
    const dms = (x - d.x) / this.#zoom;
    const free = e.altKey;
    d.moved ||= Math.abs(x - d.x) > 2 || Math.abs(y - (d.y ?? y)) > 2;
    if (d.kind === 'length') {
      this.#preview = null;
      d.to = clamp(this.#snap(d.length + dms, free), 1, 14400000);
    } else if (d.kind === 'move') {
      const t = this.#snap(d.anchor.t + dms, free);
      const minT = Math.min(...d.items.map((n) => n.t));
      const dt = Math.max(t - d.anchor.t, -minT);
      const notes = d.items.map((n) => n.n);
      const dn = clamp(Math.round((d.y - y) / ROW), -Math.min(...notes), 127 - Math.max(...notes));
      this.#preview = new Map(d.items.map((n) => [n.id, { t: n.t + dt, n: n.n + dn }]));
    } else if (d.kind === 'resize') {
      const end = this.#snap(d.anchor.t + d.anchor.d + dms, free);
      const dd = end - (d.anchor.t + d.anchor.d);
      const least = this.#stepMs && !free ? Math.round(this.#stepMs) : 1;
      this.#preview = new Map(d.items.map((n) => [n.id, { d: Math.max(least, n.d + dd) }]));
    } else if (d.kind === 'bend') {
      const t = this.#snap(d.anchor.t + dms, free);
      const minT = Math.min(...d.items.map((b) => b.t));
      const dt = Math.max(t - d.anchor.t, -minT);
      const dv = this.#bendAt(y) - this.#bendAt(d.y);
      this.#preview = new Map(d.items.map((b) => [b.id, { t: b.t + dt, v: clamp(b.v + dv, -8192, 8191) }]));
    } else if (d.kind === 'marquee') {
      this.#marquee = { ...this.#marquee, x1: x, y1: y };
      const [x0, x1] = [Math.min(this.#marquee.x0, x), Math.max(this.#marquee.x0, x)];
      const [y0, y1] = [Math.min(this.#marquee.y0, y), Math.max(this.#marquee.y0, y)];
      const sel = new Set(d.kept);
      for (const n of this.#value.notes) {
        const nx = n.t * this.#zoom;
        const ny = this.#y(n.n);
        if (nx < x1 && nx + Math.max(2, n.d * this.#zoom) > x0 && ny < y1 && ny + ROW > y0) sel.add(n.id);
      }
      for (const b of this.#value.bends) {
        const bx = b.t * this.#zoom;
        const by = this.#bendY(b.v);
        if (bx >= x0 && bx <= x1 && by >= y0 && by <= y1) sel.add(b.id);
      }
      this.#selected = sel;
    }
    this.#paint();
  }

  #up(e) {
    const d = this.#drag;
    if (!d) return;
    this.$('.main').releasePointerCapture?.(e.pointerId);
    this.#drag = null;
    this.#marquee = null;
    const preview = this.#preview;
    this.#preview = null;
    if (d.kind === 'length') {
      if (d.to && d.to !== d.length) {
        this.#value = { ...this.#value, length: d.to };
        this.#set('length', d.to);
      }
    } else if (preview && d.moved) {
      const set = [];
      for (const [id, p] of preview) {
        const was = [...this.#value.notes, ...this.#value.bends].find((x) => x.id === id);
        const changed = Object.fromEntries(Object.entries(p).filter(([k, v]) => was?.[k] !== v));
        if (Object.keys(changed).length) set.push({ id, ...changed });
      }
      if (set.length) this.#edit({ set });
    }
    this.#fit();
    this.#paint();
  }

  #cancel() {
    this.#drag = null;
    this.#marquee = null;
    this.#preview = null;
    this.#paint();
  }

  #double(e, x, y) {
    const target = this.#hit(x, y);
    if (target?.end) return;
    if (target) {
      const id = target.id;
      if (id < 0) return; // not yet given by the controller
      this.#selected.delete(id);
      this.#edit({ remove: [id] });
      return;
    }
    const s = this.#stepMs;
    const t = s && !e.altKey ? Math.floor(x / this.#zoom / s) * s : x / this.#zoom;
    if (y >= this.#laneTop && y <= this.#height) {
      this.#edit({ addBends: [{ t: Math.round(t), v: this.#bendAt(y) }] });
    } else if (y >= RULER && y < RULER + this.#notesH) {
      const d = Math.round(s || this.#beatMs / 2);
      this.#edit({ add: [{ t: Math.round(t), d, n: this.#pitchAt(y), v: this.#velocity }] });
    }
  }

  // Keys, while the roll has the focus.
  #key(e) {
    if (this.#disabled) return;
    const mod = e.ctrlKey || e.metaKey;
    if (mod && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      this.#undoLast();
    } else if (mod && e.key.toLowerCase() === 'a') {
      e.preventDefault();
      this.#selected = new Set(this.#value.notes.map((n) => n.id));
      this.#paint();
    } else if (e.key === 'Escape') {
      this.#selected.clear();
      this.#paint();
    } else if ((e.key === 'Delete' || e.key === 'Backspace') && this.#selected.size) {
      e.preventDefault();
      const ids = [...this.#selected];
      this.#selected.clear();
      this.#edit({ remove: ids });
    } else if (e.key.startsWith('Arrow') && this.#selected.size) {
      e.preventDefault();
      const notes = this.#selectedNotes();
      if (!notes.length) return;
      const step = Math.round(this.#stepMs || 10);
      let dt = 0;
      let dn = 0;
      if (e.key === 'ArrowLeft') dt = -Math.min(step, ...notes.map((n) => n.t));
      if (e.key === 'ArrowRight') dt = step;
      if (e.key === 'ArrowUp') dn = e.shiftKey ? 12 : 1;
      if (e.key === 'ArrowDown') dn = e.shiftKey ? -12 : -1;
      if (notes.some((n) => n.n + dn < 0 || n.n + dn > 127)) return;
      this.#edit({ set: notes.map((n) => ({ id: n.id, t: n.t + dt, n: n.n + dn })) });
    }
  }

  // The playhead: the announced position, run on this browser's clock.
  #tick() {
    this.#raf = requestAnimationFrame(() => this.#tick());
    const head = this.shadowRoot.querySelector('.head');
    const pos = this.#position();
    if (head) {
      const x = pos * this.#zoom;
      head.setAttribute('x1', x);
      head.setAttribute('x2', x);
    }
    const v = this.#value;
    this.$('.pos').textContent = `${time(pos)} / ${time(v.length ?? 0)}${v.state && v.state !== 'stopped' ? ` · ${v.state}` : ''}`;
  }

  #position() {
    const v = this.#value;
    if (v.state !== 'playing' && v.state !== 'recording') return v.state === 'armed' ? 0 : this.#base.pos;
    let pos = this.#base.pos + (performance.now() - this.#base.at);
    const takes = v.state === 'recording' && !v.overdub;
    if (!takes && v.length > 0) pos = v.loop ? pos % v.length : Math.min(pos, v.length);
    return pos;
  }

  // Drawing.

  #paint() {
    const v = this.#value;
    const $ = this.$;
    const off = this.#disabled;
    const names = (v.recordings ?? []).map((r) => r.name);
    const clip = $('.clip');
    const options = ['', ...names];
    if (v.recording && !names.includes(v.recording)) options.push(v.recording); // a take, not yet a file
    if (clip.options.length !== options.length || [...clip.options].some((o, i) => o.value !== options[i])) {
      clip.innerHTML = '';
      for (const n of options) clip.append(new Option(n || '(none)', n));
    }
    clip.value = v.recording ?? '';
    const recording = v.state === 'recording' || v.state === 'armed';
    const has = !!v.recording;
    for (const s of ['.clip', '.new', '.rec', '.stop', '.loop', '.overdub']) $(s).disabled = off;
    for (const s of ['.rename', '.delete', '.play']) $(s).disabled = off || !has || recording;
    for (const s of ['.bpm', '.beats', '.vel', '.quantize', '.undo']) $(s).disabled = off || !has || recording;
    $('.quantize').disabled ||= !this.#grid;
    $('.undo').disabled ||= !this.#undo.length;
    const rec = $('.rec');
    rec.classList.toggle('on', recording);
    rec.classList.toggle('armed', v.state === 'armed');
    $('.play').setAttribute('aria-pressed', String(v.state === 'playing'));
    $('.loop').setAttribute('aria-pressed', String(!!v.loop));
    $('.overdub').setAttribute('aria-pressed', String(!!v.overdub));
    const active = this.shadowRoot.activeElement;
    if (active !== $('.bpm')) $('.bpm').value = v.bpm ? String(+(+v.bpm).toFixed(2)) : '';
    if (active !== $('.beats')) $('.beats').value = v.beats ?? '';
    if (active !== $('.vel')) {
      const sel = this.#selectedNotes();
      $('.vel').value = String(sel.length && sel.every((n) => n.v === sel[0].v) ? sel[0].v : this.#velocity);
    }
    this.#draw();
  }

  #draw() {
    const v = this.#value;
    const notes = this.#notes();
    const bends = this.#bends();
    const z = this.#zoom;
    const length = this.#drag?.kind === 'length' && this.#drag.to ? this.#drag.to : v.length ?? 0;
    const end = Math.max(length, ...notes.map((n) => n.t + n.d), ...bends.map((b) => b.t), this.#position());
    const view = (this.$('.scroll').clientWidth || 600) / z;
    const width = Math.ceil(Math.max(end + 2 * this.#beatMs, view) * z);
    const height = this.#height;
    const [lo, hi] = this.#range;
    const out = [];

    // Rows, then the grid.
    for (let n = lo; n <= hi; n++) {
      if (BLACK.has(n % 12)) out.push(`<rect class="blackrow" x="0" y="${this.#y(n)}" width="${width}" height="${ROW}"/>`);
    }
    out.push(`<rect class="ruler" x="0" y="0" width="${width}" height="${RULER}"/>`);
    out.push(`<rect class="lane" x="0" y="${this.#laneTop}" width="${width}" height="${LANE}"/>`);
    const beat = this.#beatMs;
    const beats = v.beats || 4;
    const beatPx = beat * z;
    const barEvery = Math.max(1, Math.ceil(28 / (beatPx * beats))); // labels at least 28 px apart
    const sub = this.#stepMs * z >= 6 ? this.#stepMs : 0;
    for (let i = 0, t = 0; t <= width / z; i++, t = i * beat) {
      const x = t * z;
      const bar = i % beats === 0;
      if (bar || beatPx >= 5) {
        out.push(`<line class="${bar ? 'barline' : 'beat'}" x1="${x}" y1="${RULER}" x2="${x}" y2="${height}"/>`);
      }
      if (bar && (i / beats) % barEvery === 0) out.push(`<text x="${x + 3}" y="13">${i / beats + 1}</text>`);
      if (sub && sub < beat) {
        for (let s = t + sub; s < t + beat - 1; s += sub) {
          out.push(`<line class="sub" x1="${s * z}" y1="${RULER}" x2="${s * z}" y2="${RULER + this.#notesH}"/>`);
        }
      }
    }
    out.push(`<line class="centre" x1="0" y1="${this.#bendY(0)}" x2="${width}" y2="${this.#bendY(0)}"/>`);
    // Past the length: shaded.
    if (length > 0) out.push(`<rect class="past" x="${length * z}" y="${RULER}" width="${Math.max(0, width - length * z)}" height="${height - RULER}"/>`);

    // Notes.
    for (const n of notes) {
      const o = this.#outputs[n.n] ?? 0;
      const fill = o ? COLORS[((o >= 10 ? o % 10 : o) - 1) % COLORS.length] : '#9aa1ab';  // 10 × slot + output: its output's
      const cls = `note${this.#selected.has(n.id) ? ' sel' : ''}${n.t >= length && length ? ' out' : ''}`;
      const opacity = 0.45 + (0.55 * n.v) / 127;
      out.push(
        `<rect class="${cls}" data-id="${n.id}" data-kind="n" x="${n.t * z}" y="${this.#y(n.n) + 0.5}" width="${Math.max(2, n.d * z)}" height="${ROW - 1}" rx="1.5" fill="${fill}" fill-opacity="${opacity.toFixed(2)}"><title>${noteName(n.n)} · ${time(n.t)} · ${Math.round(n.d)} ms · velocity ${n.v}${o ? ` · output ${o}` : ''}</title></rect>`,
      );
    }

    // Bend: each value held until the next point, the centre before the first.
    if (bends.length) {
      let d = `M0 ${this.#bendY(0)}`;
      for (const b of bends) d += ` H${b.t * z} V${this.#bendY(b.v)}`;
      d += ` H${width}`;
      out.push(`<path class="bendline" d="${d}"/>`);
      for (const b of bends) {
        out.push(
          `<circle class="point${this.#selected.has(b.id) ? ' sel' : ''}" data-id="${b.id}" data-kind="b" cx="${b.t * z}" cy="${this.#bendY(b.v)}" r="4"><title>${time(b.t)} · ${(b.v / 8192).toFixed(2)}</title></circle>`,
        );
      }
    }

    // The length, its handle, and the playhead.
    if (v.recording) {
      const x = length * z;
      out.push(`<line class="endline" x1="${x}" y1="0" x2="${x}" y2="${height}"/>`);
      out.push(`<path class="end" data-end="" d="M${x} ${RULER} l-8 -${RULER - 4} h16 z"><title>The end: ${time(length)}</title></path>`);
    }
    out.push(`<line class="head" x1="0" y1="0" x2="0" y2="${height}"/>`);
    if (this.#marquee) {
      const m = this.#marquee;
      out.push(`<rect class="marquee" x="${Math.min(m.x0, m.x1)}" y="${Math.min(m.y0, m.y1)}" width="${Math.abs(m.x1 - m.x0)}" height="${Math.abs(m.y1 - m.y0)}"/>`);
    }
    const main = this.$('.main');
    main.setAttribute('width', width);
    main.setAttribute('height', height);
    main.innerHTML = out.join('');

    // The pitch column.
    const keys = [];
    for (let n = lo; n <= hi; n++) {
      const y = this.#y(n);
      if (BLACK.has(n % 12)) keys.push(`<rect class="blackrow" x="0" y="${y}" width="${KEYS}" height="${ROW}"/>`);
      if (n % 12 === 0) keys.push(`<text x="3" y="${y + ROW - 1.5}">${noteName(n)}</text>`);
    }
    keys.push(`<text x="3" y="${this.#laneTop + 12}">Bend</text>`);
    const k = this.$('.keys');
    k.setAttribute('height', height);
    k.innerHTML = keys.join('');
    this.#tickHead();
  }

  #tickHead() {
    const head = this.shadowRoot.querySelector('.head');
    if (!head) return;
    const x = this.#position() * this.#zoom;
    head.setAttribute('x1', x);
    head.setAttribute('x2', x);
  }
}

if (!customElements.get('x-recorder')) customElements.define('x-recorder', Recorder);
