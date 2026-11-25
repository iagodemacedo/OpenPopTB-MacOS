// Browser host for the WebAssembly build (docs/wasm/PLAN.md, M2).
// - loads the player's game folder into the in-memory file system (/game), from a folder picker or,
//   with ?dev, from the local dev server (tools/serve_web.py --game <folder>)
// - starts main() (the game runs in workers: PROXY_TO_PTHREAD) and draws its frames on the canvas
// URL parameters: ?dev  load the game from the dev server
//                 ?args=-v  extra command-line arguments, space separated
//                 ?script=31000:click:320,302  scripted input (same syntax as the native --script)
//                 ?shots=10000,30000  (dev) post canvas screenshots at these ms after start
//                 ?log  (dev) post the console output to the dev server
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

// ---- runtime ----
let runtimeReady = false, filesReady = false, started = false;
window.Module = {
  print: log,
  printErr: log,
  poptbFrame: drawFrame,
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
  if (dev) loadFromDevServer().catch((e) => { status.textContent = 'Dev load failed: ' + e.message; log(String(e)); });
  else document.getElementById('pick').addEventListener('change', (e) => loadFromPicker(e.target.files));
}
