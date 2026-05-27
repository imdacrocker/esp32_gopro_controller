'use strict';

/* ---- State --------------------------------------------------------------- */

let autoControlEnabled   = true;
let shutterLocked        = {};      // slot -> { expectedStatus, timer }
let scanning             = false;
let cameraStatusLoaded   = false;
let pollTimer            = null;    // 1s BLE scan poll
let countdownTimer       = null;    // 1s scan countdown display
let scanSecondsLeft      = 0;
let modalPairedRefreshTimer = null;
let cameraStatusTimer    = null;    // 3s camera status poll (paused during BLE scan)
let topSectionTimer      = null;    // 2s top-section poll (paused during BLE scan)
let clockTickTimer       = null;    // 1s local clock display tick

/* Clock state: server epoch fetched at perf time. renderClock extrapolates
 * locally each second so the seconds tick smoothly without polling /api/utc
 * faster than the top-section rate. */
let clockServerEpochMs   = 0;
let clockFetchedAt       = 0;
let clockValid           = false;
let clockSessionSynced   = false;

/* Connection tracker: every apiFetch contributes a heartbeat. After
 * DISCONNECT_THRESHOLD consecutive failures we enter the disconnected
 * state, freeze the UI under an overlay, and probe /api/version for a
 * clean recovery → location.reload(). */
const DISCONNECT_THRESHOLD = 3;
let consecutiveFailures  = 0;
let disconnected         = false;
let reloading            = false;
let reconnectProbeTimer  = null;

/* ---- Helpers ------------------------------------------------------------- */

function setStatus(msg) {
    document.getElementById('status').textContent = msg;
}

async function apiFetch(method, path, body) {
    const opts = { method, headers: {} };
    if (body !== undefined) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
    }
    try {
        const r = await fetch(path, opts);
        if (!r.ok) throw new Error(`${method} ${path} → ${r.status}`);
        const text = await r.text();
        consecutiveFailures = 0;
        return text ? JSON.parse(text) : {};
    } catch (err) {
        consecutiveFailures++;
        if (!disconnected && consecutiveFailures >= DISCONNECT_THRESHOLD) {
            enterDisconnected();
        }
        throw err;
    }
}

/* ---- Disconnect handling ------------------------------------------------- */

function enterDisconnected() {
    if (disconnected) return;
    disconnected = true;

    // Halt every UI timer so no further requests fire and no stale renders
    // happen while the overlay is up. The reconnect probe (below) uses raw
    // fetch and is the only thing talking to the device until we reload.
    clearInterval(topSectionTimer);       topSectionTimer       = null;
    clearInterval(cameraStatusTimer);     cameraStatusTimer     = null;
    clearInterval(modalPairedRefreshTimer); modalPairedRefreshTimer = null;
    clearInterval(pollTimer);             pollTimer             = null;
    clearInterval(countdownTimer);        countdownTimer        = null;
    clearInterval(pairStatusTimer);       pairStatusTimer       = null;
    clearTimeout(pairDismissTimer);       pairDismissTimer      = null;
    clearInterval(clockTickTimer);        clockTickTimer        = null;

    // Hide every visible modal/sheet. We're about to reload on recovery
    // anyway, so no teardown — just remove the visibility classes/attrs.
    document.querySelectorAll('.modal-overlay.open').forEach(el => el.classList.remove('open'));
    const pairOverlay = document.getElementById('pair-overlay');
    if (pairOverlay) pairOverlay.hidden = true;

    const overlay = document.getElementById('disconnect-overlay');
    if (overlay) overlay.hidden = false;

    reconnectProbeTimer = setInterval(reconnectProbe, 1000);
}

async function reconnectProbe() {
    if (reloading) return;
    try {
        const ctrl = new AbortController();
        const t    = setTimeout(() => ctrl.abort(), 1500);
        const r    = await fetch('/api/version', { signal: ctrl.signal });
        clearTimeout(t);
        if (r.ok) {
            reloading = true;
            clearInterval(reconnectProbeTimer);
            reconnectProbeTimer = null;
            location.reload();
        }
    } catch (_) { /* still down; keep probing */ }
}

/* ---- Timezone dropdown --------------------------------------------------- */

function formatTzLabel(h) {
    if (h === 0) return 'UTC';
    return h > 0 ? `UTC+${h}` : `UTC${h}`;
}

function updateSystemTimeLabel(h) {
    const el = document.getElementById('system-time-label');
    if (el) el.textContent = `System Time (${formatTzLabel(h)})`;
}

function buildTimezoneDropdown() {
    const sel = document.getElementById('tz-select');
    for (let h = -12; h <= 14; h++) {
        const opt = document.createElement('option');
        opt.value = h;
        opt.textContent = formatTzLabel(h);
        sel.appendChild(opt);
    }
    sel.addEventListener('change', () => {
        const h = parseInt(sel.value);
        apiFetch('POST', '/api/settings/timezone', { tz_offset_hours: h })
            .then(() => updateSystemTimeLabel(h))
            .catch(() => {});
    });
}

/* ---- CAN bitrate dropdown ------------------------------------------------ */

let canBitrateInitial = null;

document.getElementById('can-bitrate-select').addEventListener('change', () => {
    const sel = document.getElementById('can-bitrate-select');
    const hint = document.getElementById('can-bitrate-hint');
    const bps = parseInt(sel.value, 10);
    apiFetch('POST', '/api/settings/can-bitrate', { bitrate_bps: bps })
        .then(() => {
            if (hint) hint.hidden = (bps === canBitrateInitial);
        })
        .catch(() => {});
});

/* ---- Logging settings ---------------------------------------------------- */

const SUPPORT_EMAIL = 'imdacrocker@gmail.com';

function formatBytes(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

function applyLogEnabled(enabled) {
    const track    = document.getElementById('log-toggle-track');
    const state    = document.getElementById('log-toggle-state');
    const controls = document.getElementById('log-controls');
    if (enabled) {
        track.classList.add('on');
        state.textContent = 'On';
        state.style.color = 'var(--green)';
        controls.hidden   = false;
        refreshLogStats();
    } else {
        track.classList.remove('on');
        state.textContent = 'Off';
        state.style.color = 'var(--gray-light)';
        controls.hidden   = true;
    }
}

function refreshLogStats() {
    apiFetch('GET', '/api/logs/stats').then(d => {
        const line = document.getElementById('log-stats-line');
        if (!line) return;
        const used = formatBytes(d.used);
        const cap  = formatBytes(d.capacity);
        let txt = `Ring: ${used} used / ${cap}`;
        if (d.lines_dropped_total > 0) {
            txt += ` · ${d.lines_dropped_total} dropped`;
            line.style.color = 'var(--orange-dark)';
        } else {
            line.style.color = 'var(--text-secondary, #555)';
        }
        line.textContent = txt;
    }).catch(() => {});
}

document.getElementById('log-toggle-wrap').addEventListener('click', () => {
    const track   = document.getElementById('log-toggle-track');
    const desired = !track.classList.contains('on');
    // Optimistic UI; revert on failure.
    applyLogEnabled(desired);
    apiFetch('POST', '/api/settings/logging-enabled', { enabled: desired })
        .then(d => applyLogEnabled(d.enabled))
        .catch(() => {
            applyLogEnabled(!desired);
            setStatus('Failed to update logging setting');
        });
});

document.getElementById('log-download-btn').addEventListener('click', () => {
    window.location = '/api/logs/download';
});

document.getElementById('log-email-btn').addEventListener('click', () => {
    const subject = 'GoPro Controller log';
    const body =
        'WHAT I WAS TRYING TO DO:\n\n\n' +
        'WHAT HAPPENED:\n\n\n' +
        'WHAT I EXPECTED INSTEAD:\n\n\n' +
        '------------------------------------------------------------\n' +
        'PLEASE ATTACH the log file that was just downloaded.\n' +
        'Look in your Downloads folder for a file named like\n' +
        'gopro-ctrl-XXXXXX-YYYYMMDD...txt\n' +
        '------------------------------------------------------------\n\n' +
        'PRIVACY NOTE: the attached log file contains device\n' +
        'identifiers (MAC addresses of cameras paired with this\n' +
        'controller, SSIDs of nearby GoPro WiFi networks, and the\n' +
        'controller’s own network name). It does not contain WiFi\n' +
        'passwords or location data. Please remove anything you\n' +
        'consider sensitive before sending.\n';
    const url = `mailto:${SUPPORT_EMAIL}` +
        `?subject=${encodeURIComponent(subject)}` +
        `&body=${encodeURIComponent(body)}`;
    window.location = '/api/logs/download';
    setTimeout(() => { window.location = url; }, 400);
});

document.getElementById('log-clear-btn').addEventListener('click', () => {
    apiFetch('GET', '/api/logs/stats').then(d => {
        if (!confirm(`Clear ${formatBytes(d.used)} of captured log data?`)) return;
        apiFetch('POST', '/api/logs/clear')
            .then(() => refreshLogStats())
            .catch(() => setStatus('Failed to clear log'));
    });
});

/* ---- Auto-control toggle ------------------------------------------------- */

function applyAutoControl(enabled) {
    autoControlEnabled = enabled;

    const track  = document.getElementById('toggle-track');
    const state  = document.getElementById('toggle-state');
    const bar    = document.getElementById('control-bar');

    if (enabled) {
        track.classList.add('on');
        state.textContent  = 'On';
        state.style.color  = 'var(--green)';
        bar.style.display  = 'none';
    } else {
        track.classList.remove('on');
        state.textContent  = 'Off';
        state.style.color  = 'var(--gray-light)';
        bar.style.display  = 'grid';
    }

    // Re-render camera cards so per-camera shutter buttons appear/disappear
    renderCameraCards(lastCameraList);
}

function setAutoControl(enabled) {
    apiFetch('POST', '/api/auto-control', { enabled })
        .then(d => applyAutoControl(d.enabled))
        .catch(() => {});
}

document.getElementById('toggle-wrap').addEventListener('click', () => {
    setAutoControl(!autoControlEnabled);
});

/* ---- Camera status ------------------------------------------------------- */

let lastCameraList = [];

const STATUS_LABEL = {
    disconnected: 'Not Connected',
    pairing:      'Pairing…',
    connecting:   'Connecting…',
    idle:         'Idle',
    recording:    'Recording',
};

const STATUS_ICON = {
    disconnected: `<span class="status-icon"><svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><circle cx="7" cy="7" r="5.2"/><line x1="3.5" y1="10.5" x2="10.5" y2="3.5"/></svg></span>`,
    idle:         `<span class="status-icon"><svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="7" cy="7" r="5.2"/></svg></span>`,
    recording:    `<span class="status-icon recording-icon"><svg width="14" height="14" viewBox="0 0 14 14"><circle cx="7" cy="7" r="5" fill="currentColor"/></svg></span>`,
    connecting:   `<span class="status-icon connecting-icon"></span>`,
    pairing:      `<span class="status-icon connecting-icon"></span>`,
};

function makeBadge(status) {
    const icon = STATUS_ICON[status] || `<span class="status-icon"></span>`;
    return `<div class="status-badge ${status}">
        ${icon}
        <span>${STATUS_LABEL[status] || status}</span>
    </div>`;
}

function makeShutterBtn(cam) {
    if (autoControlEnabled) return '';
    if (cam.status !== 'idle' && cam.status !== 'recording') return '';
    const isRec = cam.status === 'recording';
    const lock  = shutterLocked[cam.slot];
    const dis   = lock ? 'disabled' : '';
    const cls   = isRec ? 'cam-shutter-stop' : 'cam-shutter-start';
    const label = isRec ? 'Stop' : 'Record';
    return `<button class="cam-shutter-btn ${cls}" ${dis}
        data-slot="${cam.slot}" data-on="${isRec ? 'false' : 'true'}">${label}</button>`;
}

function renderCameraCards(cameras) {
    lastCameraList = cameras;

    const list    = document.getElementById('cam-status-list');
    const loading = document.getElementById('cam-status-loading');
    const empty   = document.getElementById('cam-status-empty');

    loading.style.display = 'none';

    if (!cameras.length) {
        empty.style.display = 'block';
        // Remove existing cards
        list.querySelectorAll('.camera-card').forEach(el => el.remove());
        return;
    }

    empty.style.display = 'none';

    cameras.forEach(cam => {
        const id   = `cam-card-${cam.slot}`;
        let   card = document.getElementById(id);

        const hasModel = cam.model_name && cam.model_name !== 'Unknown';
        const modelLine = hasModel
            ? `<div class="cam-model-name">${cam.model_name}</div>` : '';
        const nameLine = cam.name
            ? `<div class="cam-model-name">${cam.name}</div>` : '';

        const inner = `
            <div class="cam-meta">
                <span class="cam-number">Camera ${cam.index}</span>
                ${makeBadge(cam.status)}
            </div>
            ${modelLine}
            ${nameLine}
            <div class="cam-footer">
                ${makeShutterBtn(cam)}
            </div>`;

        if (!card) {
            card = document.createElement('div');
            card.className = 'camera-card';
            card.id = id;
            list.appendChild(card);
        }
        card.innerHTML = inner;
    });

    // Remove cards for slots no longer present
    list.querySelectorAll('.camera-card').forEach(el => {
        const slot = parseInt(el.id.replace('cam-card-', ''));
        if (!cameras.find(c => c.slot === slot)) el.remove();
    });
}

function refreshCameraStatus() {
    apiFetch('GET', '/api/paired-cameras')
        .then(cameras => {
            cameraStatusLoaded = true;

            // Resolve shutter locks early if camera reached expected state
            cameras.forEach(cam => {
                const lock = shutterLocked[cam.slot];
                if (lock && cam.status === lock.expectedStatus) {
                    clearTimeout(lock.timer);
                    delete shutterLocked[cam.slot];
                }
            });

            renderCameraCards(cameras);
        })
        .catch(() => {});
}

// Shutter button delegation
document.getElementById('cam-status-list').addEventListener('click', e => {
    const btn = e.target.closest('.cam-shutter-btn');
    if (!btn || btn.disabled) return;
    const slot = parseInt(btn.dataset.slot);
    const on   = btn.dataset.on === 'true';
    const expectedStatus = on ? 'recording' : 'idle';

    btn.disabled = true;
    shutterLocked[slot] = {
        expectedStatus,
        timer: setTimeout(() => { delete shutterLocked[slot]; renderCameraCards(lastCameraList); }, 5000),
    };

    apiFetch('POST', '/api/shutter', { slot, on })
        .then(d => setStatus(`Shutter command sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Shutter command failed.'));
});

// Record All / Stop All
document.getElementById('btn-record-all').addEventListener('click', function () {
    this.disabled = true;
    document.getElementById('btn-stop-all').disabled = true;
    apiFetch('POST', '/api/shutter', { on: true })
        .then(d => setStatus(`Record All sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Record All failed.'))
        .finally(() => {
            document.getElementById('btn-record-all').disabled = false;
            document.getElementById('btn-stop-all').disabled   = false;
        });
});

document.getElementById('btn-stop-all').addEventListener('click', function () {
    this.disabled = true;
    document.getElementById('btn-record-all').disabled = true;
    apiFetch('POST', '/api/shutter', { on: false })
        .then(d => setStatus(`Stop All sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Stop All failed.'))
        .finally(() => {
            document.getElementById('btn-record-all').disabled = false;
            document.getElementById('btn-stop-all').disabled   = false;
        });
});

/* ---- RC status + UTC + auto-control poll --------------------------------- */

const MONTH_NAMES = ['January', 'February', 'March', 'April', 'May', 'June',
                     'July', 'August', 'September', 'October', 'November', 'December'];

const RC_LABEL = { logging: 'Logging', not_logging: 'Not Logging', unknown: 'Disconnected' };
const RC_CAM_STATE = { logging: 'recording', not_logging: 'idle', unknown: 'disconnected' };

/* Update the per-fetch UTC state (validity, sync-button visibility, tooltip).
 * Display formatting happens in renderClock, which ticks locally every second
 * so the seconds digit advances smoothly between polls. */
function applyUtcResponse(d) {
    const display = document.getElementById('utc-display');
    const syncBtn = document.getElementById('datetime-btn');
    const infoBtn = document.getElementById('time-info-btn');
    const tip     = display.querySelector('.modal-info-tooltip');

    clockValid         = !!d.valid;
    clockSessionSynced = !!d.session_synced;
    if (clockValid) {
        clockServerEpochMs = d.epoch_ms;
        clockFetchedAt     = performance.now();
    }

    const stale = clockValid && !clockSessionSynced;
    display.classList.toggle('stale', !clockValid || stale);
    if (tip) {
        tip.textContent = clockValid
            ? 'System time was restored from memory but is stale and needs to be updated. Either wait for the RaceCapture to send UTC, or manually sync from this device.'
            : 'No time synced. Either wait for the RaceCapture to send UTC, or manually sync from this device.';
    }

    // Hide both the SYNC and the info button on the same condition. The
    // !dataset.busy gate keeps the buttons stable while a manual sync is
    // animating its "Set ✓" confirmation.
    const synced = clockSessionSynced;
    const busy   = syncBtn && syncBtn.dataset.busy;
    if (syncBtn && !busy) syncBtn.hidden = synced;
    if (infoBtn && !busy) {
        infoBtn.hidden = synced;
        if (synced && tip) tip.classList.remove('show');
    }

    renderClock();
}

function renderClock() {
    const dateLine = document.getElementById('utc-date-line');
    const timeLine = document.getElementById('utc-time-line');
    if (!clockValid) {
        dateLine.textContent = '';
        timeLine.textContent = '--:--:--';
        return;
    }
    const nowMs = clockServerEpochMs + (performance.now() - clockFetchedAt);
    const dt    = new Date(nowMs);
    const h24   = dt.getUTCHours();
    const ampm  = h24 >= 12 ? 'PM' : 'AM';
    const h12   = h24 % 12 || 12;
    dateLine.textContent = `${MONTH_NAMES[dt.getUTCMonth()]} ${dt.getUTCDate()}, ${dt.getUTCFullYear()}`;
    timeLine.textContent = `${String(h12).padStart(2,'0')}:${String(dt.getUTCMinutes()).padStart(2,'0')}:${String(dt.getUTCSeconds()).padStart(2,'0')} ${ampm}`;
}

function refreshTopSection() {
    apiFetch('GET', '/api/logging-state').then(d => {
        const pill   = document.getElementById('rc-logging-pill');
        const camSt  = RC_CAM_STATE[d.state] || 'disconnected';
        const label  = RC_LABEL[d.state] || d.state;
        pill.className = 'status-badge ' + camSt;
        pill.innerHTML = (STATUS_ICON[camSt] || '') + `<span>${label}</span>`;
    }).catch(() => {});

    apiFetch('GET', '/api/utc').then(applyUtcResponse).catch(() => {});

    apiFetch('GET', '/api/auto-control').then(d => {
        applyAutoControl(d.enabled);
    }).catch(() => {});
}

/* ---- Settings modal ------------------------------------------------------ */

const settingsOverlay = document.getElementById('settings-overlay');

document.getElementById('settings-btn').addEventListener('click', openSettings);
document.getElementById('settings-done').addEventListener('click', closeSettings);
settingsOverlay.addEventListener('click', e => { if (e.target === settingsOverlay) closeSettings(); });

function openSettings() {
    settingsOverlay.classList.add('open');

    // Load current timezone
    apiFetch('GET', '/api/settings/timezone').then(d => {
        document.getElementById('tz-select').value = d.tz_offset_hours;
        updateSystemTimeLabel(d.tz_offset_hours);
    }).catch(() => {});

    // Load current CAN bitrate
    apiFetch('GET', '/api/settings/can-bitrate').then(d => {
        canBitrateInitial = d.bitrate_bps;
        document.getElementById('can-bitrate-select').value = String(d.bitrate_bps);
        const hint = document.getElementById('can-bitrate-hint');
        if (hint) hint.hidden = true;
    }).catch(() => {});

}

function closeSettings() {
    settingsOverlay.classList.remove('open');
}

/* ---- Advanced modal ------------------------------------------------------ *
 * Opening Advanced dismisses Settings so the two are never stacked. Closing
 * Advanced (Done or click-outside) reopens Settings so the user lands back
 * where they came from. */

const advancedOverlay = document.getElementById('advanced-overlay');

document.getElementById('advanced-btn').addEventListener('click', () => {
    closeSettings();
    openAdvanced();
});
document.getElementById('advanced-done').addEventListener('click', () => {
    closeAdvanced();
    settingsOverlay.classList.add('open');
});
advancedOverlay.addEventListener('click', e => {
    if (e.target === advancedOverlay) {
        closeAdvanced();
        settingsOverlay.classList.add('open');
    }
});

function openAdvanced() {
    advancedOverlay.classList.add('open');
    apiFetch('GET', '/api/settings/logging-enabled').then(d => {
        applyLogEnabled(!!d.enabled);
    }).catch(() => {});
}

function closeAdvanced() {
    advancedOverlay.classList.remove('open');
}

/* ---- About dialog -------------------------------------------------------- *
 * Uses the browser's native alert() for a simple OK-only dialog. */

document.getElementById('about-btn').addEventListener('click', () => {
    apiFetch('GET', '/api/version').then(v => {
        const built = (v.build_date && v.build_time)
            ? `${v.build_date} ${v.build_time}` : '—';
        alert(
            `Main App:     ${v.app || 'unknown'}\n` +
            `Built:        ${built}\n` +
            `Recovery App: ${v.recovery || 'unknown'}`
        );
    }).catch(() => alert('Failed to load version info.'));
});

// Sync system time from browser
document.getElementById('datetime-btn').addEventListener('click', () => {
    const btn = document.getElementById('datetime-btn');
    btn.dataset.busy = '1';
    btn.disabled = true;
    btn.textContent = '…';
    apiFetch('POST', '/api/settings/datetime', { epoch_ms: Date.now() })
        .then(() => {
            btn.textContent = 'Set ✓';
            const display = document.getElementById('utc-display');
            const infoBtn = document.getElementById('time-info-btn');
            const tip     = display.querySelector('.modal-info-tooltip');
            display.classList.remove('stale');
            if (infoBtn) infoBtn.hidden = true;
            if (tip) tip.classList.remove('show');
            setTimeout(() => {
                delete btn.dataset.busy;
                btn.disabled = false;
                btn.textContent = 'Sync';
                btn.hidden = true;
            }, 3000);
        })
        .catch(() => {
            btn.textContent = 'Failed';
            setTimeout(() => {
                delete btn.dataset.busy;
                btn.disabled = false;
                btn.textContent = 'Sync';
            }, 3000);
        });
});

// Reboot
document.getElementById('reboot-btn').addEventListener('click', function () {
    if (!confirm('Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved.')) return;
    this.disabled = true;
    this.textContent = 'Rebooting…';
    apiFetch('POST', '/api/reboot').catch(() => {});
    setTimeout(() => location.reload(), 3000);
});

// Shutdown
let shutdownPollTimer = null;
let shutdownComplete  = false;

document.getElementById('shutdown-btn').addEventListener('click', function () {
    if (!confirm('Shut down controller?\n\nAll cameras will be put to sleep and disconnected. You MUST reboot to reconnect.')) return;
    this.disabled = true;
    this.textContent = 'Shutting down…';
    apiFetch('POST', '/api/shutdown').catch(() => {});
    startShutdownPoll();
});

function startShutdownPoll() {
    if (shutdownPollTimer || shutdownComplete) return;
    shutdownPollTimer = setInterval(checkShutdownState, 1000);
    checkShutdownState();
}

async function checkShutdownState() {
    try {
        const r = await apiFetch('GET', '/api/shutdown');
        if (r && r.state === 'complete') enterShutdownComplete();
    } catch (e) { /* retry on next tick */ }
}

function enterShutdownComplete() {
    if (shutdownComplete) return;
    shutdownComplete = true;

    // Halt every UI timer — nothing useful can run until the user reboots.
    clearInterval(shutdownPollTimer);       shutdownPollTimer       = null;
    clearInterval(topSectionTimer);         topSectionTimer         = null;
    clearInterval(cameraStatusTimer);       cameraStatusTimer       = null;
    clearInterval(modalPairedRefreshTimer); modalPairedRefreshTimer = null;
    clearInterval(pollTimer);               pollTimer               = null;
    clearInterval(countdownTimer);          countdownTimer          = null;
    clearInterval(pairStatusTimer);         pairStatusTimer         = null;
    clearTimeout(pairDismissTimer);         pairDismissTimer        = null;
    clearInterval(clockTickTimer);          clockTickTimer          = null;

    // Dismiss any open modals/sheets.
    document.querySelectorAll('.modal-overlay.open').forEach(el => el.classList.remove('open'));
    const pairOverlay = document.getElementById('pair-overlay');
    if (pairOverlay) pairOverlay.hidden = true;

    const overlay = document.getElementById('shutdown-overlay');
    if (overlay) overlay.hidden = false;
}

document.getElementById('shutdown-reboot-btn').addEventListener('click', function () {
    this.disabled = true;
    this.textContent = 'REBOOTING…';
    apiFetch('POST', '/api/reboot').catch(() => {});

    // location.reload() alone races the device — it fires while the ESP is
    // still rebooting, the browser hangs trying to fetch, and the page never
    // refreshes from the user's POV. Probe /api/version until it answers,
    // then reload. Same pattern as the disconnect overlay.
    setTimeout(() => {
        const probe = setInterval(async () => {
            try {
                const ctrl = new AbortController();
                const t    = setTimeout(() => ctrl.abort(), 1500);
                const r    = await fetch('/api/version', { signal: ctrl.signal, cache: 'no-store' });
                clearTimeout(t);
                if (r.ok) {
                    clearInterval(probe);
                    location.reload();
                }
            } catch (_) { /* still rebooting */ }
        }, 1000);
    }, 2000);
});

// On page load, surface any in-progress / completed shutdown so a refresh
// doesn't leave the user looking at a stale UI that's about to 503 everything.
(async function checkShutdownOnLoad() {
    try {
        const r = await apiFetch('GET', '/api/shutdown');
        if (!r) return;
        if (r.state === 'complete')           enterShutdownComplete();
        else if (r.state === 'shutting_down') startShutdownPoll();
    } catch (e) { /* device unreachable — disconnect overlay will take over */ }
})();

/* ---- Manage cameras modal ------------------------------------------------ */

const modalOverlay = document.getElementById('modal-overlay');

document.getElementById('manage-btn').addEventListener('click', openModal);
document.getElementById('modal-done').addEventListener('click', closeModal);
modalOverlay.addEventListener('click', e => { if (e.target === modalOverlay) closeModal(); });

function openModal() {
    modalOverlay.classList.add('open');
    refreshModalPairedCameras();
    modalPairedRefreshTimer = setInterval(refreshModalPairedCameras, 3000);
}

function closeModal() {
    clearInterval(modalPairedRefreshTimer);
    modalPairedRefreshTimer = null;
    modalOverlay.classList.remove('open');
    /* Note: the pair-progress modal is a separate overlay with higher
     * z-index and manages its own lifecycle. */
}

/* ---- Add Camera modal ---------------------------------------------------- *
 * Two-pane slider: model picker → instructions. Hero3/Hero4 use the WiFi-RC
 * "discovered devices" flow; everything else uses BLE scan. Opening this
 * modal dismisses the Manage modal. Both Add (RC) and Pair (BLE) hand off
 * to the existing pair-progress overlay. */

const RC_MODELS         = new Set(['Hero3', 'Hero4']);
const addCameraOverlay  = document.getElementById('add-camera-overlay');
const addCameraTrack    = document.getElementById('add-camera-track');
let   addCameraRcPollTimer = null;

document.getElementById('add-camera-btn').addEventListener('click', () => {
    closeModal();
    openAddCameraModal();
});

document.getElementById('add-camera-cancel').addEventListener('click', closeAddCameraModal);
document.getElementById('add-camera-back').addEventListener('click', slideToList);

addCameraOverlay.addEventListener('click', e => {
    if (e.target === addCameraOverlay) closeAddCameraModal();
});

document.querySelectorAll('.camera-model-row').forEach(row => {
    row.addEventListener('click', () => selectCameraModel(row.dataset.model));
});

function openAddCameraModal() {
    resetAddCameraModal();
    addCameraOverlay.classList.add('open');
}

function closeAddCameraModal() {
    if (scanning) cancelScan();
    clearInterval(addCameraRcPollTimer);
    addCameraRcPollTimer = null;
    rcListActivated = false;
    addCameraOverlay.classList.remove('open');
    resetAddCameraModal();
}

function resetAddCameraModal() {
    addCameraTrack.classList.remove('step-instructions');
    document.getElementById('add-camera-back').hidden = true;
    document.getElementById('add-camera-ble-section').hidden = true;
    document.getElementById('add-camera-rc-section').hidden = true;
    document.getElementById('add-camera-ble-results').innerHTML = '';
    document.getElementById('add-camera-rc-results').innerHTML = '';
    document.getElementById('add-camera-status').textContent = '';
    document.getElementById('add-camera-instructions').textContent = '';
}

function instructionsForModel(model) {
    if (RC_MODELS.has(model)) {
        return 'Reset all wireless connections on the camera, and then choose to pair with a new WiFi RC.';
    }
    if (model === 'Hero7') {
        /* Hero7 forces the controller's STA to bounce while we negotiate the
         * AP/STA dance, which can drop the user's own browser if they're on
         * the same WiFi — call that out so they don't think it failed. */
        return 'Reset all wireless connections on the camera. After resetting you MUST update the camera Wifi to 2.4ghz manually. Then, choose to pair with the GoPro App. Once in Pairing mode, click Scan.<br><br>NOTE: The wireless connection to this browser may disconnect during this process. If this happens, reconnect to the controller\'s WiFi and refresh.';
    }
    return 'Reset all wireless connections on the camera, and then choose to pair with the GoPro App. Once in Pairing mode, click Scan.';
}

function selectCameraModel(model) {
    const instructionsEl = document.getElementById('add-camera-instructions');
    const bleSection     = document.getElementById('add-camera-ble-section');
    const rcSection      = document.getElementById('add-camera-rc-section');
    document.getElementById('add-camera-back').hidden = false;

    instructionsEl.innerHTML = instructionsForModel(model);

    if (RC_MODELS.has(model)) {
        bleSection.hidden = true;
        rcSection.hidden  = false;
        rcListActivated = true;
        refreshRcDiscovered();
        clearInterval(addCameraRcPollTimer);
        addCameraRcPollTimer = setInterval(refreshRcDiscovered, 3000);
    } else {
        bleSection.hidden = false;
        rcSection.hidden  = true;
        document.getElementById('add-camera-ble-results').innerHTML = '';
        setAddCameraStatus('');
        const scanBtn = document.getElementById('add-camera-scan-btn');
        scanBtn.textContent = 'Scan';
        scanBtn.classList.remove('scanning');
    }

    addCameraTrack.classList.add('step-instructions');
}

function slideToList() {
    if (scanning) cancelScan();
    clearInterval(addCameraRcPollTimer);
    addCameraRcPollTimer = null;
    rcListActivated = false;
    addCameraTrack.classList.remove('step-instructions');
    document.getElementById('add-camera-back').hidden = true;
    document.getElementById('add-camera-ble-results').innerHTML = '';
    document.getElementById('add-camera-rc-results').innerHTML = '';
    setAddCameraStatus('');
}

function setAddCameraStatus(msg) {
    document.getElementById('add-camera-status').textContent = msg;
}

/* ---- Info-button tooltips ----------------------------------------------- */

document.querySelectorAll('.modal-info-btn').forEach(btn => {
    btn.addEventListener('click', e => {
        e.stopPropagation();
        const tip = btn.parentElement.querySelector('.modal-info-tooltip');
        const wasOpen = tip.classList.contains('show');
        document.querySelectorAll('.modal-info-tooltip.show').forEach(t => t.classList.remove('show'));
        if (!wasOpen) tip.classList.add('show');
    });
});

document.addEventListener('click', e => {
    if (e.target.closest('.modal-info-tooltip') || e.target.closest('.modal-info-btn')) return;
    document.querySelectorAll('.modal-info-tooltip.show').forEach(t => t.classList.remove('show'));
});

/* ---- BLE Scan ------------------------------------------------------------ */

document.getElementById('add-camera-scan-btn').addEventListener('click', () => {
    if (scanning) {
        cancelScan();
    } else {
        startScan();
    }
});

function startScan() {
    scanning = true;
    scanSecondsLeft = 120;
    const btn = document.getElementById('add-camera-scan-btn');
    btn.textContent = 'Cancel Scan';
    btn.classList.add('scanning');
    setAddCameraStatus(`Scanning… ${scanSecondsLeft}s`);

    // Snapshot known-paired addrs so pollScanResults can filter without polling /api/paired-cameras
    lastPairedAddrs = new Set(lastCameraList.map(c => c.addr));

    // Pause all background polls — reduces concurrent TCP connections while NimBLE is active
    clearInterval(cameraStatusTimer);       cameraStatusTimer       = null;
    clearInterval(topSectionTimer);         topSectionTimer         = null;
    clearInterval(modalPairedRefreshTimer); modalPairedRefreshTimer = null;

    apiFetch('POST', '/api/scan').catch(() => {});

    pollTimer = setInterval(pollScanResults, 1000);
    countdownTimer = setInterval(() => {
        scanSecondsLeft--;
        if (scanSecondsLeft > 0) {
            setAddCameraStatus(`Scanning… ${scanSecondsLeft}s`);
        } else {
            stopScan(false);
        }
    }, 1000);
}

function cancelScan() {
    apiFetch('POST', '/api/scan-cancel').catch(() => {});
    stopScan(true);
}

function stopScan(cancelled) {
    scanning = false;
    clearInterval(pollTimer);
    clearInterval(countdownTimer);
    pollTimer = null;
    countdownTimer = null;

    const btn = document.getElementById('add-camera-scan-btn');
    btn.textContent = 'Scan';
    btn.classList.remove('scanning');
    setAddCameraStatus(cancelled ? 'Scan cancelled.' : 'Scan complete.');

    // Resume background polls (manage modal is closed while add-camera is open,
    // so its paired-list poll doesn't need restarting here)
    if (!cameraStatusTimer) cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
    if (!topSectionTimer)   topSectionTimer   = setInterval(refreshTopSection, 2000);
}

let lastPairedAddrs = new Set();

function pollScanResults() {
    return apiFetch('GET', '/api/cameras')
        .then(found => {
            const unpaired = found.filter(c => !lastPairedAddrs.has(c.addr));
            renderFoundCameras(unpaired);
        }).catch(() => {});
}

function renderFoundCameras(cameras) {
    const results = document.getElementById('add-camera-ble-results');
    results.innerHTML = '';
    cameras.forEach(cam => {
        const row = document.createElement('div');
        row.className = 'found-camera-row';
        row.innerHTML = `
            <div class="found-cam-info">
                <div class="found-cam-name">${cam.name}</div>
                <div class="found-cam-meta">${cam.addr} &nbsp;·&nbsp; RSSI ${cam.rssi}</div>
            </div>
            <button class="pair-this-btn" data-addr="${cam.addr}" data-addr-type="${cam.addr_type}">Pair</button>`;
        results.appendChild(row);
    });
}

document.getElementById('add-camera-ble-results').addEventListener('click', async e => {
    const btn = e.target.closest('.pair-this-btn');
    if (!btn) return;
    const addr      = btn.dataset.addr;
    const addr_type = parseInt(btn.dataset.addrType, 10);

    // Stop scan timers first so no more poll requests fire, then resume background polls
    if (scanning) {
        scanning = false;
        clearInterval(pollTimer);
        clearInterval(countdownTimer);
        pollTimer = null;
        countdownTimer = null;
        const scanBtn = document.getElementById('add-camera-scan-btn');
        scanBtn.textContent = 'Scan';
        scanBtn.classList.remove('scanning');

        if (!cameraStatusTimer) cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
        if (!topSectionTimer)   topSectionTimer   = setInterval(refreshTopSection, 2000);
    }

    document.getElementById('add-camera-ble-results').innerHTML = '';

    // Cancel the BLE scan first, then pair — avoids 4 concurrent requests to the ESP32
    await apiFetch('POST', '/api/scan-cancel').catch(() => {});

    openPairModal();
    try {
        await apiFetch('POST', '/api/pair', { addr, addr_type });
    } catch (err) {
        showPairModalFailure(err.message || 'Pair request failed.');
        return;
    }
    startPairStatusPoll();
});

/* ---- Pair-progress modal ------------------------------------------------- */

const pairOverlay   = document.getElementById('pair-overlay');
const pairThrobber  = document.getElementById('pair-throbber');
const pairStatusEl  = document.getElementById('pair-status');
const pairCancelBtn = document.getElementById('pair-cancel-btn');

let pairStatusTimer = null;
let pairDismissTimer = null;
let pairCancelInFlight = false;

const PAIR_STATE_LABEL = {
    connecting:   'Connecting to camera…',
    bonding:      'Bonding…',
    provisioning: 'Configuring camera…',
};

const PAIR_ERROR_LABEL = {
    none:               '',
    slots_full:         'All camera slots are in use. Remove a camera first.',
    ble_connect_failed: 'Could not connect to the camera. Move it closer and try again.',
    bond_failed:        'Bonding failed. Reset connections on the camera and retry.',
    hwinfo_timeout:     'Camera did not respond. Check that it is powered on and in pairing mode.',
    model_unsupported:  'This camera model is not supported.',
    handshake_timeout:  'Camera setup timed out. Try again.',
    disconnected:       'Camera disconnected during pairing.',
    cancelled:          'Pairing cancelled.',
    internal:           'Internal error during pairing.',
};

function openPairModal() {
    /* Dismiss any visible camera-management sheet first so the pair sheet is
     * the only thing on screen — on terminate, the home screen takes focus. */
    if (modalOverlay.classList.contains('open')) {
        closeModal();
    }
    if (addCameraOverlay.classList.contains('open')) {
        closeAddCameraModal();
    }
    pairCancelInFlight = false;
    pairThrobber.className = 'pair-throbber';
    pairStatusEl.textContent = 'Connecting to camera…';
    pairCancelBtn.textContent = 'Cancel';
    pairCancelBtn.disabled = false;
    pairOverlay.hidden = false;
    pairOverlay.setAttribute('aria-hidden', 'false');
    pairOverlay.classList.add('open');
}

function closePairModal() {
    pairOverlay.classList.remove('open');
    pairOverlay.hidden = true;
    pairOverlay.setAttribute('aria-hidden', 'true');
    stopPairStatusPoll();
    clearTimeout(pairDismissTimer);
    pairDismissTimer = null;
}

function showPairModalFailure(message) {
    stopPairStatusPoll();
    pairThrobber.className = 'pair-throbber failed';
    pairStatusEl.textContent = message;
    pairCancelBtn.textContent = 'OK';
    pairCancelBtn.disabled = false;
    pairCancelInFlight = false;
}

function showPairModalSuccess() {
    stopPairStatusPoll();
    pairThrobber.className = 'pair-throbber success';
    pairStatusEl.textContent = 'Success!';
    pairCancelBtn.disabled = true;

    /* Hold for 2s, then dismiss the pair sheet — manage modal is already
     * closed (closed when the pair sheet opened), so the home screen takes
     * focus directly. */
    clearTimeout(pairDismissTimer);
    pairDismissTimer = setTimeout(() => {
        closePairModal();
        refreshCameraStatus();
    }, 2000);
}

function setPairProgress(state, modelName) {
    let label = PAIR_STATE_LABEL[state] || 'Pairing…';
    if (state === 'provisioning' && modelName && modelName !== 'Unknown') {
        label = `Configuring camera (${modelName})…`;
    }
    pairStatusEl.textContent = label;
}

function startPairStatusPoll() {
    clearInterval(pairStatusTimer);
    pairStatusTimer = setInterval(pollPairStatus, 1000);
    pollPairStatus();
}

function stopPairStatusPoll() {
    clearInterval(pairStatusTimer);
    pairStatusTimer = null;
}

function pollPairStatus() {
    apiFetch('GET', '/api/pair/status')
        .then(info => {
            /* If the user already clicked Cancel, the local UI state takes
             * precedence — don't let a late "connecting" poll overwrite the
             * cancel feedback. */
            if (pairCancelInFlight && info.state !== 'failed') return;

            switch (info.state) {
                case 'connecting':
                case 'bonding':
                case 'provisioning':
                    setPairProgress(info.state, info.model_name);
                    break;
                case 'success':
                    showPairModalSuccess();
                    break;
                case 'failed': {
                    const label = PAIR_ERROR_LABEL[info.error_code]
                        || info.error_message
                        || 'Pairing failed.';
                    showPairModalFailure(label);
                    break;
                }
                case 'idle':
                default:
                    /* No attempt in flight (e.g. server forgot or never started). */
                    stopPairStatusPoll();
                    break;
            }
        })
        .catch(() => {
            /* Transient fetch failure — interval will retry. */
        });
}

pairCancelBtn.addEventListener('click', () => {
    /* Three roles for the same button:
     *   - Active pairing: "Cancel" → server-side abort, then close modal
     *     when the FAILED state arrives.
     *   - Failure shown: "OK" → just close.
     *   - Success shown: disabled (handled above). */
    if (pairThrobber.classList.contains('failed')) {
        closePairModal();
        return;
    }
    if (pairThrobber.classList.contains('success')) {
        return;  /* shouldn't happen — button is disabled */
    }

    pairCancelInFlight = true;
    pairCancelBtn.disabled = true;
    pairStatusEl.textContent = 'Cancelling…';
    apiFetch('POST', '/api/pair/cancel', {}).catch(() => {});
    /* Polling will pick up the FAILED+cancelled state and call
     * showPairModalFailure(), which re-enables the button as "OK". */
});

/* ---- RC Emulation discovered --------------------------------------------- *
 * Activated when the user picks a Hero3 / Hero4 in the Add Camera picker;
 * polled every 3 s from there. Click "Add" on a row → pair-progress modal. */

let rcListActivated = false;

function refreshRcDiscovered() {
    if (!rcListActivated) return;
    apiFetch('GET', '/api/rc/discovered')
        .then(renderRcDiscovered)
        .catch(() => {});
}

function renderRcDiscovered(devices) {
    const container = document.getElementById('add-camera-rc-results');
    container.innerHTML = '';

    if (!devices.length) {
        const p = document.createElement('p');
        p.className = 'modal-empty';
        p.textContent = 'No unidentified devices connected.';
        container.appendChild(p);
        return;
    }

    const msg = document.createElement('p');
    msg.className = 'modal-empty';
    msg.textContent = `${devices.length} device${devices.length !== 1 ? 's' : ''} connected — click Add to probe:`;
    container.appendChild(msg);

    devices.forEach(dev => {
        const row = document.createElement('div');
        row.className = 'found-camera-row';
        const ip = dev.ip || null;
        row.innerHTML = `
            <div class="found-cam-info">
                <div class="found-cam-name">Unknown Device</div>
                <div class="found-cam-meta">${dev.addr} &nbsp;·&nbsp; ${ip || 'IP pending'}</div>
            </div>
            <button class="pair-this-btn" data-addr="${dev.addr}" data-ip="${ip || ''}">Add</button>`;
        container.appendChild(row);
    });
}

document.getElementById('add-camera-rc-results').addEventListener('click', async e => {
    const btn = e.target.closest('.pair-this-btn');
    if (!btn) return;
    const addr = btn.dataset.addr;
    const ip   = btn.dataset.ip;

    if (!ip) {
        setAddCameraStatus('Cannot add — IP address not yet assigned. Wait a moment and try again.');
        return;
    }

    /* Same UX as the BLE pair flow — open the pair-progress modal, kick off
     * the add, then poll /api/pair/status for the shared state machine.
     * Success auto-dismisses; failure shows the error and an OK button. */
    document.getElementById('add-camera-rc-results').innerHTML = '';

    openPairModal();
    try {
        await apiFetch('POST', '/api/rc/add', { addr, ip });
    } catch (err) {
        showPairModalFailure(err.message || 'Add request failed.');
        return;
    }
    startPairStatusPoll();
});

/* ---- Paired cameras list in modal ---------------------------------------- */

function refreshModalPairedCameras() {
    apiFetch('GET', '/api/paired-cameras')
        .then(renderModalPairedCameras)
        .catch(() => {});
}

function renderModalPairedCameras(cameras) {
    const list  = document.getElementById('paired-list');
    const badge = document.getElementById('paired-count');
    list.innerHTML = '';

    if (!cameras.length) {
        badge.classList.remove('visible');
        const p = document.createElement('p');
        p.className = 'modal-empty';
        p.textContent = 'No cameras paired.';
        list.appendChild(p);
        return;
    }

    badge.textContent = cameras.length;
    badge.classList.add('visible');

    cameras.forEach(cam => {
        const isRc     = cam.type === 'rc_emulation';
        const isBle    = cam.type === 'ble';
        const typeBadge = isRc ? '<span class="cam-type-badge">WiFi RC</span>'
                        : isBle ? '<span class="cam-type-badge">Bluetooth</span>' : '';

        const hasModel = cam.model_name && cam.model_name !== 'Unknown';
        const nameLine  = cam.name
            ? `<div class="modal-paired-meta">${cam.name}</div>` : '';
        const modelLine = hasModel
            ? `<div class="modal-paired-meta">${cam.model_name}</div>` : '';

        const addrParts = [];
        if (cam.ip)   addrParts.push(cam.ip);
        if (cam.addr) addrParts.push(cam.addr);
        const addrLine = addrParts.length
            ? `<div class="modal-paired-meta">${addrParts.join(' · ')}</div>` : '';

        const row = document.createElement('div');
        row.className = 'modal-paired-row';
        row.innerHTML = `
            <div class="modal-paired-info">
                <div class="modal-paired-name">Camera ${cam.index} ${typeBadge}</div>
                ${nameLine}
                ${modelLine}
                ${addrLine}
            </div>
            <button class="remove-btn" data-slot="${cam.slot}" data-rc="${isRc}">Remove</button>`;
        list.appendChild(row);
    });
}

document.getElementById('paired-list').addEventListener('click', e => {
    const btn = e.target.closest('.remove-btn');
    if (!btn) return;
    const slot = parseInt(btn.dataset.slot);

    if (!confirm('Remove this camera?')) return;

    apiFetch('POST', '/api/remove-camera', { slot })
        .then(refreshModalPairedCameras)
        .catch(() => {});
});

/* ---- Startup ------------------------------------------------------------- */

buildTimezoneDropdown();
apiFetch('GET', '/api/settings/timezone')
    .then(d => updateSystemTimeLabel(d.tz_offset_hours))
    .catch(() => {});
refreshTopSection();
refreshCameraStatus();

cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
topSectionTimer   = setInterval(refreshTopSection, 2000);
clockTickTimer    = setInterval(renderClock, 1000);

document.getElementById('disconnect-reload-btn').addEventListener('click', () => {
    reloading = true;
    location.reload();
});
