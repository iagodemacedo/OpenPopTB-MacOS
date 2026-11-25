// Browser host for the WebAssembly build (docs/wasm/PLAN.md, M2).
// - loads the player's game folder into the in-memory file system (/game), from a folder picker or,
//   with ?dev, from the local dev server (tools/serve_web.py --game <folder>)
// - starts main() (the game runs in workers: PROXY_TO_PTHREAD) and draws its frames on the canvas
// - sends keyboard and mouse events to the game (Module.poptbInput, a ring buffer read by
//   runtime/plat_headless.c). The game draws its own cursor. In a window the mouse is absolute; in
//   fullscreen (Cmd+Enter) it is captured (Pointer Lock) and sends movements only, like the Mac app,
//   and Keyboard Lock keeps Escape for the game (hold Escape to leave fullscreen)
// - plays the game's sound: the mixer (runtime/audio_web.c) fills a ring buffer that an AudioWorklet
//   (audio-worklet.js) plays. Browsers allow sound only after a click or key press: choosing the
//   game folder counts; with ?dev the first click or key press starts it
// URL parameters: ?dev  load the game from the dev server
//                 ?args=-v  extra command-line arguments, space separated
//                 ?script=31000:click:320,302  scripted input (same syntax as the native --script)
//                 ?shots=10000,30000  (dev) post canvas screenshots at these ms after start
//                 ?log  (dev) post the console output to the dev server
//                 ?uiscript=30000:click:320,302;70000:key:Escape  (dev) the same input syntax, but sent as
//                   DOM events to the canvas, so it exercises the page's input path
'use strict';
const params = new URLSearchParams(location.search);
const dev = params.has('dev');
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
const panel = document.getElementById('panel');
const status = document.getElementById('status');

// files the game does not need (installer extras, the Direct3D and manual files)
const skip = (p) => /^__redist\/|\.pdf$|^d3dpoptb.*\.exe$|^dxcfg\.exe$/i.test(p);

// ---- dev log forwarding ----
let logBuf = [];
function log(line) {
  console.log(line);
  if (dev && params.has('log')) logBuf.push(line);
}
if (dev && params.has('log'))
  setInterval(() => {
    if (!logBuf.length) return;
    const body = logBuf.join('\n') + '\n'; logBuf = [];
    fetch('/__log', { method: 'POST', body }).catch(() => {});
  }, 500);

// ---- frames ----
let frames = 0;
function drawFrame(px, w, h) {
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  ctx.putImageData(new ImageData(new Uint8ClampedArray(px.buffer), w, h), 0, 0);
  frames++;
}

// ---- dev screenshots ----
function scheduleShots(t0) {
  if (!dev || !params.get('shots')) return;
  for (const ms of params.get('shots').split(',').map(Number)) {
    setTimeout(() => canvas.toBlob((b) => fetch(`/__shot?name=shot_${String(ms).padStart(6, '0')}.png`,
      { method: 'POST', body: b }).catch(() => {}), 'image/png'), Math.max(0, ms - (performance.now() - t0)));
  }
}

// ---- input ----
// KeyboardEvent.code -> [DirectInput scan code (PC set 1), Windows virtual key]; same table as plat.c
const KEYS = { Escape: [0x01, 0x1B], Minus: [0x0C, 0xBD], Equal: [0x0D, 0xBB], Backspace: [0x0E, 0x08],
  Tab: [0x0F, 0x09], BracketLeft: [0x1A, 0xDB], BracketRight: [0x1B, 0xDD], Enter: [0x1C, 0x0D],
  ControlLeft: [0x1D, 0x11], Semicolon: [0x27, 0xBA], Quote: [0x28, 0xDE], Backquote: [0x29, 0xC0],
  ShiftLeft: [0x2A, 0x10], Backslash: [0x2B, 0xDC], Comma: [0x33, 0xBC], Period: [0x34, 0xBE],
  Slash: [0x35, 0xBF], ShiftRight: [0x36, 0x10], NumpadMultiply: [0x37, 0x6A], AltLeft: [0x38, 0x12],
  Space: [0x39, 0x20], CapsLock: [0x3A, 0x14], NumLock: [0x45, 0x90], ScrollLock: [0x46, 0x91],
  Numpad7: [0x47, 0x67], Numpad8: [0x48, 0x68], Numpad9: [0x49, 0x69], NumpadSubtract: [0x4A, 0x6D],
  Numpad4: [0x4B, 0x64], Numpad5: [0x4C, 0x65], Numpad6: [0x4D, 0x66], NumpadAdd: [0x4E, 0x6B],
  Numpad1: [0x4F, 0x61], Numpad2: [0x50, 0x62], Numpad3: [0x51, 0x63], Numpad0: [0x52, 0x60],
  NumpadDecimal: [0x53, 0x6E], F11: [0x57, 0x7A], F12: [0x58, 0x7B], NumpadEnter: [0x9C, 0x0D],
  ControlRight: [0x9D, 0x11], NumpadDivide: [0xB5, 0x6F], AltRight: [0xB8, 0x12], Pause: [0xC5, 0x13],
  Home: [0xC7, 0x24], ArrowUp: [0xC8, 0x26], PageUp: [0xC9, 0x21], ArrowLeft: [0xCB, 0x25],
  ArrowRight: [0xCD, 0x27], End: [0xCF, 0x23], ArrowDown: [0xD0, 0x28], PageDown: [0xD1, 0x22],
  Insert: [0xD2, 0x2D], Delete: [0xD3, 0x2E] };
'1234567890'.split('').forEach((d, i) => { KEYS['Digit' + d] = [0x02 + i, d.charCodeAt(0)]; });
for (let i = 0; i < 10; i++) KEYS['F' + (i + 1)] = [0x3B + i, 0x70 + i];
'QWERTYUIOP'.split('').forEach((k, i) => { KEYS['Key' + k] = [0x10 + i, k.charCodeAt(0)]; });
'ASDFGHJKL'.split('').forEach((k, i) => { KEYS['Key' + k] = [0x1E + i, k.charCodeAt(0)]; });
'ZXCVBNM'.split('').forEach((k, i) => { KEYS['Key' + k] = [0x2C + i, k.charCodeAt(0)]; });

const IN_KEY = 1, IN_MOVE = 2, IN_BUTTON = 3, IN_WHEEL = 4, IN_FOCUS = 5, IN_CHAR = 6, IN_MOVE_REL = 7, IN_MOUSE_MODE = 8;
const input = (t, a, b, c) => { if (started && Module.poptbInput) Module.poptbInput(t, a, b, c || 0); };

function onKey(e) {
  if (!started) return;
  const down = e.type === 'keydown';
  if (down && e.metaKey && e.code === 'Enter') {   // Cmd+Enter: fullscreen, like the Mac app
    e.preventDefault();
    toggleFullscreen();
    return;
  }
  if (e.metaKey) return;   // leave the browser's shortcuts (Cmd+R, Cmd+W, ...) alone
  const k = KEYS[e.code];
  if (!k) return;
  e.preventDefault();
  input(IN_KEY, k[0], k[1], (down ? 1 : 0) | (e.repeat ? 2 : 0) | (e.altKey ? 4 : 0));
  if (down && e.key.length === 1 && !e.ctrlKey && !e.altKey) input(IN_CHAR, e.key.charCodeAt(0));
}
// ---- fullscreen: pointer captured, movements only ----
let locked = false, fracX = 0, fracY = 0;
const lockPointer = () => { try { const p = canvas.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch (_) {} };
function toggleFullscreen() {
  if (document.fullscreenElement) { document.exitFullscreen(); return; }
  document.documentElement.requestFullscreen().then(() => {
    // Escape goes to the game instead of leaving fullscreen (Chrome: hold Escape to leave)
    if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock(['Escape']).catch(() => {});
  }).catch(() => {});
  lockPointer();
}
document.addEventListener('fullscreenchange', () => {
  if (document.fullscreenElement) return;
  if (document.pointerLockElement) document.exitPointerLock();
  if (navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
});
document.addEventListener('pointerlockchange', () => {
  locked = document.pointerLockElement === canvas;
  fracX = fracY = 0;
  input(IN_MOUSE_MODE, locked ? 1 : 0);   // back to absolute: the game re-syncs its cursor
});

// canvas pixels per CSS pixel (the canvas is letterboxed with object-fit: contain)
function canvasScale() {
  const r = canvas.getBoundingClientRect();
  return Math.min(r.width / canvas.width, r.height / canvas.height);
}
// canvas pixel under the pointer
function gamePos(e) {
  const r = canvas.getBoundingClientRect();
  const s = canvasScale();
  const x = (e.clientX - r.left - (r.width - canvas.width * s) / 2) / s;
  const y = (e.clientY - r.top - (r.height - canvas.height * s) / 2) / s;
  return [Math.max(0, Math.min(canvas.width - 1, Math.floor(x))), Math.max(0, Math.min(canvas.height - 1, Math.floor(y)))];
}
const BUTTON = { 0: 0, 2: 1, 1: 2 };   // DOM button -> left, right, middle
function onMouse(e) {
  if (!started) return;
  if (locked) {
    if (e.type === 'mousemove') {   // movement in screen pixels -> game pixels, keeping the fractions
      const s = canvasScale();
      fracX += e.movementX / s; fracY += e.movementY / s;
      const dx = Math.trunc(fracX), dy = Math.trunc(fracY);
      fracX -= dx; fracY -= dy;
      if (dx || dy) input(IN_MOVE_REL, dx, dy);
    }
  } else {
    const [x, y] = gamePos(e);
    input(IN_MOVE, x, y);
  }
  if (e.type === 'mousedown' || e.type === 'mouseup') {
    e.preventDefault();
    if (e.button in BUTTON) input(IN_BUTTON, BUTTON[e.button], e.type === 'mousedown' ? 1 : 0);
    // fullscreen but not captured (e.g. after switching apps): a click captures it again
    if (e.type === 'mousedown' && !locked && document.fullscreenElement) lockPointer();
  }
}
window.addEventListener('keydown', onKey);
window.addEventListener('keyup', onKey);
canvas.addEventListener('mousemove', onMouse);
canvas.addEventListener('mousedown', onMouse);
window.addEventListener('mouseup', onMouse);   // also when released outside the canvas
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('wheel', (e) => {
  if (!started) return;
  e.preventDefault();
  if (e.deltaY) input(IN_WHEEL, e.deltaY < 0 ? 120 : -120);
}, { passive: false });
window.addEventListener('blur', () => input(IN_FOCUS, 0));
window.addEventListener('focus', () => input(IN_FOCUS, 1));

// ---- dev: scripted DOM input ----
function runUiScript(t0) {
  if (!dev || !params.get('uiscript')) return;
  const at = (ms, fn) => setTimeout(fn, Math.max(0, ms - (performance.now() - t0)));
  const client = (gx, gy) => {
    const r = canvas.getBoundingClientRect();
    const s = Math.min(r.width / canvas.width, r.height / canvas.height);
    return { clientX: r.left + (r.width - canvas.width * s) / 2 + (gx + 0.5) * s,
             clientY: r.top + (r.height - canvas.height * s) / 2 + (gy + 0.5) * s, bubbles: true };
  };
  for (const step of params.get('uiscript').split(';')) {
    const [ms, kind, arg] = step.split(':'); const t = Number(ms);
    if (kind === 'key') {
      const code = /^[A-Z]$/i.test(arg) ? 'Key' + arg.toUpperCase() : arg === 'Return' ? 'Enter' : arg;
      at(t, () => { window.dispatchEvent(new KeyboardEvent('keydown', { code, key: arg, bubbles: true }));
                    window.dispatchEvent(new KeyboardEvent('keyup', { code, key: arg, bubbles: true })); });
    } else if (kind === 'click' || kind === 'rclick' || kind === 'move') {
      const [x, y, hold = 120] = arg.split(',').map(Number);
      const button = kind === 'rclick' ? 2 : 0;
      at(t, () => canvas.dispatchEvent(new MouseEvent('mousemove', client(x, y))));
      if (kind === 'move') continue;
      at(t + 80, () => canvas.dispatchEvent(new MouseEvent('mousedown', { ...client(x, y), button })));
      at(t + 80 + hold, () => window.dispatchEvent(new MouseEvent('mouseup', { ...client(x, y), button })));
    }
  }
}

// ---- audio ----
let audioCtx = null, audioRing = null, audioNode = null, workletLoading = null;
function ensureAudio() {   // call from a user gesture (click, key press, folder choice)
  if (!audioCtx) {
    try {
      audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
      audioCtx.onstatechange = connectAudio;
    } catch (e) { log('audio: ' + e); return; }
  }
  if (audioCtx.state !== 'running') audioCtx.resume().catch(() => {});
  connectAudio();
}
async function connectAudio() {
  if (!audioCtx || !audioRing) return;
  const hdr = new Int32Array(audioRing.buffer, audioRing.ptr, 4);
  if (audioCtx.state !== 'running') { Atomics.store(hdr, 2, 0); return; }   // mixer falls back to the clock
  if (!workletLoading) workletLoading = audioCtx.audioWorklet.addModule('audio-worklet.js');
  await workletLoading;
  if (!audioNode) {
    audioNode = new AudioWorkletNode(audioCtx, 'poptb-audio',
      { numberOfInputs: 0, outputChannelCount: [2], processorOptions: audioRing });
    audioNode.connect(audioCtx.destination);
    log(`audio: playing at ${audioCtx.sampleRate} Hz`);
  }
  if (!Atomics.load(hdr, 2)) {
    Atomics.store(hdr, 1, Atomics.load(hdr, 0));   // start from an empty ring
    Atomics.store(hdr, 2, 1);
  }
}
// called by the game when DirectSound starts (runtime/audio_web.c)
function audioStart(buffer, ptr, n, rate) {
  audioRing = { buffer, ptr, n };
  log(`audio: ring ready (${n} frames, ${rate} Hz)`);
  connectAudio();
}
window.addEventListener('pointerdown', ensureAudio);
window.addEventListener('keydown', ensureAudio);
if (dev && params.get('audiostats'))   // dev: underrun counter in the log
  setInterval(() => { if (audioRing) log('audio underruns: ' + new Int32Array(audioRing.buffer, audioRing.ptr, 4)[3]); }, 5000);

// ---- runtime ----
let runtimeReady = false, filesReady = false, started = false;
window.Module = {
  print: log,
  printErr: log,
  poptbFrame: drawFrame,
  poptbAudioStart: audioStart,
  onRuntimeInitialized() { runtimeReady = true; maybeStart(); },
};
function maybeStart() {
  if (!runtimeReady || !filesReady || started) return;
  started = true;
  panel.classList.add('hidden');
  const args = ['--data', '/game'];
  if (params.get('args')) args.push(...params.get('args').split(' ').filter(Boolean));
  if (params.get('script')) args.push('--script', params.get('script'));
  log('starting: ' + args.join(' '));
  scheduleShots(performance.now());
  runUiScript(performance.now());
  Module.callMain(args);
}

function writeFile(rel, data) {
  const path = '/game/' + rel;
  Module.FS.mkdirTree(path.slice(0, path.lastIndexOf('/')));
  Module.FS.writeFile(path, data);
}
function waitRuntime() {
  return new Promise((res) => { const t = setInterval(() => { if (runtimeReady) { clearInterval(t); res(); } }, 50); });
}

async function loadFromPicker(fileList) {
  const files = [...fileList].map((f) => ({ f, rel: f.webkitRelativePath.split('/').slice(1).join('/') }))
                             .filter(({ rel }) => rel && !skip(rel));
  if (!files.some(({ rel }) => rel.toLowerCase() === 'poptb.exe')) {
    status.textContent = 'popTB.exe not found in that folder. Choose the folder that contains it.';
    return;
  }
  await waitRuntime();
  const total = files.reduce((s, { f }) => s + f.size, 0);
  let done = 0;
  for (const { f, rel } of files) {
    writeFile(rel, new Uint8Array(await f.arrayBuffer()));
    done += f.size;
    status.textContent = `Loading game files… ${Math.round(done / total * 100)}%`;
  }
  filesReady = true; maybeStart();
}

async function loadFromDevServer() {
  status.textContent = 'Loading game files from the dev server…';
  const list = await (await fetch('/__game/manifest.json')).json();
  await waitRuntime();
  const files = list.filter(({ path }) => !skip(path));
  const total = files.reduce((s, { size }) => s + size, 0);
  let done = 0;
  for (const { path, size } of files) {
    const r = await fetch('/__game/' + path.split('/').map(encodeURIComponent).join('/'));
    if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
    writeFile(path, new Uint8Array(await r.arrayBuffer()));
    done += size;
    status.textContent = `Loading game files… ${Math.round(done / total * 100)}%`;
  }
  log(`loaded ${files.length} files, ${(total / 1048576).toFixed(0)} MB`);
  filesReady = true; maybeStart();
}

if (!crossOriginIsolated) {
  status.textContent = 'This page needs cross-origin isolation (COOP/COEP headers) for shared memory.\n' +
                       'Serve it with tools/serve_web.py.';
} else {
  const s = document.createElement('script');
  s.src = 'PopulousTB.js';
  document.body.appendChild(s);
  if (dev) {
    ensureAudio();   // starts only if the browser allows sound without a gesture; else on first click/key
    loadFromDevServer().catch((e) => { status.textContent = 'Dev load failed: ' + e.message; log(String(e)); });
  } else document.getElementById('pick').addEventListener('change', (e) => { ensureAudio(); loadFromPicker(e.target.files); });
}
