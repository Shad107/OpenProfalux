'use strict';
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const api = (u, o) => fetch(u, o).then(r => r.ok ? r.json().catch(() => ({})) : Promise.reject(r.status));

/* ── Tabs + thème ── */
$$('.tab').forEach(t => t.onclick = () => {
  $$('.tab').forEach(x => x.classList.toggle('active', x === t));
  $$('.panel').forEach(p => p.classList.toggle('active', p.id === t.dataset.tab));
});
$('#theme-btn').onclick = () => {
  const r = document.documentElement, cur = r.getAttribute('data-theme');
  r.setAttribute('data-theme', cur === 'dark' ? 'light' : 'dark');
};

/* ── Volets : rendu + contrôle ── */
function renderVolets(list) {
  const box = $('#volets'); box.innerHTML = '';
  $('#control-empty').hidden = list.length > 0;
  for (const v of list) {
    const el = document.createElement('div');
    el.className = 'card volet';
    el.innerHTML = `
      <div class="volet-head"><h2>${v.id}</h2><span class="pos">${v.position ?? '?'}%</span></div>
      <div class="btn-row">
        <button data-cmd="up">▲</button>
        <button data-cmd="stop">■</button>
        <button data-cmd="down">▼</button>
      </div>
      <input type="range" min="0" max="100" value="${v.position ?? 50}" class="posslider">
      <p class="hint">serials : ${(v.serials || []).join(', ') || '—'}</p>`;
    el.querySelectorAll('[data-cmd]').forEach(b =>
      b.onclick = () => api('/api/shutter', { method: 'POST', body: JSON.stringify({ id: v.id, cmd: b.dataset.cmd }) }));
    el.querySelector('.posslider').onchange = e =>
      api('/api/shutter', { method: 'POST', body: JSON.stringify({ id: v.id, cmd: 'pos', value: +e.target.value }) });
    box.appendChild(el);
  }
}

/* ── Apprentissage : capture ── */
let learnAct = null, learnBits = null, learnSerial = null;
$$('#learn .act').forEach(b => b.onclick = () => {
  learnAct = b.dataset.act;
  $$('#learn .act').forEach(x => x.classList.toggle('active', x === b));
  $('#listen-btn').disabled = false;
  $('#listen-btn').textContent = `Écouter (${learnAct})`;
});
$('#listen-btn').onclick = async () => {
  $('#listen-btn').textContent = 'À l’écoute… appuie sur la télécommande'; $('#listen-btn').disabled = true;
  await api('/api/learn/start', { method: 'POST' });
  const poll = setInterval(async () => {
    const r = await api('/api/learn/poll').catch(() => null);
    if (r && r.bits) {
      clearInterval(poll); learnBits = r.bits; learnSerial = r.serial;
      $('#cap-info').textContent = `serial ${r.serial} · bouton ${r.button} · rssi ${r.rssi}`;
      $('#cap-name').value = (statusCache.remotes || {})[r.serial] || '';
      $('#learn-result').hidden = false;
      $('#listen-btn').textContent = `Écouter (${learnAct})`; $('#listen-btn').disabled = false;
    }
  }, 500);
};
$('#assign-btn').onclick = async () => {
  const name = $('#cap-name').value.trim();
  if (name && learnSerial) await api('/api/remote', { method: 'POST', body: JSON.stringify({ serial: learnSerial, name }) });
  await api('/api/learn/assign', { method: 'POST',
    body: JSON.stringify({ id: $('#learn-id').value.trim(), action: learnAct, bits: learnBits }) });
  $('#learn-result').hidden = true; loadStatus();
};

/* ── Télécommandes : nommage ── */
function renderRemotes(map) {
  const box = $('#remotes-list'); if (!box) return;
  const entries = Object.entries(map || {});
  $('#remotes-empty').hidden = entries.length > 0;
  box.innerHTML = '';
  for (const [serial, name] of entries) {
    const row = document.createElement('div'); row.className = 'btn-row';
    row.innerHTML = `<code>${serial}</code><input value="${name || ''}" placeholder="nom…" data-serial="${serial}" style="flex:2">`;
    const inp = row.querySelector('input');
    inp.onchange = () => api('/api/remote', { method: 'POST', body: JSON.stringify({ serial, name: inp.value.trim() }) });
    box.appendChild(row);
  }
}
const remoteName = s => (statusCache.remotes || {})[s] || s;

/* ── Calibration : chronos ── */
let calT = {}, calStart = 0;
function chrono(dir) {
  calStart = performance.now();
  $(`#cal-${dir}-start`).disabled = true; $(`#cal-${dir}-stop`).disabled = false;
}
function chronoStop(dir) {
  calT[dir] = Math.round(performance.now() - calStart);
  $(`#cal-${dir}-t`).textContent = (calT[dir] / 1000).toFixed(1) + 's';
  $(`#cal-${dir}-start`).disabled = false; $(`#cal-${dir}-stop`).disabled = true;
  $('#cal-save').disabled = !(calT.up && calT.down);
}
$('#cal-down-start').onclick = () => { chrono('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: $('#calib-id').value.trim(), cmd:'down' }) }); };
$('#cal-down-stop').onclick  = () => { chronoStop('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: $('#calib-id').value.trim(), cmd:'stop' }) }); };
$('#cal-up-start').onclick   = () => { chrono('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: $('#calib-id').value.trim(), cmd:'up' }) }); };
$('#cal-up-stop').onclick    = () => { chronoStop('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: $('#calib-id').value.trim(), cmd:'stop' }) }); };
$('#cal-save').onclick = () => api('/api/calibrate', { method: 'POST',
  body: JSON.stringify({ id: $('#calib-id').value.trim(), travel_up_ms: calT.up, travel_down_ms: calT.down }) });

/* ── Système : config + OTA ── */
async function loadConfig() {
  const c = await api('/api/config').catch(() => ({}));
  $('#cfg-device').value = c.device || ''; $('#cfg-ssid').value = c.wifi_ssid || '';
  $('#cfg-mqtt').value = c.mqtt_uri || ''; $('#cfg-muser').value = c.mqtt_user || '';
  $('#cfg-logframes').checked = !!c.log_frames;
  const st = await api('/api/ota/status').catch(() => ({}));
  $('#ota-version').textContent = st.version || '—';
}
$$('.tab').forEach(t => { if (t.dataset.tab === 'system') t.addEventListener('click', loadConfig); });
$('#mqtt-detect').onclick = async () => {
  const b = $('#mqtt-detect'); b.textContent = 'Recherche…'; b.disabled = true;
  const r = await api('/api/mqtt/discover').catch(() => ({}));
  if (r.uri) { $('#cfg-mqtt').value = r.uri; b.textContent = 'Trouvé : ' + r.uri; }
  else b.textContent = 'Aucun broker trouvé';
  b.disabled = false;
  setTimeout(() => b.textContent = '🔍 Détecter le broker (mDNS)', 2500);
};
$('#cfg-save').onclick = async () => {
  const b = { device: $('#cfg-device').value, wifi_ssid: $('#cfg-ssid').value,
              mqtt_uri: $('#cfg-mqtt').value, mqtt_user: $('#cfg-muser').value,
              log_frames: $('#cfg-logframes').checked,
              reboot: $('#cfg-reboot').checked };
  if ($('#cfg-wpass').value) b.wifi_pass = $('#cfg-wpass').value;
  if ($('#cfg-mpass').value) b.mqtt_pass = $('#cfg-mpass').value;
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) });
  $('#cfg-save').textContent = 'Enregistré ✓'; setTimeout(() => $('#cfg-save').textContent = 'Enregistrer', 1500);
};
$('#ota-btn').onclick = async () => {
  const f = $('#ota-file').files[0]; if (!f) return alert('Choisis un .bin');
  $('#ota-prog').hidden = false; $('#ota-btn').disabled = true;
  const buf = await f.arrayBuffer();
  const up = fetch('/api/ota/upload', { method: 'POST', body: buf });
  const poll = setInterval(async () => {
    const s = await api('/api/ota/status').catch(() => null);
    if (s) { $('#ota-bar').value = s.total ? Math.round(100 * s.written / s.total) : 0; $('#ota-msg').textContent = s.msg || ''; }
    if (s && (s.state === 4 || s.state === 99)) clearInterval(poll);
  }, 800);
  up.catch(() => {}); /* la connexion tombe au reboot, normal */
};
$('#ota-rollback').onclick = async () => {
  if (!confirm('Revenir à la version précédente et redémarrer ?')) return;
  await api('/api/ota/rollback', { method: 'POST' }).catch(() => {});
};

/* ── Sauvegarde : restauration ── */
$('#restore-btn').onclick = async () => {
  const f = $('#restore-file').files[0];
  if (!f) return alert('Choisis un fichier de sauvegarde .json');
  if (!confirm('Remplacer toute la config actuelle par cette sauvegarde ?')) return;
  const text = await f.text();
  const r = await fetch('/api/restore', { method: 'POST', body: text }).then(x => x.json()).catch(() => ({}));
  if (r.ok) { $('#restore-btn').textContent = 'Restauré ✓'; loadStatus(); setTimeout(() => $('#restore-btn').textContent = 'Restaurer (remplace la config actuelle)', 1500); }
  else alert('Échec de la restauration (fichier invalide ?)');
};

/* ── Monitor RF + status ── */
let statusCache = { volets: [], rf: [], remotes: {} };
async function loadStatus() {
  const s = await api('/api/status').catch(() => ({ volets: [], rf: [], remotes: {} }));
  statusCache = s;
  renderVolets(s.volets || []);
  renderRemotes(s.remotes || {});
  const tb = $('#rf-table tbody'); if (tb) {
    tb.innerHTML = (s.rf || []).map(f =>
      `<tr><td>${f.t}</td><td title="${f.serial}">${remoteName(f.serial)}</td><td>0x${f.button}</td><td>0x${f.hop}</td><td>${f.rssi}</td></tr>`).join('');
  }
}
loadStatus();
setInterval(loadStatus, 3000);
