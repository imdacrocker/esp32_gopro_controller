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
    const r = await fetch(path, opts);
    if (!r.ok) throw new Error(`${method} ${path} → ${r.status}`);
    const text = await r.text();
    return text ? JSON.parse(text) : {};
}

/* ---- Timezone dropdown --------------------------------------------------- */

function buildTimezoneDropdown() {
    const sel = document.getElementById('tz-select');
    for (let h = -12; h <= 14; h++) {
        const opt = document.createElement('option');
        opt.value = h;
        opt.textContent = h === 0 ? 'UTC' : (h > 0 ? `UTC+${h}` : `UTC${h}`);
        sel.appendChild(opt);
    }
    sel.addEventListener('change', () => {
        apiFetch('POST', '/api/settings/timezone', { tz_offset_hours: parseInt(sel.value) })
            .catch(() => {});
    });
}

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

function refreshTopSection() {
    apiFetch('GET', '/api/logging-state').then(d => {
        const pill = document.getElementById('rc-logging-pill');
        const cls  = 'rc-' + d.state.replace('_', '-');
        const labels = { logging: 'Logging', not_logging: 'Not Logging', unknown: 'Unknown' };
        pill.className = 'rc-value ' + cls;
        pill.textContent = labels[d.state] || d.state;
    }).catch(() => {});

    apiFetch('GET', '/api/utc').then(d => {
        const dateLine = document.getElementById('utc-date-line');
        const timeLine = document.getElementById('utc-time-line');
        if (!d.valid) {
            dateLine.textContent = 'No GPS';
            timeLine.textContent = '';
        } else {
            const dt = new Date(d.epoch_ms);
            dateLine.textContent = `${dt.getUTCFullYear()}-${String(dt.getUTCMonth()+1).padStart(2,'0')}-${String(dt.getUTCDate()).padStart(2,'0')}`;
            timeLine.textContent = `${String(dt.getUTCHours()).padStart(2,'0')}:${String(dt.getUTCMinutes()).padStart(2,'0')}:${String(dt.getUTCSeconds()).padStart(2,'0')}`;
        }
    }).catch(() => {});

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
    }).catch(() => {});

    // Show Set Date & Time row only when no live source has set time this
    // session. d.valid alone is not sufficient — firmware persists UTC across
    // reboots, so an NVS-restored boot value would otherwise hide the row
    // even though the user might want to enter a fresh time.
    apiFetch('GET', '/api/utc').then(d => {
        document.getElementById('datetime-row').style.display = d.session_synced ? 'none' : 'flex';
    }).catch(() => {});
}

function closeSettings() {
    settingsOverlay.classList.remove('open');
}

// Set Date & Time from browser — use event delegation so the button survives innerHTML replacement
document.getElementById('settings-overlay').addEventListener('click', e => {
    if (e.target.id !== 'datetime-btn') return;
    const actionDiv = document.getElementById('datetime-action');
    document.getElementById('datetime-btn').disabled = true;
    apiFetch('POST', '/api/settings/datetime', { epoch_ms: Date.now() })
        .then(() => {
            actionDiv.innerHTML = '<span class="settings-inline-msg" style="color:var(--green)">Time set ✓</span>';
            setTimeout(() => {
                actionDiv.innerHTML = '<button class="settings-action-btn" id="datetime-btn">Set from Device</button>';
            }, 2000);
        })
        .catch(() => {
            actionDiv.innerHTML = '<span class="settings-inline-msg" style="color:var(--red)">Failed — try again</span>';
            setTimeout(() => {
                actionDiv.innerHTML = '<button class="settings-action-btn" id="datetime-btn">Set from Device</button>';
            }, 2000);
        });
});

// Reboot
document.getElementById('reboot-btn').addEventListener('click', function () {
    if (!confirm('Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved.')) return;
    this.disabled = true;
    this.textContent = 'Rebooting…';
    apiFetch('POST', '/api/reboot').catch(() => {});
    setTimeout(() => location.reload(), 5000);
});

// Factory reset
document.getElementById('reset-btn').addEventListener('click', function () {
    if (!confirm('Restore Defaults?\n\nThis will erase all paired cameras and settings, then restart the controller. This cannot be undone.')) return;
    this.disabled = true;
    this.textContent = 'Resetting…';
    apiFetch('POST', '/api/factory-reset').catch(() => {});
    setTimeout(() => location.reload(), 5000);
});

/* ---- Manage cameras modal ------------------------------------------------ */

const modalOverlay = document.getElementById('modal-overlay');

document.getElementById('manage-btn').addEventListener('click', openModal);
document.getElementById('modal-done').addEventListener('click', closeModal);
modalOverlay.addEventListener('click', e => { if (e.target === modalOverlay) closeModal(); });

function openModal() {
    modalOverlay.classList.add('open');
    refreshModalPairedCameras();
    modalPairedRefreshTimer = setInterval(() => {
        refreshModalPairedCameras();
        refreshRcDiscovered();
    }, 3000);
}

function closeModal() {
    if (scanning) cancelScan();
    clearInterval(modalPairedRefreshTimer);
    modalPairedRefreshTimer = null;
    document.getElementById('modal-status').textContent = '';
    document.getElementById('results').innerHTML = '';
    document.getElementById('rc-results').innerHTML = '';
    rcListActivated = false;
    modalOverlay.classList.remove('open');
    /* Note: the pair-progress modal is a separate overlay with higher
     * z-index and manages its own lifecycle. */
}

function setModalStatus(msg) {
    document.getElementById('modal-status').textContent = msg;
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

document.getElementById('scan-btn').addEventListener('click', () => {
    if (scanning) {
        cancelScan();
    } else {
        startScan();
    }
});

function startScan() {
    scanning = true;
    scanSecondsLeft = 120;
    const btn = document.getElementById('scan-btn');
    btn.textContent = 'Cancel Scan';
    btn.classList.add('scanning');
    setModalStatus(`Scanning… ${scanSecondsLeft}s`);

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
            setModalStatus(`Scanning… ${scanSecondsLeft}s`);
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

    const btn = document.getElementById('scan-btn');
    btn.textContent = 'Scan for Cameras';
    btn.classList.remove('scanning');
    setModalStatus(cancelled ? 'Scan cancelled.' : 'Scan complete.');

    // Resume background polls
    if (!cameraStatusTimer) cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
    if (!topSectionTimer)   topSectionTimer   = setInterval(refreshTopSection, 2000);
    if (!modalPairedRefreshTimer && modalOverlay.classList.contains('open')) {
        modalPairedRefreshTimer = setInterval(() => {
            refreshModalPairedCameras();
            refreshRcDiscovered();
        }, 3000);
    }
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
    const results = document.getElementById('results');
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

document.getElementById('results').addEventListener('click', async e => {
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
        const scanBtn = document.getElementById('scan-btn');
        scanBtn.textContent = 'Scan for Cameras';
        scanBtn.classList.remove('scanning');

        if (!cameraStatusTimer) cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
        if (!topSectionTimer)   topSectionTimer   = setInterval(refreshTopSection, 2000);
        if (!modalPairedRefreshTimer && modalOverlay.classList.contains('open')) {
            modalPairedRefreshTimer = setInterval(() => {
                refreshModalPairedCameras();
                refreshRcDiscovered();
            }, 3000);
        }
    }

    document.getElementById('results').innerHTML = '';

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
    /* Dismiss the Add/Manage modal first so the pair sheet is the only
     * thing visible — on terminate, the home screen takes focus. */
    if (modalOverlay.classList.contains('open')) {
        closeModal();
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

/* ---- RC Emulation discovered --------------------------------------------- */

let rcListActivated = false;

document.getElementById('rc-add-btn').addEventListener('click', () => {
    rcListActivated = true;
    refreshRcDiscovered();
});

function refreshRcDiscovered() {
    if (!rcListActivated) return;
    apiFetch('GET', '/api/rc/discovered')
        .then(renderRcDiscovered)
        .catch(() => {});
}

function renderRcDiscovered(devices) {
    const container = document.getElementById('rc-results');
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

document.getElementById('rc-results').addEventListener('click', async e => {
    const btn = e.target.closest('.pair-this-btn');
    if (!btn) return;
    const addr = btn.dataset.addr;
    const ip   = btn.dataset.ip;

    if (!ip) {
        setModalStatus('Cannot add — IP address not yet assigned. Wait a moment and click Add a new Wifi RC Camera again.');
        return;
    }

    /* Same UX as the BLE pair flow — open the pair-progress modal, kick off
     * the add, then poll /api/pair/status for the shared state machine.
     * Success auto-dismisses; failure shows the error and an OK button. */
    document.getElementById('rc-results').innerHTML = '';

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
    const isRc = btn.dataset.rc === 'true';
    const verb = 'Remove';

    if (!confirm(`${verb} this camera?`)) return;

    apiFetch('POST', '/api/remove-camera', { slot })
        .then(() => {
            refreshModalPairedCameras();
            if (isRc) {
                setTimeout(refreshRcDiscovered, 1500);
            }
        })
        .catch(() => {});
});

/* ---- Startup ------------------------------------------------------------- */

buildTimezoneDropdown();
refreshTopSection();
refreshCameraStatus();

cameraStatusTimer = setInterval(refreshCameraStatus, 3000);
topSectionTimer   = setInterval(refreshTopSection, 2000);
