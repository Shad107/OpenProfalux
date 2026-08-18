'use strict';
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const api = (u, o) => fetch(u, o).then(r => r.ok ? r.json().catch(() => ({})) : Promise.reject(r.status));
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

/* ── Radio = ressource unique (mutex), TX synchrone ~1s. Pendant qu'une commande
 *    part, on desactive TOUS les boutons volet + on montre "envoi" pour eviter le
 *    matraquage : sinon on empile les emissions et on croit que ca ne marche pas.
 *    La classe est sur <body> -> elle survit aux re-render du polling /api/status. ── */
let radioBusy = false;
const setRadioBusy = on => { radioBusy = on; document.body.classList.toggle('radio-busy', on); };
async function sendCmd(body, activeBtn) {
  if (radioBusy) return;                       // radio occupee : on ignore le clic en trop
  setRadioBusy(true);
  if (activeBtn) activeBtn.classList.add('sending');
  try { await api('/api/shutter', { method: 'POST', body: JSON.stringify(body) }); }
  catch (e) { toast('Radio occupée, réessaie dans un instant'); }
  finally { if (activeBtn) activeBtn.classList.remove('sending'); setRadioBusy(false); }
}

/* ── Onglets + routes (hash de l'URL, ex #sys/wifi) + thème ── */
function activateMain(name) {
  const t = $(`.tab[data-t="${name}"]`); if (!t) return false;
  $$('.tab').forEach(x => x.classList.toggle('active', x === t));
  $$('.panel').forEach(p => p.classList.toggle('active', p.dataset.p === name));
  if (['sys', 'wifi', 'mqtt', 'ota'].includes(name)) loadConfig();
  return true;
}
function activateSub(sec, name) {
  const found = $$('.subtab', sec).some(x => x.dataset.s === name);
  $$('.subtab', sec).forEach(x => x.classList.toggle('active', x.dataset.s === name));
  $$('.subpanel', sec).forEach(p => p.classList.toggle('active', p.dataset.sp === name));
  return found;
}
function applyRoute() {
  const [main, sub] = (location.hash || '#control').replace(/^#/, '').split('/');
  if (!activateMain(main)) { activateMain('control'); return; }
  const sec = $(`.panel[data-p="${main}"]`);
  if (sub && sec) activateSub(sec, sub);
  if (typeof loadStatus === 'function') loadStatus();   /* refresh immediat a chaque changement d'onglet (ex: RF debug) */
  if (sub === 'rf') { if (typeof loadRf === 'function') loadRf(true); if (typeof loadFrames === 'function') loadFrames(); }
  if (sub === 'calib' && !calibLive && typeof fillCalib === 'function') fillCalib();   /* affiche les temps enregistres */
}
/* Rafraichit la 1re page quand l'onglet RF est actif ET qu'on n'a pas defile (sinon on garde la position). */
setInterval(() => { if ((location.hash || '').includes('/rf') && typeof loadRf === 'function' && rfOffset <= RF_PAGE) loadRf(true); }, 5000);
async function loadFrames() {
  const box = $('#frames-dataset'); if (!box) return;
  box.innerHTML = '<p class="hint">Chargement…</p>';
  const d = await api('/api/frames').catch(() => null);
  const fr = d && d.trames;
  if (!fr || !Object.keys(fr).length) {
    box.innerHTML = '<p class="hint">Aucune trame enregistrée (active « Écoute RF permanente » puis presse une télécommande).</p>';
    return;
  }
  box.innerHTML = Object.entries(fr).map(([serial, info]) => {
    const frames = info.frames || [];
    const shown = frames.slice(0, 300).map(f => `<code title="bouton ${esc(f.button)}">${esc(f.hop)}</code>`).join(' ');
    const more = frames.length > 300 ? ` <span class="hint">+${frames.length - 300} autres</span>` : '';
    return `<div class="card" style="margin-top:8px;padding:10px 12px"><b>${esc(remoteName(serial))}</b>
      <span class="hint">· ${info.count} trame(s) distincte(s) · hop + bouton + t (slide)</span><div class="hops">${shown}${more}</div></div>`;
  }).join('');
}
if ($('#frames-reload')) $('#frames-reload').onclick = loadFrames;
$$('.tab').forEach(t => t.onclick = () => { location.hash = t.dataset.t; });
$$('.subtab').forEach(t => t.onclick = () => {
  location.hash = `${t.closest('.panel').dataset.p}/${t.dataset.s}`;
});
addEventListener('hashchange', applyRoute);
$('#theme').onclick = () => {
  const r = document.documentElement;
  const dark = r.getAttribute('data-theme') === 'dark' ||
    (!r.getAttribute('data-theme') && matchMedia('(prefers-color-scheme:dark)').matches);
  r.setAttribute('data-theme', dark ? 'light' : 'dark');
};

/* ── Helpers ── */
function toast(m) { const t = $('#toast'); t.textContent = m; t.classList.add('show'); setTimeout(() => t.classList.remove('show'), 1600); }
function savedBtn(b, label) { const o = b.textContent; b.textContent = '✓ Enregistré'; setTimeout(() => b.textContent = label || o, 1500); }
const sigLvl = d => d >= -55 ? 4 : d >= -68 ? 3 : d >= -80 ? 2 : 1;
const sig = d => (d == null) ? '' :
  `<span class="sig l${sigLvl(d)}"><i></i><i></i><i></i><i></i></span><span class="dbm">${d} dBm</span>`;
/* RSSI le plus récent par serial, depuis la liste rf (déjà triée du + récent au + ancien) */
function rssiForSerials(serials, rf) { for (const f of rf) if ((serials || []).includes(f.serial)) return f.rssi; return null; }

/* ── Volets ── */
function renderVolets(list, rf) {
  const box = $('#volets'); box.innerHTML = '';
  $('#control-empty').hidden = list.length > 0;
  /* Position affichee UNIQUEMENT si l'ecoute permanente est active : sinon un coup de
   * vraie telecommande desynchronise l'estimation (pas de retour moteur) et un % faux
   * est pire qu'aucun %. */
  const showPos = !!statusCache.listening;
  for (const v of list) {
    const r = rssiForSerials(v.serials, rf);
    const el = document.createElement('div');
    el.className = 'card volet';
    el.innerHTML = `
      <div class="top"><span class="name">🪟 ${esc(v.id)}</span>${showPos ? `<span class="pct">${v.position ?? '?'}%</span>` : ''}</div>
      <div class="dpad"><button data-cmd="up">▲</button><button data-cmd="stop">■</button><button data-cmd="down">▼</button></div>
      ${showPos ? `<div class="slat" style="--p:${v.position ?? 50}"></div>` : ''}
      <div class="serials">serials : ${(v.serials || []).map(s => `<code>${esc(remoteName(s))}</code>`).join(' ') || 'aucun'}
        ${r != null ? `<span style="margin-left:6px">· reçu ${sig(r)}</span>` : ''}</div>`;
    el.querySelectorAll('[data-cmd]').forEach(b =>
      b.onclick = () => sendCmd({ id: v.id, cmd: b.dataset.cmd }, b));
    const slat = el.querySelector('.slat');
    if (slat) slat.onclick = e => {
      const p = Math.round(100 * (e.offsetX / e.currentTarget.offsetWidth));
      sendCmd({ id: v.id, cmd: 'pos', value: p });
    };
    box.appendChild(el);
  }
}

/* ── Apprentissage (centré volet) ── */
const LEARN_ACTIONS = [
  { a: 'up',   ico: '▲', lbl: 'Montée' },
  { a: 'stop', ico: '■', lbl: 'Stop' },
  { a: 'down', ico: '▼', lbl: 'Descente' },
];
let learning = false;   // capture en cours
let activeVolet = '';   // volet selectionne pour l'apprentissage

function renderVoletPicker() {
  const box = $('#volet-picker'); if (!box) return;
  const ids = (statusCache.volets || []).map(v => v.id);
  const nr = $('#new-volet-row');
  const newOpen = nr && !nr.hidden;
  if (activeVolet && !ids.includes(activeVolet) && !newOpen) activeVolet = '';
  box.innerHTML = '';
  for (const id of ids) {
    const c = document.createElement('button');
    c.className = 'chip' + (id === activeVolet ? ' on' : '');
    c.textContent = '🪟 ' + id;
    c.onclick = () => { activeVolet = id; if (nr) { nr.hidden = true; $('#new-volet').value = ''; } renderVoletPicker(); renderLearnSlots(); };
    box.appendChild(c);
  }
  const add = document.createElement('button');
  add.className = 'chip add' + (newOpen ? ' on' : '');
  add.textContent = '+ Nouveau volet';
  add.onclick = () => { if (!nr) return; nr.hidden = false; const inp = $('#new-volet'); inp.focus(); activeVolet = inp.value.trim(); renderVoletPicker(); renderLearnSlots(); };
  box.appendChild(add);
}

function renderLearnSlots() {
  const box = $('#learn-slots'); if (!box) return;
  const id = activeVolet;
  const v = (statusCache.volets || []).find(x => x.id === id);
  const cmd = (v && v.cmd) || {};
  box.innerHTML = '';
  for (const A of LEARN_ACTIONS) {
    const c = cmd[A.a];
    const learned = c != null;
    const row = document.createElement('div');
    row.className = 'slot' + (learned ? ' done' : '');
    const moveSel = learned
      ? `<select class="slot-move" data-from="${A.a}" title="Déplacer cette trame vers une autre action"><option value="">déplacer…</option>${LEARN_ACTIONS.filter(x => x.a !== A.a).map(x => `<option value="${x.a}">→ ${x.lbl}</option>`).join('')}</select>`
      : '';
    row.innerHTML = `<span class="slot-ico">${A.ico}</span><span class="slot-lbl">${A.lbl}</span>
      <span class="slot-state">${learned ? `✓ appris <code>0x${(c.b).toString(16).toUpperCase()}</code> <code>${esc(c.s)}</code>` : 'à capturer'}</span>
      ${moveSel}<button class="btn slot-btn" data-a="${A.a}">${learned ? 'Recapturer' : 'Capturer'}</button>`;
    /* drag-and-drop : glisser une commande apprise sur un autre slot -> reassignation */
    const act = A.a;
    if (learned) {
      row.draggable = true;
      row.addEventListener('dragstart', e => { e.dataTransfer.setData('text/plain', act); e.dataTransfer.effectAllowed = 'move'; row.classList.add('dragging'); });
      row.addEventListener('dragend', () => row.classList.remove('dragging'));
    }
    row.addEventListener('dragover', e => { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; row.classList.add('dragover'); });
    row.addEventListener('dragleave', () => row.classList.remove('dragover'));
    row.addEventListener('drop', e => {
      e.preventDefault(); row.classList.remove('dragover');
      const from = e.dataTransfer.getData('text/plain');
      if (from && from !== act && activeVolet && !learning) reassign(from, act);
    });
    box.appendChild(row);
  }
  const enabled = !!id && !learning;
  box.querySelectorAll('.slot-btn').forEach(b => { b.disabled = !enabled; b.onclick = () => captureAction(b.dataset.a, b); });
  box.querySelectorAll('.slot-move').forEach(sel => { sel.disabled = !enabled; sel.onchange = () => { if (sel.value) reassign(sel.dataset.from, sel.value); }; });
  const serialEl = $('#learn-serial');
  if (serialEl) {
    const sers = (v && v.serials) || [];
    serialEl.innerHTML = sers.length > 1
      ? `⚠ Commandes de plusieurs télécommandes : ${sers.map(s => `<code>${esc(s)}</code>`).join(' ')}`
      : sers.length ? `Télécommande : <code>${esc(sers[0])}</code>` : '';
  }
  const orientRow = $('#orient-row');
  if (orientRow) orientRow.hidden = !v;
  const orientInp = $('#orient-input');
  if (orientInp && v && document.activeElement !== orientInp)
    orientInp.value = (v.orientation != null && v.orientation >= 0) ? v.orientation : '';
  const delRow = $('#del-volet-row');
  if (delRow) delRow.hidden = !v;
  const hint = $('#learn-hint');
  if (hint) hint.textContent = id
    ? `Volet « ${id} » : clique Capturer, puis appuie une fois sur le bouton de ta télécommande (< 1 m du boîtier).`
    : 'Choisis un volet ci-dessus (ou crée-en un) pour activer la capture.';
  updateRemoteNameField();
}

/* Nom de la telecommande : prerempli avec le nom actuel (modifiable via Renommer)
 * quand le volet a deja une telecommande apprise ; vide + cache sinon. */
function updateRemoteNameField() {
  const inp = $('#cap-name'); if (!inp) return;
  const btn = $('#rename-btn');
  const vol = (statusCache.volets || []).find(x => x.id === activeVolet);
  const serial = vol && vol.serials && vol.serials[0];
  inp.dataset.serial = serial || '';
  /* pre-rempli : nom actuel de la telecommande, sinon le nom du volet (defaut modifiable), sinon vide */
  if (document.activeElement !== inp) inp.value = serial ? ((statusCache.remotes || {})[serial] || activeVolet || '') : '';
  inp.placeholder = serial ? 'Nom de cette télécommande' : 'ex : Murale chambre parents';
  if (btn) btn.hidden = !serial;
}

const renameBtn = $('#rename-btn');
if (renameBtn) renameBtn.onclick = async () => {
  const inp = $('#cap-name'); const serial = inp.dataset.serial;
  if (!serial) return;
  await api('/api/remote', { method: 'POST', body: JSON.stringify({ serial, name: inp.value.trim() }) });
  toast('Télécommande renommée'); await loadStatus();
};
const orientSave = $('#orient-save');
if (orientSave) orientSave.onclick = async () => {
  if (!activeVolet) return;
  const val = $('#orient-input').value.trim();
  const ori = val === '' ? -1 : parseInt(val, 10);
  await api('/api/volet/orientation', { method: 'POST', body: JSON.stringify({ id: activeVolet, orientation: ori }) });
  toast('Orientation enregistrée'); await loadStatus();
};

async function reassign(from, to) {
  if (!activeVolet || from === to) { renderLearnSlots(); return; }
  await api('/api/learn/reassign', { method: 'POST', body: JSON.stringify({ id: activeVolet, from, to }) });
  toast(`Commande déplacée vers ${LEARN_ACTIONS.find(x => x.a === to).lbl}`);
  await loadStatus();
}

async function captureAction(action, btn) {
  const id = activeVolet;
  if (!id) { toast('Choisis d’abord un volet'); return; }
  const label = LEARN_ACTIONS.find(x => x.a === action).lbl;
  learning = true; renderLearnSlots();
  btn.disabled = true; btn.textContent = `⏳ Appuie sur ${label}…`;
  await api('/api/learn/start', { method: 'POST', body: JSON.stringify({ action }) });
  const t0 = Date.now();
  const poll = setInterval(async () => {
    const r = await api('/api/learn/poll').catch(() => null);
    if (r && r.bits) {
      clearInterval(poll); learning = false;
      /* meme volet = meme telecommande : refuse une trame d'un autre serial que
       * celui deja appris (evite un stop d'une telecommande + montee d'une autre). */
      const vol = (statusCache.volets || []).find(x => x.id === id);
      const known = (vol && vol.serials) || [];
      if (known.length && !known.includes(r.serial)) {
        toast(`Pas la bonne télécommande : trame de ${r.serial}, le volet utilise ${known[0]}`);
        renderLearnSlots();
        return;
      }
      const name = $('#cap-name').value.trim();
      if (name) await api('/api/remote', { method: 'POST', body: JSON.stringify({ serial: r.serial, name }) });
      await api('/api/learn/assign', { method: 'POST', body: JSON.stringify({ id, action, bits: r.bits }) });
      toast(`✓ ${label} apprise (0x${r.button}, ${r.rssi} dBm)`);
      await loadStatus();     // rafraîchit -> le slot passe en ✓
    } else if (Date.now() - t0 > 16000) {
      clearInterval(poll); learning = false;
      toast(`Rien capté pour ${label}, rapproche-toi et réessaie`);
      renderLearnSlots();
    }
  }, 400);
}

const newVoletInput = $('#new-volet');
if (newVoletInput) newVoletInput.oninput = () => { activeVolet = newVoletInput.value.trim(); renderLearnSlots(); };

const delVoletBtn = $('#del-volet');
if (delVoletBtn) delVoletBtn.onclick = async () => {
  const id = activeVolet;
  if (!id) return;
  if (!confirm(`Supprimer le volet « ${id} » et toutes ses commandes apprises ?`)) return;
  await api('/api/volet/delete', { method: 'POST', body: JSON.stringify({ id }) });
  activeVolet = '';
  const nr = $('#new-volet-row'); if (nr) { nr.hidden = true; $('#new-volet').value = ''; }
  toast('Volet supprimé');
  await loadStatus();
};

/* ── Télécommandes ── */
function renderRemotes(map, rf) {
  const box = $('#remotes-list'); if (!box) return;
  const entries = Object.entries(map || {});
  $('#remotes-empty').hidden = entries.length > 0;
  box.innerHTML = '';
  for (const [serial, name] of entries) {
    const r = rssiForSerials([serial], rf);
    const row = document.createElement('div'); row.className = 'row'; row.style.alignItems = 'center';
    row.innerHTML = `<code class="badge">${esc(serial)}</code>
      <input style="flex:1" value="${esc(name)}" placeholder="nom…">${sig(r)}`;
    const inp = row.querySelector('input');
    inp.onchange = () => api('/api/remote', { method: 'POST', body: JSON.stringify({ serial, name: inp.value.trim() }) });
    box.appendChild(row);
  }
}
const remoteName = s => (statusCache.remotes || {})[s] || s;

/* ── Calibration ── */
let calT = {}, calStart = 0, calibLive = false;   /* calibLive : true pendant un chrono -> ne pas ecraser l'affichage */
function chrono(dir) { calibLive = true; calStart = performance.now(); $(`#cal-${dir}-start`).disabled = true; $(`#cal-${dir}-stop`).disabled = false; }
function chronoStop(dir) {
  calT[dir] = Math.round(performance.now() - calStart);
  $(`#cal-${dir}-t`).textContent = (calT[dir] / 1000).toFixed(1) + 's';
  const m = $(`#cal-${dir}-manual`); if (m) m.value = (calT[dir] / 1000).toFixed(1);   /* refleter dans le champ manuel */
  $(`#cal-${dir}-start`).disabled = false; $(`#cal-${dir}-stop`).disabled = true;
  $('#cal-save').disabled = !(calT.up && calT.down);
}
/* Relit les temps ENREGISTRES du volet selectionne (le statut expose travel_up_ms/down_ms). */
function fillCalib() {
  const sel = $('#calib-id'); if (!sel) return;
  const v = (statusCache.volets || []).find(x => x.id === sel.value);
  if (!v) return;
  calT = { up: v.travel_up_ms || 0, down: v.travel_down_ms || 0 };
  const ut = $('#cal-up-t'), dt = $('#cal-down-t');
  if (ut) ut.textContent = (calT.up / 1000).toFixed(1) + 's';
  if (dt) dt.textContent = (calT.down / 1000).toFixed(1) + 's';
  const um = $('#cal-up-manual'), dm = $('#cal-down-manual');
  if (um && document.activeElement !== um) um.value = calT.up ? (calT.up / 1000) : '';
  if (dm && document.activeElement !== dm) dm.value = calT.down ? (calT.down / 1000) : '';
  const sv = $('#cal-save'); if (sv) sv.disabled = !(calT.up && calT.down);
}
/* Saisie manuelle des temps (secondes) -> met a jour calT (ms) et active Enregistrer. */
function calManual(sel, key) {
  const el = $(sel); if (!el) return;
  el.oninput = () => {
    calibLive = true;   /* saisie en cours -> le poll ne doit pas ecraser */
    const s = parseFloat(el.value);
    calT[key] = (isNaN(s) || s < 0) ? 0 : Math.round(s * 1000);
    const t = $(`#cal-${key}-t`); if (t) t.textContent = ((calT[key] || 0) / 1000).toFixed(1) + 's';
    $('#cal-save').disabled = !(calT.up && calT.down);
  };
}
calManual('#cal-up-manual', 'up');
calManual('#cal-down-manual', 'down');
const calId = () => $('#calib-id').value.trim();
if ($('#calib-id')) $('#calib-id').onchange = () => { calibLive = false; fillCalib(); };   /* change de volet -> relit ses temps enregistres */
$('#cal-down-start').onclick = () => { chrono('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'down' }) }); };
$('#cal-down-stop').onclick  = () => { chronoStop('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'stop' }) }); };
$('#cal-up-start').onclick   = () => { chrono('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'up' }) }); };
$('#cal-up-stop').onclick    = () => { chronoStop('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'stop' }) }); };
$('#cal-save').onclick = () => { api('/api/calibrate', { method: 'POST',
  body: JSON.stringify({ id: calId(), travel_up_ms: calT.up, travel_down_ms: calT.down }) }); calibLive = false; toast('Calibration enregistrée'); };

/* ── Config : Wi-Fi / MQTT / Système (sauvegardes séparées, partielles) ── */
async function loadConfig() {
  const c = await api('/api/config').catch(() => ({}));
  $('#wifi-ssid').value = c.wifi_ssid || '';
  $('#mqtt-uri').value = c.mqtt_uri || ''; $('#mqtt-user').value = c.mqtt_user || '';
  if ($('#mqtt-pass')) $('#mqtt-pass').placeholder = c.mqtt_pass_len
    ? `•••••••• (${c.mqtt_pass_len} car. enregistrés, laisser vide pour ne pas changer)`
    : 'mot de passe du broker';
  $('#sys-device').value = c.device || ''; $('#sys-logframes').checked = !!c.log_frames;
  if ($('#mqtt-device')) $('#mqtt-device').value = c.device || '';
  const st = await api('/api/ota/status').catch(() => ({}));
  $('#ota-version').textContent = st.version || '…';
}
$('#wifi-save').onclick = async () => {
  const b = { wifi_ssid: $('#wifi-ssid').value.trim(), reboot: $('#wifi-reboot').checked };
  if ($('#wifi-pass').value) b.wifi_pass = $('#wifi-pass').value;
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  b.reboot ? toast('Wi-Fi enregistré, redémarrage…') : savedBtn($('#wifi-save'), 'Enregistrer le Wi-Fi');
};
$('#mqtt-save').onclick = async () => {
  const b = { mqtt_uri: $('#mqtt-uri').value.trim(), mqtt_user: $('#mqtt-user').value.trim(), reboot: true };
  if ($('#mqtt-pass').value) b.mqtt_pass = $('#mqtt-pass').value;
  if ($('#mqtt-device')) b.device = $('#mqtt-device').value.trim();
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  toast('MQTT enregistré, redémarrage…');
};
$('#sys-save').onclick = async () => {
  const b = { device: $('#sys-device').value.trim(), log_frames: $('#sys-logframes').checked, reboot: $('#sys-reboot').checked };
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  b.reboot ? toast('Enregistré, redémarrage…') : savedBtn($('#sys-save'), 'Enregistrer');
};
$('#mqtt-detect').onclick = async () => {
  const b = $('#mqtt-detect'); b.textContent = 'Recherche…'; b.disabled = true;
  const r = await api('/api/mqtt/discover').catch(() => ({}));
  if (r.uri) { $('#mqtt-uri').value = r.uri; b.textContent = 'Trouvé : ' + r.uri; }
  else b.textContent = 'Aucun broker trouvé';
  b.disabled = false;
  setTimeout(() => b.textContent = '🔍 Détecter le broker (mDNS)', 2500);
};

/* ── OTA ── */
$('#ota-btn').onclick = async () => {
  const f = $('#ota-file').files[0]; if (!f) return alert('Choisis un fichier .bin');
  $('#ota-prog').hidden = false; $('#ota-btn').disabled = true;
  const buf = await f.arrayBuffer();
  const up = fetch('/api/ota/upload', { method: 'POST', body: buf });
  const poll = setInterval(async () => {
    const s = await api('/api/ota/status').catch(() => null);
    if (s) { $('#ota-bar').value = s.total ? Math.round(100 * s.written / s.total) : 0; $('#ota-msg').textContent = s.msg || ''; }
    if (s && (s.state === 4 || s.state === 99)) clearInterval(poll);
  }, 800);
  up.catch(() => {});
};
$('#ota-rollback').onclick = async () => {
  if (!confirm('Revenir à la version précédente et redémarrer ?')) return;
  await api('/api/ota/rollback', { method: 'POST' }).catch(() => {});
};
/* OTA depuis GitHub (la dernière release publique) */
let githubUrl = null;
$('#ota-check').onclick = async () => {
  const span = $('#ota-latest'); span.textContent = '⏳ Interrogation de GitHub…'; $('#ota-github').hidden = true;
  try {
    const r = await fetch('https://api.github.com/repos/Shad107/OpenProfalux/releases/latest');
    if (!r.ok) { span.textContent = r.status === 404 ? '❌ Aucune release publique (dépôt privé ?)' : `❌ HTTP ${r.status}`; return; }
    const j = await r.json();
    const latest = j.tag_name || j.name || '?';
    const cur = $('#ota-version').textContent;
    githubUrl = (j.assets || []).map(a => a.browser_download_url).find(u => /openprofalux\.bin$/.test(u));
    if (githubUrl) $('#ota-github').hidden = false;
    span.textContent = (cur && cur === latest) ? `✅ ${cur} = dernière version`
      : `⚠️ Installée ${cur}, disponible ${latest}${githubUrl ? '' : ' (pas d’asset .bin)'}`;
  } catch { span.textContent = '❌ Pas d’accès Internet ?'; }
};
$('#ota-github').onclick = async () => {
  if (!githubUrl || !confirm('Installer la dernière version depuis GitHub et redémarrer ?')) return;
  $('#ota-prog').hidden = false; $('#ota-github').disabled = true; $('#ota-msg').textContent = 'téléchargement…';
  await api('/api/ota/pull', { method: 'POST', body: JSON.stringify({ url: githubUrl }) }).catch(() => {});
  const poll = setInterval(async () => {
    const s = await api('/api/ota/status').catch(() => null);
    if (s) { $('#ota-bar').value = s.total ? Math.round(100 * s.written / s.total) : 0; $('#ota-msg').textContent = s.msg || 'en cours…'; }
    if (s && (s.state === 4 || s.state === 99)) clearInterval(poll);
  }, 800);
};

/* ── Restauration ── */
$('#restore-btn').onclick = async () => {
  const f = $('#restore-file').files[0];
  if (!f) return alert('Choisis un fichier de sauvegarde .json');
  if (!confirm('Remplacer toute la config actuelle par cette sauvegarde ?')) return;
  const text = await f.text();
  const r = await fetch('/api/restore', { method: 'POST', body: text }).then(x => x.json()).catch(() => ({}));
  if (r.ok) { toast('Sauvegarde restaurée'); loadStatus(); } else alert('Échec de la restauration (fichier invalide ?)');
};

/* ── Statut global (polling) ── */
let statusCache = { volets: [], rf: [], remotes: {} };
function fillVoletPickers(volets) {
  const ids = (volets || []).map(v => v.id);
  const sel = $('#calib-id');
  if (sel) { const cur = sel.value; sel.innerHTML = ids.map(id => `<option>${esc(id)}</option>`).join(''); if (ids.includes(cur)) sel.value = cur; }
  const dl = $('#volet-list'); if (dl) dl.innerHTML = ids.map(id => `<option value="${esc(id)}">`).join('');
}
function wifiStatusLine(w) {
  const el = $('#wifi-status'); if (!el) return;
  if (w && w.connected) {
    el.className = 'statline ok';
    el.innerHTML = `<span class="dot"></span><b>Connecté</b> à « ${esc(w.ssid)} » ${sig(w.rssi)}<span class="r">${esc(w.ip || '')}</span>`;
  } else {
    el.className = 'statline bad';
    el.innerHTML = `<span class="dot"></span><b>Non connecté</b> : le boîtier est en point d'accès de configuration`;
  }
}
function mqttStatusLine(ok) {
  const el = $('#mqtt-status'); if (!el) return;
  el.className = 'statline ' + (ok ? 'ok' : 'bad');
  el.innerHTML = `<span class="dot"></span><b>${ok ? 'Connecté' : 'Déconnecté'}</b>${ok ? '' : '<span class="r">aucun broker</span>'}`;
}
async function loadStatus() {
  const s = await api('/api/status').catch(() => ({ volets: [], rf: [], remotes: {} }));
  statusCache = s;
  const rf = s.rf || [];
  renderVolets(s.volets || [], rf);
  fillVoletPickers(s.volets || []);
  if ((location.hash || '').includes('/calib') && !calibLive) fillCalib();   /* affiche les temps enregistres */
  if (!learning) { renderVoletPicker(); renderLearnSlots(); }
  wifiStatusLine(s.wifi); mqttStatusLine(s.mqtt);
  const ci = $('#calib-info'); if (ci) ci.hidden = !!s.listening;   /* bandeau visible seulement si option OFF */
}
/* Volet auquel une telecommande (serial) est rattachee, sinon null. */
function voletForSerial(s) { for (const v of (statusCache.volets || [])) if ((v.serials || []).includes(s)) return v.id; return null; }
/* Nom a afficher pour un serial : nom telecommande > nom volet > le serial brut. */
function frameName(s) { return (statusCache.remotes || {})[s] || voletForSerial(s) || s; }
/* Libelle du bouton (Haut/Bas/Stop) s'il correspond a une commande apprise d'un volet, sinon null. */
const BTN_LABEL = { up: 'Haut', down: 'Bas', stop: 'Stop' };
function buttonLabel(serial, buttonHex) {
  const b = parseInt(buttonHex, 16);
  for (const v of (statusCache.volets || [])) {
    if (!(v.serials || []).includes(serial)) continue;
    const c = v.cmd || {};
    for (const k of ['up', 'down', 'stop']) if (c[k] && c[k].b === b) return BTN_LABEL[k];
  }
  return null;
}
/* Une ligne de trame : heure reelle + nom cliquable (vers Telecommandes) + bouton nomme si connu. */
function rfRow(f) {
  const ts = f.t > 1600000000
    ? new Date(f.t * 1000).toLocaleString('fr-FR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit' })
    : '—';
  const name = frameName(f.serial);
  const vid = voletForSerial(f.serial);   /* volet rattache -> lien qui le selectionne dans l'apprentissage */
  const nameCell = vid
    ? `<a href="#remotes/learn" class="frame-link" data-volet="${esc(vid)}" title="${esc(f.serial)} — paramétrer ${esc(vid)}">${esc(name)}</a>`
    : (name !== f.serial
        ? `<a href="#remotes/learn" title="${esc(f.serial)} — Télécommandes">${esc(name)}</a>`
        : `<span class="m" title="télécommande non nommée">${esc(f.serial)}</span>`);
  const bl = buttonLabel(f.serial, f.button);
  const btnCell = bl ? `<span class="badge">${esc(bl)}</span>` : `<span class="badge m">0x${esc(f.button)}</span>`;
  return `<tr><td class="m">${ts}</td><td>${nameCell}</td>
    <td>${btnCell}</td><td class="m">0x${esc(f.hop)}</td><td class="m">${f.rssi} dBm</td>
    <td><button class="replay" title="Rejouer cette trame" data-s="${esc(f.serial)}" data-h="${esc(f.hop)}">▶</button></td></tr>`;
}
/* Onglet RF : trames du ring, chargees par pages (scroll infini). Trie serveur par date. */
let rfOffset = 0, rfTotal = 0, rfLoading = false;
const RF_PAGE = 50;
function updateRfFooter() {
  const f = $('#rf-footer'); if (!f) return;
  f.textContent = rfTotal
    ? `${Math.min(rfOffset, rfTotal)} / ${rfTotal} trame(s)` + (rfOffset < rfTotal ? ' — défile pour charger la suite' : '')
    : 'Aucune trame captée.';
}
async function loadRf(reset) {
  const tb = $('#rf'); if (!tb || rfLoading) return;
  rfLoading = true;
  if (reset) rfOffset = 0;
  const d = await api(`/api/rf?offset=${rfOffset}&limit=${RF_PAGE}`).catch(() => null);
  rfLoading = false;
  if (!d || !Array.isArray(d.frames)) return;
  rfTotal = d.total || 0;
  const html = d.frames.map(rfRow).join('');
  if (reset) tb.innerHTML = html; else tb.insertAdjacentHTML('beforeend', html);
  rfOffset += d.frames.length;
  updateRfFooter();
}
/* Scroll infini : charge la page suivante quand on approche du bas du conteneur (hauteur limitee). */
{
  const wrap = $('.rf-scroll');
  if (wrap) wrap.addEventListener('scroll', () => {
    if (!rfLoading && rfOffset < rfTotal && wrap.scrollTop + wrap.clientHeight >= wrap.scrollHeight - 48) loadRf(false);
  });
}
/* Rejouer une trame captee : renvoie la trame brute (serial+hop) au boitier. Delegation (le tbody est re-rendu). */
const rfBody = $('#rf');
if (rfBody) rfBody.addEventListener('click', async (e) => {
  /* clic sur un nom rattache a un volet -> selectionne ce volet dans l'apprentissage (le href navigue). */
  const a = e.target.closest('a.frame-link');
  if (a) { activeVolet = a.dataset.volet; setTimeout(() => { renderVoletPicker(); renderLearnSlots(); }, 0); return; }
  const b = e.target.closest('button.replay'); if (!b) return;
  b.disabled = true;
  const r = await api('/api/rf/replay', { method: 'POST', body: JSON.stringify({ serial: b.dataset.s, hop: b.dataset.h }) }).catch(() => null);
  toast(r && r.ok ? 'Trame rejouée' : 'Échec du rejeu');
  setTimeout(() => { b.disabled = false; }, 800);
});
applyRoute();
loadStatus();
setInterval(loadStatus, 3000);
