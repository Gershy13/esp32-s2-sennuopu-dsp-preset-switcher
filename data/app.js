let ws;

const MAX_LOG_LINES = 200;
let autoScroll = true;

function connectWS() {
  const proto = (location.protocol === 'https:') ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws');

  ws.onopen = () => { console.log('WS connected'); };

  ws.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data);
      if (data.type === 'log') {
        appendLog(data.msg);
      } else {
        updateUI(data);
      }
    } catch (e) {
      console.warn('Bad JSON:', ev.data);
    }
  };

  ws.onclose = () => {
    console.log('WS closed, retrying in 1s...');
    setTimeout(connectWS, 1000);
  };
}

function updateUI(data) {
  const statusEl = document.getElementById('dspStatus');
  statusEl.innerText = data.dspConnected ? "Connected" : "Disconnected";
  statusEl.classList.toggle('status-green', data.dspConnected);
  statusEl.classList.toggle('status-red', !data.dspConnected);

  document.getElementById('currentPreset').innerText = data.currentPreset || 'N/A';

  for (let i = 1; i <= 6; i++) {
    document.getElementById('btn' + i).classList.toggle('active', i === data.currentPreset);
  }
}

function setPreset(num) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;

  const buttons = document.querySelectorAll('button');

  buttons.forEach(btn => {
    btn.disabled = true;
    btn.classList.add('countdown');
  });

  ws.send(JSON.stringify({ cmd: "setPreset", preset: num }));

  setTimeout(() => {
    buttons.forEach(btn => {
      btn.disabled = false;
      btn.classList.remove('countdown');
    });
  }, 2000);
}

function appendLog(msg) {
  const log = document.getElementById('serialLog');
  const line = document.createElement('div');
  const now = new Date();
  const ts = now.toTimeString().slice(0, 8);
  line.textContent = '[' + ts + '] ' + msg;
  log.appendChild(line);

  while (log.children.length > MAX_LOG_LINES) {
    log.removeChild(log.firstChild);
  }

  if (autoScroll) {
    log.scrollTop = log.scrollHeight;
  }
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

connectWS();
