// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/ui/webui/enclave_shard/enclave_shard_page.h"

namespace enclave {

// Static markup only. Everything that changes is filled in by shard.js
// with textContent / classList / hidden, so the page works under the default
// WebUI CSP (no inline script, no inline handlers, Trusted Types enforced).
const char kEnclaveShardHtml[] = R"BZL(<!doctype html>
<html lang="en" dir="ltr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>Sync via Shard</title>
<link rel="stylesheet" href="shard.css">
<script src="shard.js" defer></script>
</head>
<body>
<div class="hm-page">
  <div class="hm-grid-bg" aria-hidden="true"></div>
  <main class="hm-canvas">
    <h1 class="hm-page-title">Sync via Shard</h1>

    <section class="hm-card cs-status" aria-labelledby="status-title">
      <div id="status-bar" class="hm-card-status-bar hm-card-status-bar--paused"></div>
      <div class="cs-card-body">
        <div class="cs-status-head">
          <h2 id="status-title" class="hm-section-title">Status</h2>
          <span id="status-badge" class="hm-badge hm-badge--default">Starting</span>
        </div>
        <p id="status-text" class="cs-text" role="status" aria-live="polite">Looking for Shard&hellip;</p>
        <p id="status-meta" class="cs-muted" hidden></p>
      </div>
    </section>

    <div id="message" class="hm-banner hm-banner--info" role="status" hidden></div>
    <div id="error" class="hm-banner hm-banner--error" role="alert" hidden></div>

    <section class="hm-card" aria-labelledby="switch-label">
      <div class="cs-card-body">
        <label class="hm-toggle-row" id="switch-row">
          <input type="checkbox" role="switch" id="switch" class="hm-toggle__input">
          <span class="hm-toggle" aria-hidden="true"><span class="hm-toggle__thumb"></span></span>
          <span class="hm-toggle-row__text">
            <span class="hm-toggle-row__label" id="switch-label">Sync via Shard</span>
            <span class="hm-toggle-row__hint" id="switch-hint">Bookmarks, history, open tabs, settings and more go to your Shard vault. Passwords go to Shard's own password store.</span>
          </span>
        </label>

        <div id="pairing" class="cs-pairing" hidden>
          <div class="hm-stat-card__label">Pairing code</div>
          <div id="pairing-code" class="cs-pairing-code">--- ---</div>
          <p class="cs-text">Confirm this code in Shard.</p>
          <div class="cs-actions">
            <button type="button" id="cancel-pairing" class="hm-btn hm-btn--neutral">Cancel</button>
          </div>
        </div>
      </div>
    </section>

    <section id="details" class="cs-details" aria-labelledby="details-title" hidden>
      <h2 id="details-title" class="hm-section-title">Sync</h2>
      <div class="hm-stat-cards">
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Profile in Shard</div>
          <div id="stat-profile" class="hm-stat-card__value">&mdash;</div>
        </div>
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Replication</div>
          <div id="stat-replicating" class="hm-stat-card__value">&mdash;</div>
        </div>
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Records</div>
          <div id="stat-records" class="hm-stat-card__value">&mdash;</div>
        </div>
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Size</div>
          <div id="stat-bytes" class="hm-stat-card__value">&mdash;</div>
        </div>
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Last write</div>
          <div id="stat-last-write" class="hm-stat-card__value">&mdash;</div>
        </div>
        <div class="hm-stat-card">
          <div class="hm-stat-card__label">Passwords</div>
          <div id="stat-passwords" class="hm-stat-card__value">&mdash;</div>
        </div>
      </div>
      <div class="cs-actions">
        <button type="button" id="disable" class="hm-btn hm-btn--cyan">Stop sync</button>
        <button type="button" id="remove" class="hm-btn hm-btn--red">Remove synced data from Shard</button>
      </div>
    </section>
  </main>
</div>

<div id="confirm" class="hm-modal-overlay" hidden>
  <div class="hm-modal hm-modal--sm" role="alertdialog" aria-modal="true"
       aria-labelledby="confirm-title" aria-describedby="confirm-text">
    <div class="hm-modal__handle" aria-hidden="true"></div>
    <h3 id="confirm-title" class="hm-modal__title">Remove synced data?</h3>
    <p id="confirm-text" class="cs-text">The records of this profile are deleted from Shard and sync is switched off for it. Data on this device stays. Other devices keep their own copies.</p>
    <div class="hm-modal__footer">
      <button type="button" id="confirm-cancel" class="hm-btn hm-btn--neutral">Cancel</button>
      <button type="button" id="confirm-ok" class="hm-btn hm-btn--red">Remove</button>
    </div>
  </div>
</div>

<div id="toasts" class="hm-toasts" aria-live="polite"></div>
</body>
</html>
)BZL";

const char kEnclaveShardCss[] = R"BZL(/* Dark-only design tokens; phone layout below 768 px at the end. */
:root {
  --hm-bg: #080c10;
  --hm-surface: #0d1117;
  --hm-border: #1c2333;
  --hm-accent: #00d4ff;
  --hm-text: #e6edf3;
  --hm-muted: #8b949e;
  --hm-ok: #10b981;
  --hm-error: #ef4444;
  --hm-active: #3b82f6;
  --hm-paused: #6b7280;
  --hm-warning: #f59e0b;
  --hm-purple: #7c3aed;
  --hm-purple-l: #a78bfa;
  --hm-safe-bottom: env(safe-area-inset-bottom, 0px);
  font-family: 'JetBrains Mono', monospace;
  color-scheme: dark;
}

* { box-sizing: border-box; }

body {
  margin: 0;
  background-color: var(--hm-bg);
  color: var(--hm-text);
  border: 8px solid #000000; /* viewport enclave, sec. 5.3 */
  overflow-x: hidden;
  font-size: 13px;
  min-height: 100dvh;
}

[hidden] { display: none !important; }

.hm-page {
  min-height: calc(100dvh - 16px);
  background: var(--hm-bg);
  position: relative;
  display: flex;
  flex-direction: column;
}

.hm-grid-bg {
  position: fixed;
  inset: 0;
  background-image:
    linear-gradient(rgba(0, 212, 255, 0.03) 1px, transparent 1px),
    linear-gradient(90deg, rgba(0, 212, 255, 0.03) 1px, transparent 1px);
  background-size: 40px 40px;
  pointer-events: none;
  z-index: 0;
}

.hm-canvas {
  position: relative;
  z-index: 1;
  flex: 1;
  padding: 32px 40px 60px;
  display: flex;
  flex-direction: column;
  gap: 28px;
  width: 100%;
  max-width: 960px;
}

.hm-page-title {
  font-family: 'Syne', sans-serif;
  font-size: 22px;
  font-weight: 800;
  letter-spacing: -0.5px;
  line-height: 1.2;
  margin: 0;
}

.hm-section-title {
  display: flex;
  align-items: center;
  gap: 12px;
  font-family: 'Syne', sans-serif;
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 1.5px;
  color: var(--hm-muted);
  margin: 0;
  flex: 1;
}
.hm-section-title::after {
  content: '';
  flex: 1;
  height: 1px;
  background: var(--hm-border);
}

/* Cards, sec. 8 */
.hm-card {
  background: var(--hm-surface);
  border: 1px solid var(--hm-border);
  border-radius: 12px;
  overflow: hidden;
  position: relative;
}
.hm-card-status-bar {
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 2px;
}
.hm-card-status-bar--ok { background: var(--hm-ok); }
.hm-card-status-bar--error { background: var(--hm-error); }
.hm-card-status-bar--paused { background: var(--hm-paused); }
.hm-card-status-bar--warning { background: var(--hm-warning); }

.cs-card-body {
  padding: 20px;
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.cs-status .cs-card-body { padding-left: 20px; gap: 12px; }
.cs-status-head {
  display: flex;
  align-items: center;
  gap: 12px;
}
.cs-text { margin: 0; font-size: 13px; line-height: 1.5; }
.cs-muted { margin: 0; font-size: 12px; color: var(--hm-muted); }

.hm-stat-cards {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
}
.hm-stat-card {
  min-width: 10vw;
  background: var(--hm-surface);
  border: 1px solid var(--hm-border);
  border-radius: 12px;
  padding: 16px 20px;
  overflow: hidden;
  flex: 1;
}
.hm-stat-card__label {
  font-family: 'Syne', sans-serif;
  font-size: 10px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 1px;
  color: var(--hm-muted);
  margin-bottom: 8px;
}
.hm-stat-card__value {
  font-family: 'Syne', sans-serif;
  font-size: 20px;
  font-weight: 800;
  color: var(--hm-text);
  white-space: nowrap;
}

.cs-details {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

/* Pairing: the code is the page's one large value (22 px, sec. 3.2 top row). */
.cs-pairing {
  background: var(--hm-bg);
  border: 1px solid rgba(0, 212, 255, 0.30); /* --hm-accent @ 30% */
  border-radius: 12px;
  padding: 16px 20px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.cs-pairing .hm-stat-card__label { margin-bottom: 0; }
.cs-pairing-code {
  font-family: 'Syne', sans-serif;
  font-size: 22px;
  font-weight: 800;
  letter-spacing: -0.5px;
  color: var(--hm-accent);
}

/* Badges, sec. 12.1 */
.hm-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-family: 'JetBrains Mono', monospace;
  font-size: 10px;
  text-transform: uppercase;
  font-weight: 600;
  letter-spacing: 0.5px;
  white-space: nowrap;
}
.hm-badge--ok { background: rgba(16, 185, 129, 0.15); color: #10b981; border: 1px solid rgba(16, 185, 129, 0.30); }
.hm-badge--error { background: rgba(239, 68, 68, 0.15); color: #ef4444; border: 1px solid rgba(239, 68, 68, 0.30); }
.hm-badge--default { background: rgba(107, 114, 128, 0.15); color: #6b7280; }

/* Banners, sec. 19.2 */
.hm-banner {
  display: flex;
  align-items: flex-start;
  gap: 8px;
  padding: 12px 16px;
  border-radius: 8px;
  font-family: 'JetBrains Mono', monospace;
  font-size: 12px;
  overflow-wrap: anywhere;
}
.hm-banner--error { background: rgba(239, 68, 68, 0.08); border: 1px solid rgba(239, 68, 68, 0.30); color: #ef4444; }
.hm-banner--info { background: rgba(0, 212, 255, 0.08); border: 1px solid rgba(0, 212, 255, 0.30); color: var(--hm-accent); }

/* Buttons, sec. 7 */
.hm-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  height: 30px;
  padding: 0 14px;
  border-radius: 8px;
  font-family: 'JetBrains Mono', monospace;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.5px;
  cursor: pointer;
  transition: all 0.15s;
  border: 1px solid transparent;
}
.hm-btn:disabled { opacity: 0.5; cursor: not-allowed; }
.hm-btn:focus-visible { outline: 1px solid var(--hm-accent); outline-offset: 2px; }
.hm-btn--cyan { background: rgba(0, 212, 255, 0.10); border-color: rgba(0, 212, 255, 0.30); color: var(--hm-accent); }
.hm-btn--red { background: rgba(239, 68, 68, 0.10); border-color: rgba(239, 68, 68, 0.30); color: #ef4444; }
.hm-btn--neutral { background: transparent; border-color: var(--hm-border); color: var(--hm-muted); }
@media (hover: hover) {
  .hm-btn--cyan:hover:not(:disabled) { background: rgba(0, 212, 255, 0.20); }
  .hm-btn--red:hover:not(:disabled) { background: rgba(239, 68, 68, 0.20); }
  .hm-btn--neutral:hover:not(:disabled) { background: rgba(139, 148, 158, 0.10); /* --hm-muted @ 10% */ }
}
.cs-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}

/* Toggle, sec. 13 */
.hm-toggle-row {
  display: flex;
  align-items: flex-start;
  gap: 12px;
  cursor: pointer;
  position: relative;
}
.hm-toggle-row--disabled { opacity: 0.5; cursor: not-allowed; }
.hm-toggle__input {
  position: absolute;
  opacity: 0;
  width: 1px;
  height: 1px;
  margin: 0;
  pointer-events: none;
}
.hm-toggle {
  position: relative;
  flex-shrink: 0;
  width: 40px;
  height: 22px;
  border-radius: 11px;
  background: var(--hm-border);
  transition: background 0.2s;
}
.hm-toggle__thumb {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: #fff;
  position: absolute;
  top: 2px;
  left: 2px;
  transition: transform 0.2s;
}
.hm-toggle__input:checked + .hm-toggle { background: var(--hm-accent); }
.hm-toggle__input:checked + .hm-toggle .hm-toggle__thumb { transform: translateX(18px); }
.hm-toggle__input:focus-visible + .hm-toggle { outline: 1px solid var(--hm-accent); outline-offset: 2px; }
.hm-toggle-row__text { display: flex; flex-direction: column; gap: 4px; }
.hm-toggle-row__label { font-size: 13px; font-weight: 600; color: var(--hm-text); }
.hm-toggle-row__hint { font-size: 12px; color: var(--hm-muted); line-height: 1.5; }

/* Modal, sec. 14 */
.hm-modal-overlay {
  position: fixed;
  inset: 0;
  z-index: 100;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.6);
  backdrop-filter: blur(4px);
}
.hm-modal {
  background: var(--hm-surface);
  border: 1px solid var(--hm-border);
  border-radius: 14px;
  padding: 24px;
  width: 520px;
  max-width: 90vw;
  max-height: 90vh;
  overflow-y: auto;
}
.hm-modal--sm { width: 380px; }
.hm-modal__handle { display: none; }
.hm-modal__title {
  font-family: 'Syne', sans-serif;
  font-size: 15px;
  font-weight: 700;
  color: var(--hm-text);
  margin: 0 0 20px;
}
.hm-modal__footer {
  display: flex;
  justify-content: flex-end;
  gap: 12px;
  margin-top: 24px;
  padding-top: 16px;
  border-top: 1px solid var(--hm-border);
}

/* Toasts, sec. 15.3 */
.hm-toasts {
  position: fixed;
  bottom: 24px;
  right: 24px;
  z-index: 100;
  display: flex;
  flex-direction: column-reverse;
  gap: 8px;
  pointer-events: none;
}
.hm-toast {
  pointer-events: auto;
  background: var(--hm-surface);
  border: 1px solid var(--hm-border);
  border-radius: 8px;
  padding: 12px 16px;
  min-width: 280px;
  max-width: 420px;
  font-family: 'JetBrains Mono', monospace;
  font-size: 12px;
  color: var(--hm-text);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
  display: flex;
  align-items: flex-start;
  gap: 10px;
}
.hm-toast--success { border-left: 3px solid var(--hm-ok); }
.hm-toast--error { border-left: 3px solid var(--hm-error); }
.hm-toast__body { flex: 1; display: flex; flex-direction: column; gap: 4px; overflow-wrap: anywhere; }
.hm-toast__title { font-weight: 600; }
.hm-toast__text { color: var(--hm-muted); }
.hm-toast__close {
  background: transparent;
  border: none;
  color: var(--hm-muted);
  font: inherit;
  cursor: pointer;
  padding: 0 4px;
}

::selection { background: rgba(0, 212, 255, 0.20); }

/* Phones and narrow windows (below 768 px). */
@media (max-width: 768px) {
  body { border: 0; }
  .hm-page { min-height: 100dvh; }
  .hm-canvas {
    padding: 12px 16px calc(24px + var(--hm-safe-bottom));
    gap: 16px;
  }
  .cs-details { gap: 12px; }
  .hm-stat-cards {
    flex-wrap: nowrap;
    overflow-x: auto;
    margin: 0 -16px;
    padding: 0 16px;
    gap: 12px;
    scrollbar-width: none;
  }
  .hm-stat-card { flex: 0 0 auto; min-width: 140px; }
  .hm-btn { height: 44px; padding: 0 16px; font-size: 12px; }
  .cs-actions { flex-direction: column-reverse; align-items: stretch; }
  .cs-actions .hm-btn { width: 100%; }
  .hm-toggle-row { min-height: 48px; align-items: center; }
  .hm-btn--cyan:active:not(:disabled) { background: rgba(0, 212, 255, 0.20); }
  .hm-btn--red:active:not(:disabled) { background: rgba(239, 68, 68, 0.20); }
  .hm-btn--neutral:active:not(:disabled) { background: rgba(139, 148, 158, 0.10); }
  .hm-modal-overlay { align-items: flex-end; }
  .hm-modal,
  .hm-modal--sm {
    width: 100%;
    max-width: 100%;
    max-height: 92vh;
    border-radius: 16px 16px 0 0;
    border-bottom: none;
    padding: 20px 16px calc(20px + var(--hm-safe-bottom));
    box-shadow: 0 -8px 24px rgba(0, 0, 0, 0.4);
  }
  .hm-modal__handle {
    display: block;
    width: 36px;
    height: 4px;
    border-radius: 2px;
    background: var(--hm-border);
    margin: 0 auto 16px;
  }
  .hm-modal__footer { flex-direction: column-reverse; }
  .hm-modal__footer .hm-btn { width: 100%; }
  .hm-toasts {
    left: 16px;
    right: 16px;
    bottom: calc(var(--hm-safe-bottom) + 16px);
  }
  .hm-toast { min-width: 0; max-width: none; }
  .hm-toast__close { min-width: 44px; min-height: 44px; }
}

@media (prefers-reduced-motion: reduce) {
  .hm-btn, .hm-toggle, .hm-toggle__thumb { transition: none; }
}
)BZL";

const char kEnclaveShardJs[] = R"BZL('use strict';

// The two globals WebUIMessageHandler calls into (normally from cr.js,
// ui/webui/resources/js/cr.ts): cr.webUIListenerCallback for
// FireWebUIListener, cr.webUIResponse for ResolveJavascriptCallback (unused).
(function() {
  const listeners = new Map();
  window.cr = {
    webUIListenerCallback(event, ...args) {
      const callback = listeners.get(event);
      if (callback) {
        callback(...args);
      }
    },
    webUIResponse() {},
  };

  const $ = (id) => document.getElementById(id);

  const PHASES = {
    unavailable: {badge: 'Unavailable', bar: 'paused', kind: 'default',
                  text: 'Sync via Shard is not available in this profile.'},
    starting: {badge: 'Starting', bar: 'paused', kind: 'default',
               text: 'Looking for Shard\u2026'},
    not_running: {badge: 'Not running', bar: 'paused', kind: 'default',
                  text: 'Shard is not running on this device. Start it to sync.'},
    incompatible: {badge: 'Incompatible', bar: 'error', kind: 'error',
                   text: 'This Shard version is not supported.'},
    no_vault: {badge: 'No vault', bar: 'warning', kind: 'default',
               text: 'Shard has no vault yet. Create one in Shard to sync.'},
    locked: {badge: 'Locked', bar: 'warning', kind: 'default',
             text: 'Unlock Shard to sync.'},
    offline: {badge: 'Offline', bar: 'warning', kind: 'default',
              text: 'Shard is offline; changes wait.'},
    ready: {badge: 'Ready', bar: 'ok', kind: 'ok', text: ''},
  };

  // Pairing needs a running, compatible Shard.
  const CAN_ENABLE = new Set(['no_vault', 'locked', 'offline', 'ready']);

  let state = {phase: 'starting'};
  let busy = false;

  function setVariant(el, prefix, variant) {
    for (const name of [...el.classList]) {
      if (name.startsWith(prefix)) {
        el.classList.remove(name);
      }
    }
    el.classList.add(prefix + variant);
  }

  function formatBytes(bytes) {
    if (!bytes) {
      return '0 B';
    }
    const units = ['B', 'KiB', 'MiB', 'GiB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    return (unit === 0 ? String(value) : value.toFixed(1)) + ' ' + units[unit];
  }

  function formatTime(ms) {
    if (!ms) {
      return 'Never';
    }
    const date = new Date(ms);
    const now = new Date();
    if (date.toDateString() === now.toDateString()) {
      return date.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
    }
    return date.toLocaleDateString([], {day: 'numeric', month: 'short'}) +
        ' ' + date.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
  }

  function statusText(s) {
    const phase = PHASES[s.phase] || PHASES.starting;
    if (s.phase === 'incompatible' && s.message) {
      return s.message;
    }
    if (s.phase !== 'ready') {
      return phase.text;
    }
    if (s.pairing) {
      return 'Waiting for you to confirm in Shard.';
    }
    if (!s.enabled) {
      return 'Shard is ready. Turn on sync to start.';
    }
    return s.replicating ? 'Syncing.' : 'Sync is on.';
  }

  function render(s) {
    state = s || {phase: 'starting'};
    const phase = PHASES[state.phase] || PHASES.starting;

    $('status-badge').textContent = phase.badge;
    setVariant($('status-badge'), 'hm-badge--', phase.kind);
    setVariant($('status-bar'), 'hm-card-status-bar--', phase.bar);
    $('status-text').textContent = statusText(state);

    const meta = [];
    if (state.coreVersion) {
      meta.push('Shard ' + state.coreVersion);
    }
    if (state.deviceName) {
      meta.push(state.deviceName);
    }
    $('status-meta').textContent = meta.join(' \u00b7 ');
    $('status-meta').hidden = meta.length === 0;

    // `message` is shown as the status line for "incompatible".
    const message = state.phase === 'incompatible' ? '' : (state.message || '');
    $('message').textContent = message;
    $('message').hidden = !message;
    $('error').textContent = state.error || '';
    $('error').hidden = !state.error;

    const enabled = !!state.enabled;
    const pairing = !!state.pairing;
    const sw = $('switch');
    sw.checked = enabled || pairing;
    sw.disabled = busy ||
        (!enabled && !pairing && !CAN_ENABLE.has(state.phase));
    $('switch-row').classList.toggle('hm-toggle-row--disabled', sw.disabled);

    $('pairing').hidden = !pairing;
    $('pairing-code').textContent = state.pairingCode || '--- ---';

    $('details').hidden = !enabled || pairing;
    $('stat-replicating').textContent = state.replicating ? 'Running' : 'Paused';
    const limit = state.recordLimit || 0;
    // The limit is shared by every profile of the vault.
    $('stat-records').textContent = (state.profileRecords || 0) +
        ' of ' + (state.records || 0) + (limit ? ' / ' + limit : '');
    $('stat-profile').textContent = state.slotName ?
        state.slotName + (state.primarySlot ? ' (primary)' : '') :
        'Not chosen yet';
    $('stat-bytes').textContent = formatBytes(state.bytes || 0);
    $('stat-last-write').textContent = formatTime(state.lastWrite || 0);
    $('stat-passwords').textContent =
        state.passwords ? 'Granted' : 'Not granted';
    $('disable').disabled = busy;
    $('remove').disabled = busy;
  }

  function toast(kind, title, text) {
    const el = document.createElement('div');
    el.className = 'hm-toast hm-toast--' + kind;
    el.setAttribute('role', kind === 'error' ? 'alert' : 'status');
    const body = document.createElement('div');
    body.className = 'hm-toast__body';
    const head = document.createElement('div');
    head.className = 'hm-toast__title';
    head.textContent = title;
    body.appendChild(head);
    if (text) {
      const line = document.createElement('div');
      line.className = 'hm-toast__text';
      line.textContent = text;
      body.appendChild(line);
    }
    const close = document.createElement('button');
    close.type = 'button';
    close.className = 'hm-toast__close';
    close.setAttribute('aria-label', 'Close');
    close.textContent = '\u2715';
    close.addEventListener('click', () => el.remove());
    el.appendChild(body);
    el.appendChild(close);
    $('toasts').appendChild(el);
    // Errors stay until closed; other toasts go after 4 s.
    if (kind !== 'error') {
      setTimeout(() => el.remove(), 4000);
    }
  }

  let lastFocus = null;

  function openConfirm() {
    lastFocus = document.activeElement;
    $('confirm').hidden = false;
    $('confirm-cancel').focus();
  }

  function closeConfirm() {
    $('confirm').hidden = true;
    if (lastFocus && lastFocus.focus) {
      lastFocus.focus();
    }
  }

  function onSwitch() {
    const sw = $('switch');
    if (sw.checked) {
      chrome.send('enable');
    } else if (state.pairing) {
      chrome.send('cancelPairing');
    } else {
      chrome.send('disable');
    }
  }

  function onRemoveDone(result) {
    busy = false;
    if (result && result.ok) {
      toast('success', 'Synced data removed from Shard.', '');
    } else {
      toast('error', 'Could not remove synced data.',
            (result && result.message) || '');
    }
    render(state);
  }

  function init() {
    listeners.set('shard-state', render);
    listeners.set('shard-remove-done', onRemoveDone);

    $('switch').addEventListener('change', onSwitch);
    $('cancel-pairing').addEventListener('click',
                                         () => chrome.send('cancelPairing'));
    $('disable').addEventListener('click', () => chrome.send('disable'));
    $('remove').addEventListener('click', openConfirm);
    $('confirm-cancel').addEventListener('click', closeConfirm);
    $('confirm').addEventListener('click', (e) => {
      if (e.target === $('confirm')) {
        closeConfirm();
      }
    });
    $('confirm-ok').addEventListener('click', () => {
      closeConfirm();
      busy = true;
      render(state);
      chrome.send('removeData');
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !$('confirm').hidden) {
        closeConfirm();
      }
    });

    render(state);
    chrome.send('getState');
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
)BZL";

}  // namespace enclave
