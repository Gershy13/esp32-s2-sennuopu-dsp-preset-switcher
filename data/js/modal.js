// ── Settings modal ──────────────────────────────────────────────────────────

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

// ── Device info ─────────────────────────────────────────────────────────────

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

// ── Dark mode ────────────────────────────────────────────────────────────────

function toggleDarkMode(checkbox) {
  const theme = checkbox.checked ? 'dark' : 'light';
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('theme', theme);
}

// ── OTA upload ───────────────────────────────────────────────────────────────

const otaReady      = { firmware: false, filesystem: false };
const assignedFiles = { firmware: null, filesystem: null };

function triggerFileDialog() {
  document.getElementById('otaFileInput').click();
}

function handleFileSelect(e) {
  assignFiles(e.target.files);
  e.target.value = ''; // allow re-selecting same file
}

function handleDragOver(e) {
  e.preventDefault();
  document.getElementById('dropZone').classList.add('drag-over');
}

function handleDragLeave(e) {
  e.preventDefault();
  document.getElementById('dropZone').classList.remove('drag-over');
}

function handleDrop(e) {
  e.preventDefault();
  document.getElementById('dropZone').classList.remove('drag-over');
  assignFiles(e.dataTransfer.files);
}

function assignFiles(files) {
  Array.from(files).forEach(file => {
    const name = file.name.toLowerCase();
    if (name.includes('littlefs') || name.includes('spiffs')) {
      assignedFiles.filesystem = file;
    } else if (name.endsWith('.bin')) {
      assignedFiles.firmware = file;
    }
  });
  updateSlotUI();
}

function updateSlotUI() {
  setSlot('firmware',   assignedFiles.firmware);
  setSlot('filesystem', assignedFiles.filesystem);
  document.getElementById('uploadAllBtn').disabled =
    !assignedFiles.firmware && !assignedFiles.filesystem;
}

function setSlot(type, file) {
  const isFw     = type === 'firmware';
  const slotEl   = document.getElementById(isFw ? 'firmwareSlot'          : 'fsSlot');
  const fileEl   = document.getElementById(isFw ? 'firmwareSlotFile'      : 'fsSlotFile');
  const clearBtn = document.getElementById(isFw ? 'firmwareClearBtn'      : 'fsClearBtn');
  const statusEl = document.getElementById(isFw ? 'firmwareStatus'        : 'fsStatus');
  const trackEl  = document.getElementById(isFw ? 'firmwareProgressTrack' : 'fsProgressTrack');
  const fillEl   = document.getElementById(isFw ? 'firmwareProgress'      : 'fsProgress');

  if (file) {
    fileEl.textContent = file.name;
    clearBtn.style.display = '';
    slotEl.classList.add('slot-assigned');
  } else {
    fileEl.textContent = 'Not assigned';
    clearBtn.style.display = 'none';
    slotEl.classList.remove('slot-assigned');
    statusEl.textContent = '';
    statusEl.className = 'upload-status';
    trackEl.style.display = 'none';
    fillEl.style.width = '0%';
  }
}

function clearSlot(type) {
  assignedFiles[type] = null;
  updateSlotUI();
}

async function uploadAll() {
  const btn = document.getElementById('uploadAllBtn');
  btn.disabled = true;
  if (assignedFiles.firmware)   await doUpload('firmware');
  if (assignedFiles.filesystem) await doUpload('filesystem');
  btn.disabled = !assignedFiles.firmware && !assignedFiles.filesystem;
  updateRebootButton();
}

function doUpload(type) {
  const isFw     = type === 'firmware';
  const file     = assignedFiles[type];
  const fillEl   = document.getElementById(isFw ? 'firmwareProgress'      : 'fsProgress');
  const statusEl = document.getElementById(isFw ? 'firmwareStatus'        : 'fsStatus');
  const trackEl  = document.getElementById(isFw ? 'firmwareProgressTrack' : 'fsProgressTrack');

  trackEl.style.display = '';
  fillEl.style.width = '0%';
  statusEl.textContent = 'Uploading…';
  statusEl.className = 'upload-status';

  return new Promise(resolve => {
    const formData = new FormData();
    formData.append('file', file);

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
      try {
        const resp = JSON.parse(xhr.responseText);
        if (resp.ok) {
          fillEl.style.width = '100%';
          statusEl.textContent = 'Flashed ✓';
          statusEl.className = 'upload-status success';
          otaReady[type] = true;
        } else {
          statusEl.textContent = 'Error: ' + (resp.error || 'Unknown');
          statusEl.className = 'upload-status error';
        }
      } catch (_) {
        statusEl.textContent = 'Unexpected response.';
        statusEl.className = 'upload-status error';
      }
      resolve();
    };

    xhr.onerror = () => {
      statusEl.textContent = 'Upload failed — check connection.';
      statusEl.className = 'upload-status error';
      resolve();
    };

    xhr.send(formData);
  });
}

// ── Reboot ───────────────────────────────────────────────────────────────────

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
    hint.textContent = hint.textContent.charAt(0).toUpperCase() + hint.textContent.slice(1);
  } else {
    hint.textContent = 'Upload at least one image above, then reboot to apply.';
  }
}

function rebootDevice() {
  const btn      = document.getElementById('rebootBtn');
  const statusEl = document.getElementById('rebootStatus');
  btn.disabled = true;
  statusEl.textContent = 'Sending reboot command…';
  statusEl.className = 'upload-status';
  fetch('/api/restart', { method: 'POST' })
    .then(r => r.json())
    .then(() => {
      otaReady.firmware = false;
      otaReady.filesystem = false;
      pollForReconnect(statusEl);
    })
    .catch(() => {
      statusEl.textContent = 'Reboot request failed.';
      statusEl.className = 'upload-status error';
      btn.disabled = false;
    });
}

function pollForReconnect(statusEl) {
  let tick = 0;
  const dots = ['', '.', '..', '...'];
  const interval = setInterval(() => {
    tick++;
    statusEl.textContent = 'Waiting for device to come back online' + dots[tick % 4];
    fetch('/api/info', { cache: 'no-store' })
      .then(r => {
        if (r.ok) {
          clearInterval(interval);
          statusEl.textContent = 'Device back online — reloading…';
          statusEl.className = 'upload-status success';
          setTimeout(() => location.reload(), 800);
        }
      })
      .catch(() => {}); // still offline — keep polling
  }, 1500);
}
