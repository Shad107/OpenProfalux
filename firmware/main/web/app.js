'use strict';
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const api = (u, o) => fetch(u, o).then(r => r.ok ? r.json().catch(() => ({})) : Promise.reject(r.status));
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

/* ── Onglets + thème ── */
$$('.tab').forEach(t => t.onclick = () => {
  $$('.tab').forEach(x => x.classList.toggle('active', x === t));
  $$('.panel').forEach(p => p.classList.toggle('active', p.dataset.p === t.dataset.t));
  if (t.dataset.t === 'sys') loadConfig();
});
/* Sous-onglets (generique, scope au panneau parent) */
$$('.subtab').forEach(t => t.onclick = () => {
  const sec = t.closest('.panel');
  $$('.subtab', sec).forEach(x => x.classList.toggle('active', x === t));
  $$('.subpanel', sec).forEach(p => p.classList.toggle('active', p.dataset.sp === t.dataset.s));
  if (['wifi', 'mqtt', 'ota'].includes(t.dataset.s)) loadConfig();
});
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
  for (const v of list) {
    const r = rssiForSerials(v.serials, rf);
    const el = document.createElement('div');
    el.className = 'card volet';
    el.innerHTML = `
      <div class="top"><span class="name">🪟 ${esc(v.id)}</span><span class="pct">${v.position ?? '?'}%</span></div>
      <div class="dpad"><button data-cmd="up">▲</button><button data-cmd="stop">■</button><button data-cmd="down">▼</button></div>
      <div class="slat" style="--p:${v.position ?? 50}"></div>
      <div class="serials">serials : ${(v.serials || []).map(s => `<code>${esc(remoteName(s))}</code>`).join(' ') || '—'}
        ${r != null ? `<span style="margin-left:6px">· reçu ${sig(r)}</span>` : ''}</div>`;
    el.querySelectorAll('[data-cmd]').forEach(b =>
      b.onclick = () => api('/api/shutter', { method: 'POST', body: JSON.stringify({ id: v.id, cmd: b.dataset.cmd }) }));
    el.querySelector('.slat').onclick = e => {
      const p = Math.round(100 * (e.offsetX / e.currentTarget.offsetWidth));
      api('/api/shutter', { method: 'POST', body: JSON.stringify({ id: v.id, cmd: 'pos', value: p }) });
    };
    box.appendChild(el);
  }
}

/* ── Apprentissage ── */
let learnAct = null, learnBits = null, learnSerial = null;
$$('#acts button').forEach(b => b.onclick = () => {
  learnAct = b.dataset.a;
  $$('#acts button').forEach(x => x.classList.toggle('on', x === b));
  $('#listen-btn').disabled = false; $('#listen-btn').textContent = `Écouter (${learnAct})`;
});
$('#listen-btn').onclick = async () => {
  $('#listen-btn').textContent = 'À l’écoute… appuie sur la télécommande'; $('#listen-btn').disabled = true;
  await api('/api/learn/start', { method: 'POST' });
  const poll = setInterval(async () => {
    const r = await api('/api/learn/poll').catch(() => null);
    if (r && r.bits) {
      clearInterval(poll); learnBits = r.bits; learnSerial = r.serial;
      $('#cap-info').textContent = `serial ${r.serial} · bouton 0x${r.button} · ${r.rssi} dBm`;
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
  $('#learn-result').hidden = true; toast('Commande affectée'); loadStatus();
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
let calT = {}, calStart = 0;
function chrono(dir) { calStart = performance.now(); $(`#cal-${dir}-start`).disabled = true; $(`#cal-${dir}-stop`).disabled = false; }
function chronoStop(dir) {
  calT[dir] = Math.round(performance.now() - calStart);
  $(`#cal-${dir}-t`).textContent = (calT[dir] / 1000).toFixed(1) + 's';
  $(`#cal-${dir}-start`).disabled = false; $(`#cal-${dir}-stop`).disabled = true;
  $('#cal-save').disabled = !(calT.up && calT.down);
}
const calId = () => $('#calib-id').value.trim();
$('#cal-down-start').onclick = () => { chrono('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'down' }) }); };
$('#cal-down-stop').onclick  = () => { chronoStop('down'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'stop' }) }); };
$('#cal-up-start').onclick   = () => { chrono('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'up' }) }); };
$('#cal-up-stop').onclick    = () => { chronoStop('up'); api('/api/shutter', { method:'POST', body: JSON.stringify({ id: calId(), cmd:'stop' }) }); };
$('#cal-save').onclick = () => { api('/api/calibrate', { method: 'POST',
  body: JSON.stringify({ id: calId(), travel_up_ms: calT.up, travel_down_ms: calT.down }) }); toast('Calibration enregistrée'); };

/* ── Config : Wi-Fi / MQTT / Système (sauvegardes séparées, partielles) ── */
async function loadConfig() {
  const c = await api('/api/config').catch(() => ({}));
  $('#wifi-ssid').value = c.wifi_ssid || '';
  $('#mqtt-uri').value = c.mqtt_uri || ''; $('#mqtt-user').value = c.mqtt_user || '';
  $('#sys-device').value = c.device || ''; $('#sys-logframes').checked = !!c.log_frames;
  const st = await api('/api/ota/status').catch(() => ({}));
  $('#ota-version').textContent = st.version || '—';
}
$('#wifi-save').onclick = async () => {
  const b = { wifi_ssid: $('#wifi-ssid').value.trim(), reboot: $('#wifi-reboot').checked };
  if ($('#wifi-pass').value) b.wifi_pass = $('#wifi-pass').value;
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  b.reboot ? toast('Wi-Fi enregistré — redémarrage…') : savedBtn($('#wifi-save'), 'Enregistrer le Wi-Fi');
};
$('#mqtt-save').onclick = async () => {
  const b = { mqtt_uri: $('#mqtt-uri').value.trim(), mqtt_user: $('#mqtt-user').value.trim(), reboot: true };
  if ($('#mqtt-pass').value) b.mqtt_pass = $('#mqtt-pass').value;
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  toast('MQTT enregistré — redémarrage…');
};
$('#sys-save').onclick = async () => {
  const b = { device: $('#sys-device').value.trim(), log_frames: $('#sys-logframes').checked, reboot: $('#sys-reboot').checked };
  await api('/api/config', { method: 'POST', body: JSON.stringify(b) }).catch(() => {});
  b.reboot ? toast('Enregistré — redémarrage…') : savedBtn($('#sys-save'), 'Enregistrer');
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
    el.innerHTML = `<span class="dot"></span><b>Non connecté</b> — le boîtier est en point d'accès de configuration`;
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
  renderRemotes(s.remotes || {}, rf);
  fillVoletPickers(s.volets || []);
  wifiStatusLine(s.wifi); mqttStatusLine(s.mqtt);
  const tb = $('#rf'); if (tb) tb.innerHTML = rf.map(f =>
    `<tr><td class="m">${f.t}</td><td title="${esc(f.serial)}">${esc(remoteName(f.serial))}</td>
      <td><span class="badge">0x${esc(f.button)}</span></td><td class="m">0x${esc(f.hop)}</td><td class="m">${f.rssi} dBm</td></tr>`).join('');
}
loadStatus();
setInterval(loadStatus, 3000);
