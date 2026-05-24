let ws;
const MAX_LOG_LINES = 200;
let autoScroll = true;

// ── WebSocket ──────────────────────────────────────────────────────────────

function connectWS() {
  const proto = (location.protocol === 'https:') ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws');

  ws.onopen = () => { console.log('WS connected'); };

  ws.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data);
      if (data.type === 'log') {
        appendLog(data.msg);
      } else if (data.type === 'history') {
        data.msgs.forEach(msg => appendLog(msg));
      } else {
        updateUI(data);
      }
    } catch (e) {
      console.warn('Bad JSON:', ev.data);
    }
  };

  ws.onclose = () => {
    console.log('WS closed, retrying in 5s...');
    setTimeout(connectWS, 5000);
  };
}

function updateUI(data) {
  const statusEl = document.getElementById('dspStatus');
  statusEl.innerText = data.dspConnected ? 'Connected' : 'Disconnected';
  statusEl.classList.toggle('status-green', data.dspConnected);
  statusEl.classList.toggle('status-red', !data.dspConnected);

  document.getElementById('currentPreset').innerText = data.currentPreset || 'N/A';

  for (let i = 1; i <= 6; i++) {
    document.getElementById('btn' + i).classList.toggle('active', i === data.currentPreset);
  }
}

// Only locks the preset/refresh buttons, not anything inside the modal
function lockButtons() {
  document.querySelectorAll('.btn-container button, .refresh-btn').forEach(btn => {
    btn.disabled = true;
    btn.classList.add('countdown');
  });
  setTimeout(() => {
    document.querySelectorAll('.btn-container button, .refresh-btn').forEach(btn => {
      btn.disabled = false;
      btn.classList.remove('countdown');
    });
  }, 2000);
}

function requestCurrentPreset() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  lockButtons();
  ws.send(JSON.stringify({ cmd: 'getPreset' }));
}

function setPreset(num) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  lockButtons();
  ws.send(JSON.stringify({ cmd: 'setPreset', preset: num }));
}

// ── Serial monitor ─────────────────────────────────────────────────────────

function appendLog(msg) {
  const log = document.getElementById('serialLog');
  const line = document.createElement('div');
  const ts = new Date().toTimeString().slice(0, 8);
  line.textContent = '[' + ts + '] ' + msg;
  log.appendChild(line);
  while (log.children.length > MAX_LOG_LINES) log.removeChild(log.firstChild);
  if (autoScroll) log.scrollTop = log.scrollHeight;
}

function clearLog() {
  document.getElementById('serialLog').innerHTML = '';
}

function toggleAutoScroll(checkbox) {
  autoScroll = checkbox.checked;
}

function toggleSerialMonitor(btn) {
  const log = document.getElementById('serialLog');
  const hidden = log.style.display === 'none';
  log.style.display = hidden ? '' : 'none';
  btn.textContent = hidden ? 'Hide' : 'Show';
}

// ── Settings modal ─────────────────────────────────────────────────────────

function openSettings() {
  document.getElementById('settingsModal').classList.add('modal-open');
  fetchDeviceInfo();
  // Sync toggle to current theme
  document.getElementById('darkModeToggle').checked =
    document.documentElement.getAttribute('data-theme') === 'dark';
}

function closeSettings() {
  document.getElementById('settingsModal').classList.remove('modal-open');
}

function handleOverlayClick(e) {
  if (e.target === document.getElementById('settingsModal')) closeSettings();
}

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') closeSettings();
});

// ── Device info ────────────────────────────────────────────────────────────

function fetchDeviceInfo() {
  const el = document.getElementById('deviceInfo');
  el.innerHTML = '<div class="info-loading">Loading&hellip;</div>';
  fetch('/api/info')
    .then(r => r.json())
    .then(info => {
      el.innerHTML = `
        <div class="info-grid">
          <div class="info-item">
            <div class="info-label">IP Address</div>
            <div class="info-value">${info.ip}</div>
          </div>
          <div class="info-item">
            <div class="info-label">Hostname</div>
            <div class="info-value">${info.hostname}.local</div>
          </div>
          <div class="info-item">
            <div class="info-label">Chip</div>
            <div class="info-value">${info.chip}</div>
          </div>
          <div class="info-item">
            <div class="info-label">CPU</div>
            <div class="info-value">${info.cpuFreqMHz} MHz</div>
          </div>
          <div class="info-item">
            <div class="info-label">Flash</div>
            <div class="info-value">${info.flashSizeMB} MB</div>
          </div>
          <div class="info-item">
            <div class="info-label">Free Heap</div>
            <div class="info-value">${info.freeHeapKB} KB</div>
          </div>
          <div class="info-item" style="grid-column: 1 / -1;">
            <div class="info-label">Uptime</div>
            <div class="info-value">${formatUptime(info.uptimeSec)}</div>
          </div>
        </div>`;
    })
    .catch(() => {
      el.innerHTML = '<div class="info-loading">Failed to load device info.</div>';
    });
}

function formatUptime(sec) {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

// ── Dark mode ──────────────────────────────────────────────────────────────

function toggleDarkMode(checkbox) {
  const theme = checkbox.checked ? 'dark' : 'light';
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('theme', theme);
}

// ── OTA upload ─────────────────────────────────────────────────────────────

const otaReady = { firmware: false, filesystem: false };

function updateRebootButton() {
  const ready = otaReady.firmware || otaReady.filesystem;
  const btn   = document.getElementById('rebootBtn');
  const hint  = document.getElementById('rebootHint');
  btn.disabled = !ready;
  if (ready) {
    const parts = [];
    if (otaReady.firmware)   parts.push('firmware');
    if (otaReady.filesystem) parts.push('storage');
    hint.textContent = parts.join(' & ') + ' ready — reboot to apply.';
  } else {
    hint.textContent = 'Upload at least one image above, then reboot to apply.';
  }
}

function rebootDevice() {
  const btn      = document.getElementById('rebootBtn');
  const statusEl = document.getElementById('rebootStatus');
  btn.disabled = true;
  statusEl.textContent = 'Rebooting…';
  statusEl.className = 'upload-status';
  fetch('/api/restart', { method: 'POST' })
    .then(r => r.json())
    .then(() => {
      statusEl.textContent = 'Device is rebooting — reconnecting…';
      statusEl.className = 'upload-status success';
      otaReady.firmware = false;
      otaReady.filesystem = false;
    })
    .catch(() => {
      statusEl.textContent = 'Reboot request failed.';
      statusEl.className = 'upload-status error';
      btn.disabled = false;
    });
}

function uploadOTA(type) {
  const isFirmware = type === 'firmware';
  const fileInput  = document.getElementById(isFirmware ? 'firmwareFile'    : 'fsFile');
  const fillEl     = document.getElementById(isFirmware ? 'firmwareProgress': 'fsProgress');
  const statusEl   = document.getElementById(isFirmware ? 'firmwareStatus'  : 'fsStatus');
  const btnEl      = document.getElementById(isFirmware ? 'firmwareUploadBtn': 'fsUploadBtn');

  statusEl.className = 'upload-status';
  statusEl.textContent = '';

  if (!fileInput.files.length) {
    statusEl.textContent = 'Please select a .bin file first.';
    statusEl.className = 'upload-status error';
    return;
  }

  const formData = new FormData();
  formData.append('file', fileInput.files[0]);

  btnEl.disabled = true;
  fillEl.style.width = '0%';
  statusEl.textContent = 'Uploading…';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/' + type);

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round(e.loaded / e.total * 100);
      fillEl.style.width = pct + '%';
      statusEl.textContent = 'Uploading… ' + pct + '%';
    }
  };

  xhr.onload = () => {
    btnEl.disabled = false;
    try {
      const resp = JSON.parse(xhr.responseText);
      if (resp.ok) {
        fillEl.style.width = '100%';
        statusEl.textContent = 'Flashed successfully — reboot when ready.';
        statusEl.className = 'upload-status success';
        otaReady[type] = true;
        updateRebootButton();
      } else {
        statusEl.textContent = 'Error: ' + (resp.error || 'Unknown error');
        statusEl.className = 'upload-status error';
      }
    } catch (_) {
      statusEl.textContent = 'Unexpected response from device.';
      statusEl.className = 'upload-status error';
    }
  };

  xhr.onerror = () => {
    btnEl.disabled = false;
    statusEl.textContent = 'Upload failed — check connection.';
    statusEl.className = 'upload-status error';
  };

  xhr.send(formData);
}

// ── Init ───────────────────────────────────────────────────────────────────

connectWS();
