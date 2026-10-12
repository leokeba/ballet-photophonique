// <x-midi>: the MIDI keyboard (docs/DESIGN.md section 7.1). It draws
// /midi/keys, {outputs[128], frequencies[128], voices[]}, as an 88-key
// keyboard coloured by output, and:
//
// - forwards a MIDI input of this computer (Web MIDI: Chrome or Edge, on a
//   secure page such as the launcher on GitHub Pages) to /midi/send: notes,
//   pitch bend, all notes off; one request in flight, what comes meanwhile
//   batched, a newer bend replacing an unsent one;
// - edits the keys: click one (shift-click: a range, or play keys with Learn
//   on), then choose its output and, for a single key, its frequency.

const LOW = 21; // A0
const HIGH = 108; // C8
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const COLORS = ['#e4572e', '#f3a712', '#29bf12', '#00a5cf', '#9b5de5', '#ef476f', '#118ab2', '#8d6e63'];
const BLACK = new Set([1, 3, 6, 8, 10]);
const W = 14; // a white key's width
const STORE = 'x-midi:input';
const LAUNCHER = 'https://leokeba.github.io/ballet-photophonique/';

const noteName = (n) => `${NAMES[n % 12]}${Math.floor(n / 12) - 1}`;
const noteHz = (n) => 440 * 2 ** ((n - 69) / 12);
const hz = (f) => (f >= 1000 ? `${+(f / 1000).toFixed(4)} kHz` : `${+f.toFixed(3)} Hz`);
const pack = (s, a, b) => s | (a << 8) | (b << 16);
const store = {
  get: () => {
    try {
      return localStorage.getItem(STORE);
    } catch {
      return null;
    }
  },
  set: (v) => {
    try {
      localStorage.setItem(STORE, v);
    } catch {}
  },
};

class Midi extends HTMLElement {
  #value = { outputs: [], frequencies: [], voices: [] };
  #schema = null;
  #disabled = false;
  #access = null;
  #input = null;
  #held = new Set(); // notes held on this computer's keyboard
  #selected = new Set();
  #anchor = null;
  #queue = [];
  #busy = false;
  #keys = new Map(); // note → <g>
  #led = 0;

  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).innerHTML = `
      <style>
        :host { display: block; }
        .bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 8px; }
        select, input, button { font: inherit; color: var(--text, inherit); background: var(--surface, #fff);
          border: 1px solid var(--border, #d9dde3); border-radius: 6px; padding: 3px 7px; }
        button { cursor: pointer; }
        button[aria-pressed="true"] { background: var(--accent, #2f5bd3); border-color: var(--accent, #2f5bd3); color: #fff; }
        button:disabled, input:disabled { opacity: .5; cursor: default; }
        .status { color: var(--muted, #5d6675); font-size: .9em; }
        .status.warn { color: var(--warn, #b45309); }
        .led { width: 9px; height: 9px; border-radius: 50%; background: var(--border, #d9dde3); transition: background .15s; }
        .led.on { background: var(--ok, #1f9d55); transition: none; }
        .scroll { overflow-x: auto; padding-bottom: 4px; }
        svg { display: block; touch-action: manipulation; user-select: none; }
        .k rect { stroke: var(--border, #9aa1ab); stroke-width: .6; cursor: pointer; }
        .w rect { fill: #fdfdfd; } .b rect { fill: #262a31; }
        .k.held rect { filter: brightness(.6); }
        .k.held:not(.mapped) rect { fill: var(--accent, #2f5bd3); filter: none; }
        .k.sel rect { stroke: var(--accent, #2f5bd3); stroke-width: 2.2; }
        .k.sounds .dot { display: block; }
        .dot { display: none; fill: var(--accent, #2f5bd3); stroke: #fff; stroke-width: 1; }
        .k text { font: 600 8px system-ui, sans-serif; text-anchor: middle; pointer-events: none; }
        .w text { fill: #222; } .b text { fill: #fff; }
        .c { fill: var(--muted, #5d6675); font: 8px system-ui, sans-serif; text-anchor: middle; }
        .edit { display: flex; flex-wrap: wrap; gap: 6px 14px; align-items: center; margin-top: 8px; }
        .edit b { font-weight: 600; }
        .outs { display: inline-flex; gap: 3px; }
        .outs button { min-width: 2em; }
        .outs button i { display: inline-block; width: 8px; height: 8px; border-radius: 2px; margin-right: 4px; }
        input[type=number] { width: 9em; }
        .hint { color: var(--muted, #5d6675); font-size: .85em; margin-top: 6px; }
      </style>
      <div class="bar">
        <label>MIDI input <select class="inputs"><option value="">None</option></select></label>
        <span class="led" title="MIDI activity"></span>
        <span class="status"></span>
        <button class="learn" aria-pressed="false" title="Played keys become the selection">Learn</button>
      </div>
      <div class="scroll"><svg></svg></div>
      <div class="edit">
        <span class="which">No key selected</span>
        <span>Output <span class="outs"></span></span>
        <label>Frequency <input class="freq" type="number" min="0" step="any" placeholder="the note's own"> Hz</label>
        <button class="reset" title="Back to the note's own frequency">Note's own</button>
      </div>
      <div class="hint">Click a key to choose its output; shift-click for a range.</div>`;
    const $ = (s) => this.shadowRoot.querySelector(s);
    this.$ = $;
    this.#build();
    $('.inputs').addEventListener('change', (e) => this.#use(e.target.value, true));
    $('.learn').addEventListener('click', (e) => {
      const b = e.currentTarget;
      b.setAttribute('aria-pressed', String(b.getAttribute('aria-pressed') !== 'true'));
    });
    $('.freq').addEventListener('change', (e) => this.#setFrequency(e.target.value === '' ? 0 : +e.target.value));
    $('.reset').addEventListener('click', () => this.#setFrequency(0));
  }

  connectedCallback() {
    if (this.#access || !this.isConnected) return;
    if (!navigator.requestMIDIAccess) {
      this.#status(
        window.isSecureContext
          ? 'This browser has no Web MIDI: use Chrome or Edge.'
          : `Web MIDI needs a secure page: open this controller from the launcher (${LAUNCHER}).`,
        true,
      );
      return;
    }
    navigator.requestMIDIAccess({ sysex: false }).then(
      (access) => {
        this.#access = access;
        access.onstatechange = () => this.#list();
        this.#list();
      },
      () => this.#status('MIDI access was refused: allow it in the address bar.', true),
    );
  }

  disconnectedCallback() {
    if (this.#input) this.#input.onmidimessage = null;
    if (this.#access) this.#access.onstatechange = null;
    this.#access = null;
    this.#input = null;
  }

  set value(v) {
    this.#value = v && typeof v === 'object' ? v : this.#value;
    this.#paint();
  }
  get value() {
    return this.#value;
  }
  set schema(s) {
    this.#schema = s;
    this.#outputs();
  }
  set disabled(d) {
    this.#disabled = !!d;
    this.#editor();
  }
  set label(_) {}
  set error(_) {}

  // How many outputs: the key map's range.
  get #count() {
    return this.#schema?.children?.outputs?.items?.max ?? 5;
  }

  #status(text, warn = false) {
    const s = this.$('.status');
    s.textContent = text;
    s.classList.toggle('warn', warn);
  }

  // MIDI inputs: the remembered one, else the first.
  #list() {
    if (!this.#access) return;
    const sel = this.$('.inputs');
    const inputs = [...this.#access.inputs.values()];
    const want = this.#input?.id ?? store.get() ?? inputs[0]?.id ?? '';
    sel.innerHTML = '<option value="">None</option>';
    for (const i of inputs) sel.append(new Option(i.name || i.id, i.id));
    const id = inputs.some((i) => i.id === want) ? want : (inputs[0]?.id ?? '');
    sel.value = id;
    this.#use(id, false);
    if (!inputs.length) this.#status('No MIDI input: plug in a keyboard.');
  }

  #use(id, chosen) {
    if (chosen) store.set(id);
    const next = id && this.#access ? this.#access.inputs.get(id) : null;
    if (next === this.#input) return;
    if (this.#input) this.#input.onmidimessage = null;
    this.#held.clear();
    this.#input = next ?? null;
    if (this.#input) {
      this.#input.onmidimessage = (e) => this.#message(e.data);
      this.#status(`Playing from ${this.#input.name || 'the keyboard'}.`);
    } else if (this.#access) {
      this.#status('No MIDI input chosen.');
    }
    this.#paint();
  }

  #message(d) {
    const s = d[0];
    const t = s & 0xf0;
    let m = null;
    if (t === 0x90 || t === 0x80) {
      const on = t === 0x90 && d[2] > 0;
      on ? this.#held.add(d[1]) : this.#held.delete(d[1]);
      if (on && this.$('.learn').getAttribute('aria-pressed') === 'true') this.#select(d[1], this.#held.size > 1);
      m = pack(s, d[1], d[2]);
    } else if (t === 0xe0) {
      // Only the latest bend matters.
      this.#queue = this.#queue.filter((q) => (q & 0xf0) !== 0xe0);
      m = pack(s, d[1], d[2]);
    } else if (t === 0xb0 && (d[1] === 120 || d[1] === 123)) {
      this.#held.clear();
      m = pack(s, d[1], d[2]);
    }
    if (m === null) return;
    const led = this.$('.led');
    led.classList.add('on');
    clearTimeout(this.#led);
    this.#led = setTimeout(() => led.classList.remove('on'), 80);
    this.#queue.push(m);
    this.#pump();
    this.#paint();
  }

  async #pump() {
    if (this.#busy || !this.#queue.length || !this.tesser) return;
    this.#busy = true;
    const batch = this.#queue.splice(0);
    try {
      await this.tesser.call('/midi/send', batch.length === 1 ? batch[0] : batch);
    } catch (e) {
      this.#status(`Not sent: ${e?.message ?? e?.error ?? e}`, true);
    }
    this.#busy = false;
    this.#pump();
  }

  // The keyboard, once.
  #build() {
    const svg = this.$('svg');
    const whites = [];
    for (let n = LOW; n <= HIGH; n++) if (!BLACK.has(n % 12)) whites.push(n);
    const width = whites.length * W;
    svg.setAttribute('viewBox', `0 0 ${width} 92`);
    svg.setAttribute('width', String(width * 1.25));
    const ns = 'http://www.w3.org/2000/svg';
    const el = (tag, attrs, parent) => {
      const e = document.createElementNS(ns, tag);
      for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
      parent.append(e);
      return e;
    };
    const blacks = [];
    let x = 0;
    for (let n = LOW; n <= HIGH; n++) {
      const black = BLACK.has(n % 12);
      const g = document.createElementNS(ns, 'g');
      g.classList.add('k', black ? 'b' : 'w');
      g.dataset.note = n;
      const kx = black ? x - W * 0.32 : x;
      const kw = black ? W * 0.64 : W;
      const kh = black ? 48 : 78;
      el('rect', { x: kx, y: 0, width: kw, height: kh, rx: 1.5 }, g);
      el('circle', { class: 'dot', cx: kx + kw / 2, cy: kh - 24, r: 3 }, g);
      el('text', { x: kx + kw / 2, y: kh - 6 }, g);
      el('title', {}, g);
      if (!black) svg.append(g);
      else blacks.push(g);
      if (n % 12 === 0) el('text', { class: 'c', x: x + W / 2, y: 89 }, svg).textContent = noteName(n);
      if (!black) x += W;
      this.#keys.set(n, g);
      g.addEventListener('click', (e) => this.#select(n, e.shiftKey));
    }
    for (const g of blacks) svg.append(g); // over the white keys
    // Start around middle C.
    requestAnimationFrame(() => {
      const scroll = this.$('.scroll');
      scroll.scrollLeft = Math.max(0, (whites.indexOf(48) * W * 1.25) - 20);
    });
  }

  #select(n, range) {
    if (range && this.#anchor !== null) {
      this.#selected = new Set();
      const [a, b] = [Math.min(this.#anchor, n), Math.max(this.#anchor, n)];
      for (let i = a; i <= b; i++) this.#selected.add(i);
    } else {
      this.#selected = new Set([n]);
      this.#anchor = n;
    }
    this.#paint();
  }

  #outputs() {
    const box = this.$('.outs');
    box.innerHTML = '';
    for (let o = 0; o <= this.#count; o++) {
      const b = document.createElement('button');
      b.dataset.output = o;
      b.innerHTML = o ? `<i style="background:${COLORS[(o - 1) % COLORS.length]}"></i>${o}` : '–';
      b.title = o ? `Output ${o}` : 'No output';
      b.addEventListener('click', () => this.#setOutput(o));
      box.append(b);
    }
    this.#editor();
  }

  #setOutput(o) {
    if (!this.#selected.size || !this.tesser) return;
    const outputs = [...(this.#value.outputs ?? [])];
    while (outputs.length < 128) outputs.push(0);
    for (const n of this.#selected) outputs[n] = o;
    this.tesser.set('outputs', outputs);
  }

  #setFrequency(f) {
    if (this.#selected.size !== 1 || !this.tesser || !(f >= 0)) return;
    const [n] = this.#selected;
    this.tesser.set(`frequencies/${n}`, f);
  }

  #paint() {
    const outs = this.#value.outputs ?? [];
    const freqs = this.#value.frequencies ?? [];
    const sounding = new Set((this.#value.voices ?? []).filter((v) => v >= 0));
    for (const [n, g] of this.#keys) {
      const o = outs[n] ?? 0;
      const black = g.classList.contains('b');
      g.querySelector('rect').style.fill = o ? COLORS[(o - 1) % COLORS.length] : '';
      g.querySelector('rect').style.fillOpacity = o && !black ? '0.75' : '';
      g.querySelector('text').textContent = o ? String(o) : '';
      g.classList.toggle('held', this.#held.has(n));
      g.classList.toggle('mapped', o > 0);
      g.classList.toggle('sel', this.#selected.has(n));
      g.classList.toggle('sounds', sounding.has(n));
      const f = freqs[n] > 0 ? freqs[n] : noteHz(n);
      g.querySelector('title').textContent = `${noteName(n)} (${n}): ${o ? `output ${o}` : 'no output'}, ${hz(f)}`;
    }
    this.#editor();
  }

  #editor() {
    const sel = [...this.#selected].sort((a, b) => a - b);
    const outs = this.#value.outputs ?? [];
    const which = this.$('.which');
    const freq = this.$('.freq');
    if (!sel.length) {
      which.textContent = 'No key selected';
    } else if (sel.length === 1) {
      const n = sel[0];
      which.innerHTML = `<b>${noteName(n)}</b> (${n}), its own ${hz(noteHz(n))}`;
    } else {
      which.innerHTML = `<b>${noteName(sel[0])}–${noteName(sel.at(-1))}</b> (${sel.length} keys)`;
    }
    const same = sel.length && sel.every((n) => (outs[n] ?? 0) === (outs[sel[0]] ?? 0)) ? (outs[sel[0]] ?? 0) : null;
    for (const b of this.$('.outs').children) {
      b.disabled = this.#disabled || !sel.length;
      b.setAttribute('aria-pressed', String(same !== null && +b.dataset.output === same));
    }
    const one = sel.length === 1;
    freq.disabled = this.#disabled || !one;
    this.$('.reset').disabled = this.#disabled || !one;
    if (one && this.shadowRoot.activeElement !== freq) {
      const f = (this.#value.frequencies ?? [])[sel[0]] ?? 0;
      freq.value = f > 0 ? String(f) : '';
    } else if (!one) {
      freq.value = '';
    }
  }
}

if (!customElements.get('x-midi')) customElements.define('x-midi', Midi);
