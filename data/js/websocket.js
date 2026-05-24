// ── WebSocket & preset control ──────────────────────────────────────────────

let ws;

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

// ── Init ─────────────────────────────────────────────────────────────────────

connectWS();
