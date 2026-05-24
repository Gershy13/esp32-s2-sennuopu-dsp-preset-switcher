// ── Serial monitor ──────────────────────────────────────────────────────────

const MAX_LOG_LINES = 200;
let autoScroll = true;

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
