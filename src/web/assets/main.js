async function apiGet(path) {
  const r = await fetch(path);
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.error || (path + ': HTTP ' + r.status));
  return body;
}

async function apiPost(path, jsonBody) {
  const opts = { method: 'POST' };
  if (jsonBody !== undefined) {
    opts.headers = { 'Content-Type': 'application/json' };
    opts.body = JSON.stringify(jsonBody);
  }
  const r = await fetch(path, opts);
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.error || (path + ': HTTP ' + r.status));
  return body;
}

async function apiDelete(path) {
  const r = await fetch(path, { method: 'DELETE' });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.error || (path + ': HTTP ' + r.status));
  return body;
}

function formatBytes(n) {
  if (!n || n <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  let v = n;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return v.toFixed(i === 0 ? 0 : 2) + ' ' + units[i];
}

function formatSpeed(bytesPerSec) {
  return formatBytes(bytesPerSec) + '/s';
}

function formatEta(job) {
  const p = job.progress;
  if (!p || p.total_bytes <= 0) return '-';
  const remaining = p.total_bytes - p.downloaded_bytes;
  if (remaining <= 0) return '-';
  const speed = window.__lastSpeed || 0;
  if (speed <= 0) return '-';
  let secs = Math.round(remaining / speed);
  const h = Math.floor(secs / 3600); secs -= h * 3600;
  const m = Math.floor(secs / 60); secs -= m * 60;
  return (h ? h + 'h ' : '') + (m ? m + 'm ' : '') + secs + 's';
}

/* Centralizes the "which phase is this job in, and what bytes/pct go with
 * it" logic shared by the dashboard table and the job detail page -- the
 * API exposes exact done/total byte counts not just for the download
 * itself but also for whichever of verify/repair/extract is currently
 * running (see job_json.c's progress_json()), so the progress bar can
 * show real numbers ("4.3 GB / 12 GB") through every phase, not just
 * while downloading. */
function jobProgressInfo(p) {
  let done = 0, total = 0;

  if (p && p.extracting) { done = p.extract_bytes_done; total = p.extract_bytes_total; }
  else if (p && p.repairing) { done = p.repair_bytes_done; total = p.repair_bytes_total; }
  else if (p && p.verifying) { done = p.verify_bytes_done; total = p.verify_bytes_total; }
  else { done = (p && p.downloaded_bytes) || 0; total = (p && p.total_bytes) || 0; }

  const pct = total > 0 ? Math.min(100, Math.round((done / total) * 100)) : 0;
  const label = formatBytes(done) + ' / ' + formatBytes(total);

  return { pct, done, total, label };
}

function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

/* Log lines come from log_log() as "<date> <time> [LEVEL] message" (see
 * src/log/log.c) -- [WARN ] and [ERROR] are padded/named consistently
 * enough to just substring-match rather than needing a real parser. */
function logLineClass(line) {
  if (line.includes('[ERROR]')) return 'level-error';
  if (line.includes('[WARN')) return 'level-warn';
  if (line.includes('[DEBUG]')) return 'level-debug';
  return 'level-info';
}

/* Renders log lines into a scrollable container, auto-following new lines
 * only while the user is already scrolled near the bottom -- so refreshing
 * every couple seconds doesn't yank them away from something they scrolled
 * up to read. */
function renderLogLines(container, lines) {
  /* A full innerHTML replace tears out every existing .log-line node --
   * if the user currently has text selected inside this container, its
   * Selection anchor/focus nodes go stale mid-drag, and the browser's
   * usual recovery is to collapse or re-anchor the range at the start of
   * whatever replaces it. Visually that's exactly "selection jumps to the
   * top and balloons to cover almost everything" on the very next refresh
   * (this polls every 2s, see index.html). Skipping the re-render while a
   * real (non-collapsed) selection lives in here leaves it alone until the
   * user is done; the next refresh after they click away picks up
   * whatever lines arrived in the meantime. */
  const sel = window.getSelection();
  if (sel && !sel.isCollapsed && sel.rangeCount > 0 && container.contains(sel.anchorNode)) {
    return;
  }

  const nearBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 40;
  container.innerHTML = lines.length
    ? lines.map(l => `<div class="log-line ${logLineClass(l)}">${escapeHtml(l)}</div>`).join('')
    : '<div class="muted">No log entries.</div>';
  if (nearBottom) container.scrollTop = container.scrollHeight;
}

const STATE_META = {
  queued:      ['fa-clock', 'Queued'],
  downloading: ['fa-arrow-down', 'Downloading'],
  paused:      ['fa-circle-pause', 'Paused'],
  verifying:   ['fa-magnifying-glass', 'Verifying'],
  repairing:   ['fa-wrench', 'Repairing'],
  extracting:  ['fa-box-open', 'Extracting'],
  completed:   ['fa-circle-check', 'Completed'],
  failed:      ['fa-triangle-exclamation', 'Failed'],
  cancelled:   ['fa-circle-xmark', 'Cancelled'],
};

function stateBadge(state) {
  const [icon, label] = STATE_META[state] || ['fa-question', state];
  return `<span class="badge ${state}"><i class="fa-solid ${icon}"></i> ${label}</span>`;
}

function progressBarHtml(job) {
  const info = jobProgressInfo(job.progress);
  return `
    <progress value="${info.pct}" max="100"></progress>
    <span class="progress-label">${info.label} (${info.pct}%)</span>
  `;
}

/* Mirrors queue_remove_job()'s own guard (src/queue/queue.c) -- a job in
 * one of these states is a raw pointer the downloader/finalizer threads
 * are actively working on unlocked, so DELETE /api/jobs/:id refuses it
 * (409). Keeping the Remove button hidden for exactly this set, rather
 * than showing it and surfacing that rejection as an alert(), is just UI
 * politeness -- the backend is what actually enforces this. */
const JOB_ACTIVE_STATES = ['downloading', 'verifying', 'repairing', 'extracting'];

function jobActionButtons(job) {
  const btn = (icon, action, label, cls) =>
    `<button type="button" class="icon-btn ${cls || ''}" title="${label}" aria-label="${label}" onclick="jobAction('${job.id}','${action}')"><i class="fa-solid ${icon}"></i></button>`;

  const parts = [];
  if (job.state === 'queued' || job.state === 'downloading') parts.push(btn('fa-pause', 'pause', 'Pause'));
  if (job.state === 'paused') parts.push(btn('fa-play', 'resume', 'Resume'));
  if (job.state === 'failed' || job.state === 'cancelled') parts.push(btn('fa-arrow-rotate-right', 'retry', 'Retry'));
  if (job.state !== 'completed' && job.state !== 'cancelled') parts.push(btn('fa-ban', 'cancel', 'Cancel', 'danger'));
  if (!JOB_ACTIVE_STATES.includes(job.state)) parts.push(btn('fa-trash', 'remove', 'Remove', 'danger'));
  return `<div class="actions">${parts.join('')}</div>`;
}

async function jobAction(id, action) {
  try {
    if (action === 'remove') {
      const ok = await confirmModal('Remove this job from the queue?', {
        title: 'Remove job', confirmLabel: 'Remove', danger: true,
      });
      if (!ok) return;
      await apiDelete('/api/jobs/' + encodeURIComponent(id));
    } else {
      await apiPost('/api/jobs/' + encodeURIComponent(id) + '/' + action);
    }
    if (window.refreshNow) window.refreshNow();
  } catch (e) {
    alert(e.message);
  }
}

/* Wires up one showModal() call's worth of listeners on `dialog` and
 * resolves/settles exactly once, from whichever of these happens first:
 * - a [data-modal-cancel] descendant is clicked (header close, footer
 *   Cancel, ...) -> settle(cancelValue)
 * - a click lands on the dialog backdrop itself, i.e. outside <article>
 *   (the backdrop IS the dialog element -- it fills the viewport, so
 *   e.target === dialog means "outside the visible box") -> settle(cancelValue)
 * - the native ESC-to-close 'cancel' event -> settle(cancelValue)
 * - caller-supplied extraSettlers (e.g. the confirm/add button) fire
 * Deliberately doesn't rely on the dialog's 'close' event to drive
 * resolution -- calls dialog.close() itself once settled instead of
 * waiting to react to it, since close()'s own 'close' event isn't
 * reliably observed in every environment this runs in. */
function runModal(dialog, cancelValue, wireExtra) {
  return new Promise((resolve) => {
    let settled = false;
    const settle = (value) => {
      if (settled) return;
      settled = true;
      cleanup();
      if (dialog.open) dialog.close();
      resolve(value);
    };

    const cancelBtns = Array.from(dialog.querySelectorAll('[data-modal-cancel]'));
    const onCancelClick = () => settle(cancelValue);
    const onBackdropClick = (e) => { if (e.target === dialog) settle(cancelValue); };
    const onNativeCancel = (e) => { e.preventDefault(); settle(cancelValue); };

    cancelBtns.forEach(b => b.addEventListener('click', onCancelClick));
    dialog.addEventListener('click', onBackdropClick);
    dialog.addEventListener('cancel', onNativeCancel);

    function cleanup() {
      cancelBtns.forEach(b => b.removeEventListener('click', onCancelClick));
      dialog.removeEventListener('click', onBackdropClick);
      dialog.removeEventListener('cancel', onNativeCancel);
      extraCleanup();
    }

    const extraCleanup = wireExtra(settle) || (() => {});
    dialog.showModal();
  });
}

/* Pico-styled native <dialog> replacing window.confirm() -- expects
 * index.html's #confirm-modal markup (title/message/confirm button).
 * Falls back to the real confirm() on a page that doesn't have it. */
function confirmModal(message, { title = 'Confirm', confirmLabel = 'Confirm', danger = false } = {}) {
  const dialog = document.getElementById('confirm-modal');
  if (!dialog) return Promise.resolve(confirm(message));

  dialog.querySelector('#confirm-modal-title').textContent = title;
  dialog.querySelector('#confirm-modal-message').textContent = message;
  const confirmBtn = dialog.querySelector('#confirm-modal-confirm');
  confirmBtn.textContent = confirmLabel;
  confirmBtn.classList.toggle('danger', !!danger);

  return runModal(dialog, false, (settle) => {
    const onConfirmClick = () => settle(true);
    confirmBtn.addEventListener('click', onConfirmClick);
    return () => confirmBtn.removeEventListener('click', onConfirmClick);
  });
}

/* "Some.Release.Name.2024.nzb" -> "Some Release Name 2024" -- the default
 * offered by the rename-on-add modal (see index.html's #rename-modal). */
function suggestJobName(filename) {
  return filename.replace(/\.nzb$/i, '').replace(/\./g, ' ').trim();
}

/* Resolves to { name, outputDir, shadowmount } (name possibly edited from
 * suggestedName), or null if cancelled. Falls back to auto-accepting
 * suggestedName (with the given defaultOutputDir and shadowmount off) on
 * a page without #rename-modal. */
function renamePrompt(suggestedName, defaultOutputDir) {
  const dialog = document.getElementById('rename-modal');
  if (!dialog) return Promise.resolve({ name: suggestedName, outputDir: defaultOutputDir || '', shadowmount: false });

  const input = dialog.querySelector('#rename-input');
  const outputInput = dialog.querySelector('#rename-output-dir');
  const shadowmountInput = dialog.querySelector('#rename-shadowmount');
  const confirmBtn = dialog.querySelector('#rename-modal-confirm');
  input.value = suggestedName;
  outputInput.value = defaultOutputDir || '';
  shadowmountInput.checked = false;

  const result = runModal(dialog, null, (settle) => {
    const doConfirm = () => {
      if (!input.value.trim()) return;
      settle({
        name: input.value.trim(),
        outputDir: outputInput.value.trim(),
        shadowmount: shadowmountInput.checked,
      });
    };
    const onKeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); doConfirm(); } };
    confirmBtn.addEventListener('click', doConfirm);
    input.addEventListener('keydown', onKeydown);
    outputInput.addEventListener('keydown', onKeydown);
    return () => {
      confirmBtn.removeEventListener('click', doConfirm);
      input.removeEventListener('keydown', onKeydown);
      outputInput.removeEventListener('keydown', onKeydown);
    };
  });
  input.focus();
  input.select();
  return result;
}
