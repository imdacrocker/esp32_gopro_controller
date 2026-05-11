'use strict';

/* updates.js — OTA update flow for the main app web UI (ota_design.md §13).
 *
 * Lifecycle:
 *   - Loaded after app.js. Auto-binds to the settings button so the panel
 *     fetches version + channel + base URL the first time the user opens it.
 *   - Channel selector POSTs immediately on change (Q2 — "just remembers").
 *   - "Check for updates" fetches the manifest and shows version + Install.
 *   - "Install" downloads both blobs from the cloud, uploads them with two
 *     progress bars, commits, and polls /api/version after reboot.
 */

(function () {

const POLL_INITIAL_DELAY_MS = 3000;
const POLL_INTERVAL_MS      = 2000;
const POLL_REQ_TIMEOUT_MS   = 3000;
const POLL_OVERALL_MS       = 60000;
const MANIFEST_TIMEOUT_MS   = 10000;

const CHANNEL_LABELS = { stable: 'Stable', beta: 'Beta', dev: 'Dev' };

const els = {};
let baseUrl = null;          // cached after first load (manifest.ota_base_url)
let repoPath = null;         // cached after first load (manifest.ota_repo_path)
let lastChannel = null;      // tracked so we can revert on POST failure
let lastManifest = null;     // most recent successful manifest fetch
let installing = false;
let panelInited = false;

document.addEventListener('DOMContentLoaded', () => {
    els.appVersion      = document.getElementById('upd-app-version');
    els.recoveryVersion = document.getElementById('upd-recovery-version');
    els.channelSelect   = document.getElementById('upd-channel-select');
    els.checkBtn        = document.getElementById('upd-check-btn');
    els.result          = document.getElementById('upd-result');
    els.recoveryBtn     = document.getElementById('upd-recovery-btn');

    // Refresh whenever the user opens settings — versions and channel may
    // have changed since the last open (e.g., a recovery round-trip).
    document.getElementById('settings-btn')
        .addEventListener('click', loadPanel);

    els.checkBtn.addEventListener('click', onCheckClick);
    els.channelSelect.addEventListener('change', onChannelChange);
    els.recoveryBtn.addEventListener('click', onRestartRecoveryClick);
});

/* ---- Panel load --------------------------------------------------------- */

async function loadPanel() {
    if (installing) return;  // never reset state mid-install

    // /api/version carries everything we need: versions, ota_base_url,
    // ota_repo_path. Channel comes from /api/ota/channel because it
    // includes the available[] list.
    const [version, channel] = await Promise.all([
        safeJson('/api/version'),
        safeJson('/api/ota/channel'),
    ]);

    if (version) {
        els.appVersion.textContent      = version.app || 'unknown';
        els.recoveryVersion.textContent = version.recovery || 'unknown';
        if (version.ota_base_url && version.ota_repo_path) {
            baseUrl  = version.ota_base_url;
            repoPath = version.ota_repo_path;
            panelInited = true;
        }
    }

    if (channel) {
        const cur = channel.current;
        const opts = (channel.available || ['stable', 'beta']);
        els.channelSelect.innerHTML = '';
        for (const c of opts) {
            const o = document.createElement('option');
            o.value = c;
            o.textContent = CHANNEL_LABELS[c] || c;
            els.channelSelect.appendChild(o);
        }
        els.channelSelect.value = cur;
        els.channelSelect.disabled = false;
        lastChannel = cur;
    }

    const ready = baseUrl && repoPath;
    els.checkBtn.disabled = !ready;
    if (!ready) {
        showResult('error', 'Update server URL is not configured. Reflash the device.');
    }
}

async function safeJson(path, opts) {
    try {
        const r = await fetch(path, opts);
        if (!r.ok) return null;
        return await r.json();
    } catch (_) {
        return null;
    }
}

/* ---- Channel switch ----------------------------------------------------- */

async function onChannelChange() {
    const next = els.channelSelect.value;
    if (next === lastChannel) return;

    els.channelSelect.disabled = true;
    try {
        const r = await fetch('/api/ota/channel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ channel: next }),
        });
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        lastChannel = next;
        // Clear any previous check result — it was for the old channel.
        hideResult();
    } catch (e) {
        // Revert dropdown so the UI matches device state.
        els.channelSelect.value = lastChannel;
        showResult('error', `Couldn't change channel: ${e.message}`);
    } finally {
        els.channelSelect.disabled = false;
    }
}

/* ---- Check for updates -------------------------------------------------- */

async function onCheckClick() {
    if (!baseUrl || !repoPath) return;
    els.checkBtn.disabled = true;
    showResult('info', 'Checking for updates…');

    const channel = els.channelSelect.value;
    const url = `${baseUrl}/${repoPath}/releases/download/latest-${channel}/manifest.json`;

    try {
        const manifest = await fetchManifest(url);
        lastManifest = { manifest, baseManifestUrl: url, channel };
        renderManifestResult(manifest, channel);
    } catch (e) {
        renderFetchError(e, channel);
    } finally {
        els.checkBtn.disabled = false;
    }
}

async function fetchManifest(url) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), MANIFEST_TIMEOUT_MS);
    let r;
    try {
        r = await fetch(url, { signal: ctl.signal });
    } catch (e) {
        throw makeError('network', e.message);
    } finally {
        clearTimeout(timer);
    }
    if (r.status === 404) throw makeError('notfound');
    if (!r.ok)            throw makeError('network', `HTTP ${r.status}`);

    let data;
    try {
        data = await r.json();
    } catch (_) {
        throw makeError('badjson');
    }
    if (!validateManifest(data)) throw makeError('badjson');
    return data;
}

function validateManifest(m) {
    if (!m || typeof m !== 'object') return false;
    if (!m.app || !m.ui) return false;
    for (const k of ['version', 'sha256', 'url', 'size']) {
        if (typeof m.app[k] === 'undefined' || typeof m.ui[k] === 'undefined') return false;
    }
    if (typeof m.app.sha256 !== 'string' || m.app.sha256.length !== 64) return false;
    if (typeof m.ui.sha256  !== 'string' || m.ui.sha256.length  !== 64) return false;
    return true;
}

function makeError(kind, detail) {
    const e = new Error(detail || kind);
    e.kind = kind;
    return e;
}

function renderFetchError(e, channel) {
    if (e.kind === 'notfound') {
        showResult('info',
            `No ${CHANNEL_LABELS[channel] || channel} releases published yet.`);
    } else if (e.kind === 'badjson') {
        showResult('error',
            'Got an invalid response from the update server. Try again, and if it persists report it.');
    } else {
        showResult('error',
            "Can't reach the update server. Your phone or laptop needs internet access to check for updates — cellular data works while you're connected to the camera's WiFi.");
    }
}

function renderManifestResult(manifest, channel) {
    const v = manifest.app.version;
    const released = manifest.released ? ` (released ${manifest.released})` : '';
    const notes = manifest.release_notes_url
        ? ` <a href="${escapeAttr(manifest.release_notes_url)}" target="_blank" rel="noopener">release notes</a>`
        : '';
    els.result.className = 'upd-result upd-info';
    els.result.innerHTML =
        `<div class="upd-result-version">v${escapeHtml(v)}</div>` +
        `<div>${CHANNEL_LABELS[channel] || channel}${escapeHtml(released)}${notes}</div>` +
        `<div class="upd-result-actions">` +
            `<button class="settings-action-btn" id="upd-install-btn">Install v${escapeHtml(v)}</button>` +
        `</div>`;
    els.result.hidden = false;
    document.getElementById('upd-install-btn').addEventListener('click', onInstallClick);
}

/* ---- Install ------------------------------------------------------------ */

async function onInstallClick() {
    if (!lastManifest) return;
    installing = true;

    const { manifest, baseManifestUrl } = lastManifest;
    // Resolve blob URLs relative to the manifest URL (per §5).
    const appUrl = new URL(manifest.app.url, baseManifestUrl).toString();
    const uiUrl  = new URL(manifest.ui.url,  baseManifestUrl).toString();

    showInstallProgress(manifest.app.version);
    setStage('Downloading app…', 0);

    try {
        const appBytes = await downloadWithProgress(appUrl, manifest.app.size,
            (pct) => setStage('Downloading app…', pct, 0));

        setStage('Downloading UI…', 100, 0);
        const uiBytes = await downloadWithProgress(uiUrl, manifest.ui.size,
            (pct) => setStage('Downloading UI…', 100, pct, 0));

        setStage('Uploading app to device…', 100, 100, 0);
        await uploadToDevice('/api/ota/upload-app', appBytes, manifest.app.sha256,
            (pct) => setStage('Uploading app to device…', 100, 100, pct, 0));

        setStage('Uploading UI to device…', 100, 100, 100, 0);
        await uploadToDevice('/api/ota/upload-ui', uiBytes, manifest.ui.sha256,
            (pct) => setStage('Uploading UI to device…', 100, 100, 100, pct));

        setStage('Committing…', 100, 100, 100, 100);
        const commit = await postJson('/api/ota/commit');

        if (commit.rebooting === false) {
            // Manual UI-only edge case (auto-update never hits this since
            // versions are paired). Q6 — just prompt reload.
            showResult('success', 'UI updated. Reload the page to see changes.');
            installing = false;
            return;
        }

        showResult('info',
            `Device rebooting into v${escapeHtml(manifest.app.version)}. Waiting for it to come back up…`);
        const ok = await pollForVersion(manifest.app.version);
        if (ok) {
            showResult('success',
                `Updated to v${escapeHtml(manifest.app.version)}. Reload the page to use the new UI.`);
        } else {
            showResult('error',
                `Device didn't respond after ${POLL_OVERALL_MS / 1000}s. Reconnect to the camera's WiFi and reload this page to verify.`);
        }
    } catch (e) {
        showResult('error', `Install failed: ${escapeHtml(e.message || String(e))}`);
    } finally {
        installing = false;
    }
}

function showInstallProgress(version) {
    els.result.className = 'upd-result upd-info';
    els.result.innerHTML =
        `<div class="upd-result-version">Installing v${escapeHtml(version)}</div>` +
        `<div id="upd-stage-label" style="margin-top:8px">Starting…</div>` +
        progressRow('upd-prog-app-dl',  'Download app') +
        progressRow('upd-prog-ui-dl',   'Download UI') +
        progressRow('upd-prog-app-up',  'Upload app') +
        progressRow('upd-prog-ui-up',   'Upload UI');
    els.result.hidden = false;
}

function progressRow(id, label) {
    return `<div class="upd-progress-row">` +
        `<div class="upd-progress-label"><span>${label}</span><span id="${id}-pct">0%</span></div>` +
        `<div class="upd-progress-bar"><div class="upd-progress-fill" id="${id}-fill"></div></div>` +
        `</div>`;
}

function setStage(label, appDl, uiDl, appUp, uiUp) {
    const stage = document.getElementById('upd-stage-label');
    if (stage) stage.textContent = label;
    setBar('upd-prog-app-dl', appDl);
    setBar('upd-prog-ui-dl',  uiDl);
    setBar('upd-prog-app-up', appUp);
    setBar('upd-prog-ui-up',  uiUp);
}

function setBar(id, pct) {
    if (typeof pct !== 'number') return;
    const fill = document.getElementById(`${id}-fill`);
    const txt  = document.getElementById(`${id}-pct`);
    if (fill) fill.style.width = `${Math.max(0, Math.min(100, pct))}%`;
    if (txt)  txt.textContent  = `${Math.round(pct)}%`;
}

function downloadWithProgress(url, expectedSize, onProgress) {
    // XHR for download progress events; fetch's body stream has weaker
    // browser support for progress reporting.
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', url);
        xhr.responseType = 'arraybuffer';
        xhr.onprogress = (e) => {
            const total = e.lengthComputable ? e.total : expectedSize;
            if (total > 0) onProgress((e.loaded / total) * 100);
        };
        xhr.onload = () => {
            if (xhr.status >= 200 && xhr.status < 300) {
                onProgress(100);
                resolve(xhr.response);
            } else {
                reject(new Error(`download failed: HTTP ${xhr.status}`));
            }
        };
        xhr.onerror = () => reject(new Error('download failed: network error'));
        xhr.onabort = () => reject(new Error('download aborted'));
        xhr.send();
    });
}

function uploadToDevice(path, bytes, sha256, onProgress) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', path);
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        xhr.setRequestHeader('X-Sha256', sha256);
        xhr.setRequestHeader('X-Size', String(bytes.byteLength));
        xhr.upload.onprogress = (e) => {
            if (e.lengthComputable) onProgress((e.loaded / e.total) * 100);
        };
        xhr.onload = () => {
            if (xhr.status >= 200 && xhr.status < 300) {
                onProgress(100);
                resolve();
            } else {
                let detail = xhr.responseText;
                try { detail = JSON.parse(detail).error || detail; } catch (_) {}
                reject(new Error(`upload failed (HTTP ${xhr.status}): ${detail}`));
            }
        };
        xhr.onerror = () => reject(new Error('upload failed: network error'));
        xhr.onabort = () => reject(new Error('upload aborted'));
        xhr.send(bytes);
    });
}

async function postJson(path, body) {
    const r = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: body ? JSON.stringify(body) : '',
    });
    if (!r.ok) throw new Error(`${path} → HTTP ${r.status}`);
    const text = await r.text();
    return text ? JSON.parse(text) : {};
}

/* ---- Poll for the new version ------------------------------------------ */

async function pollForVersion(targetVersion) {
    await sleep(POLL_INITIAL_DELAY_MS);
    const deadline = Date.now() + POLL_OVERALL_MS - POLL_INITIAL_DELAY_MS;
    while (Date.now() < deadline) {
        const v = await fetchVersionWithTimeout();
        if (v && v.app === targetVersion) return true;
        await sleep(POLL_INTERVAL_MS);
    }
    return false;
}

async function fetchVersionWithTimeout() {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), POLL_REQ_TIMEOUT_MS);
    try {
        const r = await fetch('/api/version', { signal: ctl.signal });
        if (!r.ok) return null;
        return await r.json();
    } catch (_) {
        return null;
    } finally {
        clearTimeout(timer);
    }
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

/* ---- Restart to recovery ------------------------------------------------ */

async function onRestartRecoveryClick() {
    const ok = confirm(
        'Restart into recovery mode?\n\n' +
        "You'll need to reconnect to the camera's WiFi after it restarts. " +
        'Continue?');
    if (!ok) return;

    els.recoveryBtn.disabled = true;
    els.recoveryBtn.textContent = 'Restarting…';
    try {
        await postJson('/api/ota/reboot-recovery');
        showResult('info',
            "Restarting into recovery. Reconnect to the camera's WiFi and reload this page.");
    } catch (e) {
        showResult('error', `Restart failed: ${e.message}`);
        els.recoveryBtn.disabled = false;
        els.recoveryBtn.textContent = 'Restart to Recovery';
    }
}

/* ---- Result area ------------------------------------------------------- */

function showResult(kind, html) {
    els.result.className = `upd-result upd-${kind}`;
    els.result.innerHTML = html;
    els.result.hidden = false;
}

function hideResult() {
    els.result.hidden = true;
    els.result.innerHTML = '';
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[c]));
}
function escapeAttr(s) { return escapeHtml(s); }

})();
