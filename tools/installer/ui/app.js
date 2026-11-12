// Nova Setup: three pages (start, progress, done) over the backend's
// commands (src/main.rs). Outside the setup executable (a browser preview) a
// mock backend stands in.
'use strict';

const tauri = window.__TAURI__;
const invoke = tauri ? tauri.core.invoke : mockInvoke;
const listen = tauri ? tauri.event.listen : mockListen;
const appWindow = tauri ? tauri.window.getCurrentWindow() : null;

const $ = (id) => document.getElementById(id);
const state = { info: null, minors: new Set(), busy: false, installed: null, experimental: false };

function escape(text) {
  return String(text).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
}

// One page shown; the footer's step and buttons follow it.
function show(page, step, secondary, primary) {
  for (const p of ['start', 'progress', 'done']) $('page-' + p).classList.toggle('shown', p === page);
  $('step').textContent = step;
  setButton($('btn-secondary'), secondary);
  setButton($('btn-primary'), primary);
}

function setButton(button, spec) {
  button.classList.toggle('hidden', !spec);
  if (!spec) return;
  button.textContent = spec.label;
  button.onclick = spec.onClick;
  button.disabled = !!spec.disabled;
  button.classList.toggle('danger', !!spec.danger);
  button.classList.toggle('primary', button.id === 'btn-primary' && !spec.danger);
}

function setProgress(fraction, text) {
  const f = Math.max(0, Math.min(1, fraction));
  $('bar-fill').style.width = (f * 100).toFixed(1) + '%';
  $('percent').textContent = Math.round(f * 100) + ' %';
  if (text) $('status').textContent = text;
}

// ---- The start page ---------------------------------------------------------------

function renderHoudini() {
  const info = state.info;
  const found = info.houdinis.filter((h) => h.supported);
  const where = found.length ? found[0].packages : `Documents\\houdini${info.houdini_minor}\\packages`;
  const detail = found.length
    ? `${found.map((h) => h.version).join(', ')} found \u00b7 package in ${where}`
    : `Not found yet \u00b7 Nova is registered for it anyway`;
  $('houdini').innerHTML =
    `<label class="check"><input type="checkbox" id="minor" ${state.minors.size ? 'checked' : ''}>` +
    `<span>Houdini ${escape(info.houdini_minor)}<span class="sub" title="${escape(detail)}">${escape(detail)}</span></span></label>`;
  $('minor').addEventListener('change', (e) => {
    if (e.target.checked) state.minors.add(info.houdini_minor); else state.minors.delete(info.houdini_minor);
    updateInstall();
  });
}

// The card Nova XPU would render on (info.xpu: the NVIDIA driver's first
// device and what Nova XPU does with it). One it is built for but not tested
// on gets the option that turns it on; unticked, Nova XPU renders on the CPU.
function renderGpu() {
  const info = state.info, xpu = info.xpu;
  const nvidia = info.gpus.find((g) => g.nvidia);
  const gpu = nvidia || info.gpus[0];
  const name = xpu ? xpu.name : gpu ? gpu.name : 'No graphics card found';
  if (xpu && xpu.support === 'experimental') {
    const detail = 'Experimental: use it for Nova XPU (not tested on this card yet). Off: CPU.';
    $('gpu').innerHTML =
      `<label class="check"><input type="checkbox" id="experimental" ${state.experimental ? 'checked' : ''}>` +
      `<span>${escape(name)}<span class="sub" title="${escape(detail)}">${escape(detail)}</span></span></label>`;
    $('experimental').addEventListener('change', (e) => { state.experimental = e.target.checked; });
    return;
  }
  const detail = xpu
    ? (xpu.support === 'tested' ? 'Nova XPU available'
      : 'Older than Nova XPU supports (RTX 20 / GTX 16 or newer): Nova XPU will use the CPU')
    : nvidia ? 'The NVIDIA driver did not answer: Nova XPU needs driver 590 or newer'
      : 'No NVIDIA GPU: Nova XPU will use the CPU';
  $('gpu').innerHTML = `${escape(name)}<span class="sub" title="${escape(detail)}">${escape(detail)}</span>`;
}

function renderNote() {
  const info = state.info, lines = [];
  if (info.installed)
    lines.push(`Nova ${info.installed.version} is installed in ${info.installed.dir}. Installing replaces it.`);
  const others = info.houdinis.filter((h) => !h.supported).map((h) => h.version);
  if (others.length)
    lines.push(`Also found Houdini ${others.join(', ')}: this build of Nova runs in Houdini ${info.houdini_minor} only.`);
  $('start-note').innerHTML = lines.map(escape).join('<br>');
}

function updateInstall() {
  $('btn-primary').disabled = state.minors.size === 0 || $('dir').value.trim().length < 3;
}

function startPage() {
  if (state.info.uninstall) {
    show('start', 'Step 1 of 3', { label: 'Cancel', onClick: close },
      { label: 'Remove', onClick: run, danger: true, disabled: !state.info.installed });
    return;
  }
  show('start', 'Step 1 of 3', { label: 'Cancel', onClick: close }, { label: 'Install', onClick: run });
  updateInstall();
}

// ---- Install, uninstall ---------------------------------------------------------------

async function run() {
  if (state.busy) return;
  state.busy = true;
  const removing = state.info.uninstall;
  $('progress-title').textContent = removing ? 'Removing Nova' : 'Installing Nova';
  $('message').className = 'message hidden';
  setProgress(0, 'Starting');
  show('progress', 'Step 2 of 3', { label: 'Cancel', disabled: true }, { label: removing ? 'Remove' : 'Install', disabled: true });
  try {
    if (removing) await invoke('run_uninstall');
    else state.installed = await invoke('run_install',
      { dir: $('dir').value.trim(), minors: [...state.minors], experimental: state.experimental });
    state.busy = false;
    donePage();
  } catch (e) {
    state.busy = false;
    const text = String(e), m = $('message');
    if (text.startsWith('HOUDINI_RUNNING:')) {
      setProgress(0, 'Waiting');
      m.textContent = `Houdini is running (${text.slice('HOUDINI_RUNNING:'.length)}). Close it, then click Retry.`;
      m.className = 'message';
    } else if (text.startsWith('NOT_NOVA:')) {
      // The backend never replaces a nova folder or install.json that is not Nova's.
      m.textContent = `${text.slice('NOT_NOVA:'.length)} already holds a nova folder or install.json that is not ` +
        `Nova's. Go back and choose another folder.`;
      m.className = 'message error';
    } else if (text.startsWith('INSIDE_OLD:')) {
      const old = text.slice('INSIDE_OLD:'.length);
      m.textContent = `That folder is inside the Nova install in ${old}. Go back and choose ${old} itself or another folder.`;
      m.className = 'message error';
    } else {
      m.textContent = text;
      m.className = 'message error';
    }
    show('progress', 'Step 2 of 3', { label: 'Back', onClick: startPage }, { label: 'Retry', onClick: run });
  }
}

function donePage() {
  if (state.info.uninstall) {
    $('done-title').textContent = 'Nova is removed';
    $('done-lead').textContent = 'Its files and its Houdini package file are gone. Your scenes stay as they are.';
    $('done-husk').classList.add('hidden');
    show('done', 'Step 3 of 3', null, { label: 'Close', onClick: close });
    return;
  }
  show('done', 'Step 3 of 3', { label: 'Open folder', onClick: () => invoke('open_folder', { path: state.installed.dir }) },
    { label: 'Close', onClick: close });
}

// ---- Start ---------------------------------------------------------------------------------

async function start() {
  const info = await invoke('system_info');
  state.info = info;
  document.title = `Nova ${info.version} Setup`;
  if (info.uninstall) {
    $('start-title').textContent = 'Remove Nova';
    $('start-lead').innerHTML = info.installed
      ? `Nova ${escape(info.installed.version)} in <b>${escape(info.installed.dir)}</b> and its Houdini package file will be removed. Your scenes stay as they are.`
      : 'Nova is not installed on this computer.';
    $('rows').classList.add('hidden');
  } else {
    $('start-title').textContent = `Install Nova ${info.version}`;
    $('dir').value = info.default_dir;
    state.minors.add(info.houdini_minor); // also before Houdini is installed
    state.experimental = !!info.experimental_gpus; // kept as it is where the option is not shown
    renderHoudini();
    renderGpu();
    renderNote();
    $('dir').addEventListener('input', updateInstall);
    $('btn-browse').onclick = async () => {
      const dir = await invoke('pick_folder', { start: $('dir').value });
      if (dir) { $('dir').value = dir; updateInstall(); }
    };
  }
  await listen('progress', (e) => setProgress(e.payload.fraction, e.payload.text));
  startPage();
  if (appWindow) await appWindow.show();
}

function close() {
  if (state.busy) return;
  if (appWindow) appWindow.close(); else window.close();
}

$('btn-close').onclick = close;
$('btn-min').onclick = () => appWindow && appWindow.minimize();
document.addEventListener('mousedown', (e) => {
  if (appWindow && e.buttons === 1 && e.target.closest('[data-tauri-drag-region]') && !e.target.closest('button'))
    appWindow.startDragging();
});
document.addEventListener('contextmenu', (e) => { if (!e.target.closest('input')) e.preventDefault(); });

// ---- The browser preview's backend ---------------------------------------------------------

function mockInvoke(cmd, args) {
  const params = new URLSearchParams(location.search);
  if (cmd === 'system_info')
    return Promise.resolve({
      version: '0.1.4', houdini_minor: '22.0', payload_mb: 17.4, uninstall: params.has('uninstall'),
      default_dir: 'C:\\Users\\you\\AppData\\Local\\Programs\\Nova',
      installed: params.has('installed') || params.has('uninstall')
        ? { dir: 'C:\\Users\\you\\AppData\\Local\\Programs\\Nova', version: '0.1.4', packages: [] } : null,
      houdinis: [
        { version: '22.0.466', minor: '22.0', supported: true, path: '', packages: 'C:\\Users\\you\\Documents\\houdini22.0\\packages' },
        ...(params.has('other') ? [{ version: '21.0.512', minor: '21.0', supported: false, path: '', packages: '' }] : []),
      ],
      gpus: params.has('nogpu') ? [{ name: 'Intel(R) UHD Graphics 770', nvidia: false, memory_gb: 0.1 }]
        : [{ name: 'NVIDIA GeForce RTX 5080', nvidia: true, memory_gb: 15.6 }],
      // ?oldgpu: a card Nova XPU is not tested on (the option); ?ancient: one
      // it does not support; ?nodriver: an NVIDIA card whose driver is silent.
      xpu: params.has('nogpu') || params.has('nodriver') ? null
        : params.has('oldgpu') ? { name: 'NVIDIA GeForce RTX 3070', capability: '8.6', support: 'experimental' }
          : params.has('ancient') ? { name: 'NVIDIA GeForce GTX 1080', capability: '6.1', support: 'old' }
            : { name: 'NVIDIA GeForce RTX 5080', capability: '12.0', support: 'tested' },
      experimental_gpus: params.has('experimental'),
    });
  if (cmd === 'run_install' || cmd === 'run_uninstall')
    return new Promise((resolve, reject) => {
      let f = 0;
      const timer = setInterval(() => {
        f += 0.05;
        mockEmit({ fraction: Math.min(f, 1), text: f < 0.85 ? 'Copying hdNova.dll' : 'Registering Nova with Houdini' });
        if (f >= 1) {
          clearInterval(timer);
          if (params.has('running')) reject('HOUDINI_RUNNING:houdinifx.exe');
          else if (params.has('notnova') && cmd === 'run_install') reject('NOT_NOVA:' + args.dir);
          else resolve({ dir: args && args.dir, version: '0.1.4', packages: [] });
        }
      }, 60);
    });
  if (cmd === 'pick_folder') return Promise.resolve('D:\\Tools\\Nova');
  return Promise.resolve(null);
}
const mockListeners = [];
function mockListen(name, fn) { mockListeners.push(fn); return Promise.resolve(() => {}); }
function mockEmit(payload) { mockListeners.forEach((fn) => fn({ payload })); }

start();
