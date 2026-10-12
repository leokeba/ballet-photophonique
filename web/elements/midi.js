// <x-midi>: the MIDI keyboard (docs/DESIGN.md section 7.1). It draws
// /midi/keys, {outputs[128], frequencies[128], voices[]}, as an 88-key
// keyboard coloured by output, and:
//
// - forwards a MIDI input of this computer (Web MIDI: Chrome or Edge, on a
//   secure page such as the launcher on GitHub Pages) to /midi/send: notes,
//   pitch bend, all notes off; one request in flight, what comes meanwhile
//   batched, a newer bend replacing an unsent one;
// - plays from the computer's keys, as Ableton Live does, when no MIDI input
//   is in use (or by choice): A W S E D F T G Y H U J K O L P ; ' are an
//   octave and a half from C, Z and X move the octave, C and V the velocity.
//   Physical positions (KeyboardEvent.code), so an AZERTY keyboard plays the
//   same places, labelled with its own letters where the browser tells them
//   (Chrome's keyboard layout map, on secure pages; QWERTY's otherwise).
//   No secure page needed: it works from the controller's own page too;
// - edits the keys: click one (shift-click: a range, or play keys with Learn
//   on), then choose its output and, for a single key, its frequency.

const LOW = 21; // A0
const HIGH = 108; // C8
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const COLORS = ['#e4572e', '#f3a712', '#29bf12', '#00a5cf', '#9b5de5', '#ef476f', '#118ab2', '#8d6e63'];
const BLACK = new Set([1, 3, 6, 8, 10]);
const W = 14; // a white key's width
const STORE = 'x-midi:input';
const STORE_TYPING = 'x-midi:typing';
const LAUNCHER = 'https://leokeba.github.io/ballet-photophonique/';
// The computer's keys: semitones above the octave's C, by physical position.
const TYPING = {
  KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9,
  KeyU: 10, KeyJ: 11, KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16, Quote: 17,
};
const OCTAVE_DOWN = 'KeyZ';
const OCTAVE_UP = 'KeyX';
const VELOCITY_DOWN = 'KeyC';
const VELOCITY_UP = 'KeyV';
const VELOCITIES = [1, 20, 40, 60, 80, 100, 120, 127]; // Ableton's steps
// Labels without the browser's layout map: QWERTY.
const QWERTY = (code) => (code.startsWith('Key') ? code.slice(3) : { Semicolon: ';', Quote: "'" }[code] ?? code);

const noteName = (n) => `${NAMES[n % 12]}${Math.floor(n / 12) - 1}`;
const noteHz = (n) => 440 * 2 ** ((n - 69) / 12);
const hz = (f) => (f >= 1000 ? `${+(f / 1000).toFixed(4)} kHz` : `${+f.toFixed(3)} Hz`);
const pack = (s, a, b) => s | (a << 8) | (b << 16);
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
const store = local(STORE);
const typingChoice = local(STORE_TYPING);

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
  #typing = false; // playing from the computer's keys
  #octave = 60; // the note of the A key: C4
  #velocity = 100;
  #pressed = new Map(); // code → the note its key-down played
  #labels = new Map(); // code → the letter on this layout
  #onKey = (e) => this.#key(e);
  #onBlur = () => this.#releaseTyped();

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
        .kbd { display: none; font: 600 7px ui-monospace, monospace; }
        .typing .kbd { display: block; }
        .w .kbd { fill: var(--muted, #5d6675); } .b .kbd { fill: #cfd3da; }
        .typinfo { color: var(--muted, #5d6675); font-size: .9em; display: none; }
        .typing .typinfo { display: inline; }
        kbd { font: .85em ui-monospace, monospace; border: 1px solid var(--border, #d9dde3); border-radius: 3px; padding: 0 3px; }
      </style>
      <div class="bar">
        <label class="picker">MIDI input <select class="inputs"><option value="">None</option></select></label>
        <span class="led" title="MIDI activity"></span>
        <span class="status"></span>
        <button class="learn" aria-pressed="false" title="Played keys become the selection">Learn</button>
        <button class="typekeys" aria-pressed="false" title="Play with the computer's keys">Computer keys</button>
        <span class="typinfo"></span>
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
    $('.typekeys').addEventListener('click', () => {
      typingChoice.set(this.#typing ? '0' : '1');
      this.#setTyping(!this.#typing);
    });
  }

  connectedCallback() {
    if (!this.isConnected) return;
    window.addEventListener('keydown', this.#onKey);
    window.addEventListener('keyup', this.#onKey);
    window.addEventListener('blur', this.#onBlur);
    navigator.keyboard?.getLayoutMap?.().then(
      (map) => {
        for (const code of [...Object.keys(TYPING), OCTAVE_DOWN, OCTAVE_UP, VELOCITY_DOWN, VELOCITY_UP]) {
          const c = map.get(code);
          if (c) this.#labels.set(code, c.toUpperCase());
        }
        this.#paint();
      },
      () => {},
    );
    this.#setTyping(this.#typingWanted());
    if (this.#access) return;
    if (!navigator.requestMIDIAccess) {
      this.$('.picker').hidden = true;
      this.#status(
        window.isSecureContext
          ? 'This browser has no Web MIDI (Chrome or Edge have it): play with the computer\'s keys.'
          : `No Web MIDI on this page: play with the computer's keys, or open the controller from the launcher (${LAUNCHER}) for a MIDI keyboard.`,
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
    window.removeEventListener('keydown', this.#onKey);
    window.removeEventListener('keyup', this.#onKey);
    window.removeEventListener('blur', this.#onBlur);
    this.#releaseTyped();
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
    if (this.#disabled) this.#releaseTyped();
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
    if (!inputs.length) this.#status("No MIDI input: plug in a keyboard, or play with the computer's keys.");
  }

  #use(id, chosen) {
    if (chosen) store.set(id);
    const next = id && this.#access ? this.#access.inputs.get(id) : null;
    if (next === this.#input) return;
    if (this.#input) this.#input.onmidimessage = null;
    this.#held.clear();
    this.#input = next ?? null;
    if (this.#input) {
      this.#input.onmidimessage = (e) => this.#message(e.data, e.timeStamp);
      this.#status(`Playing from ${this.#input.name || 'the keyboard'}.`);
    } else if (this.#access) {
      this.#status('No MIDI input chosen.');
    }
    this.#setTyping(this.#typingWanted());
    this.#paint();
  }

  // The computer's keys: as last chosen, else when no MIDI input is in use.
  #typingWanted() {
    const choice = typingChoice.get();
    return choice === null ? !this.#input : choice === '1';
  }

  #setTyping(on) {
    if (!on) this.#releaseTyped();
    this.#typing = on;
    this.$('.typekeys').setAttribute('aria-pressed', String(on));
    this.shadowRoot.querySelector('.bar').classList.toggle('typing', on);
    this.$('svg').classList.toggle('typing', on);
    this.#paint();
  }

  #key(e) {
    if (!this.#typing || this.#disabled || e.ctrlKey || e.metaKey || e.altKey) return;
    // Not while typing in a field, here or anywhere on the page.
    const t = e.composedPath()[0];
    if (t instanceof HTMLElement && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    const down = e.type === 'keydown';
    const step = TYPING[e.code];
    if (step !== undefined) {
      e.preventDefault();
      if (down) {
        if (e.repeat || this.#pressed.has(e.code)) return;
        const n = this.#octave + step;
        if (n > 127) return;
        this.#pressed.set(e.code, n);
        this.#message([0x90, n, this.#velocity]);
      } else if (this.#pressed.has(e.code)) {
        // The note its key-down played, whatever the octave is now.
        this.#message([0x80, this.#pressed.get(e.code), 0]);
        this.#pressed.delete(e.code);
      }
      return;
    }
    if (!down || e.repeat) return;
    if (e.code === OCTAVE_DOWN || e.code === OCTAVE_UP) {
      e.preventDefault();
      this.#octave = Math.min(108, Math.max(0, this.#octave + (e.code === OCTAVE_UP ? 12 : -12)));
      this.#reveal(this.#octave);
      this.#paint();
    } else if (e.code === VELOCITY_DOWN || e.code === VELOCITY_UP) {
      e.preventDefault();
      const i = VELOCITIES.findIndex((v) => v >= this.#velocity);
      const j = Math.min(VELOCITIES.length - 1, Math.max(0, (i < 0 ? VELOCITIES.length - 1 : i) + (e.code === VELOCITY_UP ? 1 : -1)));
      this.#velocity = VELOCITIES[j];
      this.#paint();
    }
  }

  // Note offs for the computer's keys held down: on blur, when typing stops,
  // when the element goes.
  #releaseTyped() {
    for (const n of this.#pressed.values()) this.#message([0x80, n, 0]);
    this.#pressed.clear();
  }

  #label(code) {
    return this.#labels.get(code) ?? QWERTY(code);
  }

  // Scrolls the keyboard so that `note`'s octave shows.
  #reveal(note) {
    const scroll = this.$('.scroll');
    const g = this.#keys.get(Math.max(LOW, Math.min(HIGH, note)));
    if (!g) return;
    const x = +g.querySelector('rect').getAttribute('x') * 1.25;
    if (x < scroll.scrollLeft || x + 24 * W * 1.25 > scroll.scrollLeft + scroll.clientWidth) {
      scroll.scrollLeft = Math.max(0, x - 20);
    }
  }

  // `at`: when it was played, on performance.now()'s clock (Web MIDI's
  // timeStamp is on it too).
  #message(d, at = performance.now()) {
    const now = performance.now();
    if (!(at > 0 && at <= now)) at = now; // a timeStamp on another clock
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
      this.#queue = this.#queue.filter((q) => (q.m & 0xf0) !== 0xe0);
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
    this.#queue.push({ m, at });
    this.#pump();
    this.#paint();
  }

  async #pump() {
    if (this.#busy || !this.#queue.length || !this.tesser) return;
    this.#busy = true;
    const now = performance.now();
    const batch = this.#queue.splice(0).map(({ m, at }) => {
      const age = Math.min(60000, Math.round(now - at));
      return age >= 1 ? [m, age] : m;
    });
    try {
      // A pair is only ever an item of a batch.
      await this.tesser.call('/midi/send', batch.length === 1 && typeof batch[0] === 'number' ? batch[0] : batch);
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
      el('text', { class: 'kbd', x: kx + kw / 2, y: black ? 10 : 58 }, g);
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
      g.querySelector('.kbd').textContent = '';
      g.classList.toggle('held', this.#held.has(n));
      g.classList.toggle('mapped', o > 0);
      g.classList.toggle('sel', this.#selected.has(n));
      g.classList.toggle('sounds', sounding.has(n));
      const f = freqs[n] > 0 ? freqs[n] : noteHz(n);
      g.querySelector('title').textContent = `${noteName(n)} (${n}): ${o ? `output ${o}` : 'no output'}, ${hz(f)}`;
    }
    if (this.#typing) {
      for (const [code, step] of Object.entries(TYPING)) {
        const g = this.#keys.get(this.#octave + step);
        if (g) g.querySelector('.kbd').textContent = this.#label(code);
      }
      const k = (code) => `<kbd>${this.#label(code)}</kbd>`;
      const top = Math.min(127, this.#octave + 17);
      this.$('.typinfo').innerHTML =
        `${noteName(this.#octave)}–${noteName(top)} ${k(OCTAVE_DOWN)} ${k(OCTAVE_UP)} · ` +
        `velocity ${this.#velocity} ${k(VELOCITY_DOWN)} ${k(VELOCITY_UP)}`;
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
