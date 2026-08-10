(function initDraftPhase7(root) {
  'use strict';

  const STORAGE_KEY = 'cff_phase7_draft_operations';
  const POLL_MS = 4000;
  let latestSnapshot = null;
  let pollTimer = null;
  let loading = false;

  function currentLeague() {
    return root.getLeagueState?.() || null;
  }

  function isCommissioner() {
    return Boolean(root.isCurrentCommissioner?.(currentLeague()));
  }

  function lifecycleState(snapshot = latestSnapshot) {
    if (snapshot?.lifecycleState) return String(snapshot.lifecycleState).toUpperCase();
    const status = String(snapshot?.status || '').toLowerCase();
    if (status === 'complete') return 'COMPLETED';
    if (status === 'cancelled') return 'CANCELLED';
    if (status === 'paused') return 'PAUSED';
    if (status === 'open') return 'IN_PROGRESS';
    if (snapshot?.lobbyOpen) return 'LOBBY_OPEN';
    if (snapshot?.draftDate || currentLeague()?.draftDate) return 'SCHEDULED';
    return 'NOT_SCHEDULED';
  }

  function snapshotVersion(snapshot) {
    const value = Number(snapshot?.version ?? snapshot?.revision);
    return Number.isFinite(value) ? value : -1;
  }

  function shouldReplaceSnapshot(incoming, current = latestSnapshot) {
    if (!incoming) return false;
    if (!current) return true;
    const incomingVersion = snapshotVersion(incoming);
    const currentVersion = snapshotVersion(current);
    if (incomingVersion < 0 || currentVersion < 0) return true;
    return incomingVersion >= currentVersion;
  }

  function currentSnapshot() {
    const lifecycleSnapshot = root.CFFDraftLifecycle?.latest?.() || null;
    if (!latestSnapshot) return lifecycleSnapshot;
    if (!lifecycleSnapshot) return latestSnapshot;
    return snapshotVersion(lifecycleSnapshot) > snapshotVersion(latestSnapshot)
      ? lifecycleSnapshot
      : latestSnapshot;
  }

  function stateLabel(state) {
    return String(state || '')
      .toLowerCase()
      .split('_')
      .map((part) => part ? part[0].toUpperCase() + part.slice(1) : '')
      .join(' ');
  }

  function clockLabel(snapshot = currentSnapshot()) {
    const state = lifecycleState(snapshot);
    if (state === 'PAUSED') return 'Paused';
    if (state === 'COMPLETED') return 'Done';
    return null;
  }

  function installClockRenderer() {
    const original = root.renderDraftClock;
    if (typeof original !== 'function' || original.__cffPhase7Wrapped) return;
    const wrapped = function phase7DraftClockRenderer(...args) {
      const label = clockLabel(currentSnapshot() || root.getDraftMeta?.());
      if (label === 'Paused') {
        const clock = root.document?.getElementById?.('draft-clock');
        if (clock) clock.textContent = label;
        return;
      }
      return original.apply(this, args);
    };
    wrapped.__cffPhase7Wrapped = true;
    root.renderDraftClock = wrapped;
  }

  function acceptSnapshot(snapshot) {
    const current = currentSnapshot();
    if (!shouldReplaceSnapshot(snapshot, current)) {
      if (current) latestSnapshot = current;
      return current;
    }
    latestSnapshot = snapshot;
    root.applyDraftState?.(snapshot);
    return latestSnapshot;
  }

  async function syncLifecycleCache() {
    if (typeof root.CFFDraftLifecycle?.sync !== 'function') return currentSnapshot();
    try {
      const synchronized = await root.CFFDraftLifecycle.sync();
      return synchronized ? acceptSnapshot(synchronized) : currentSnapshot();
    } catch {
      return currentSnapshot();
    }
  }

  function readOperations() {
    try {
      return JSON.parse(root.sessionStorage?.getItem?.(STORAGE_KEY) || '{}') || {};
    } catch {
      return {};
    }
  }

  function writeOperations(value) {
    try {
      root.sessionStorage?.setItem?.(STORAGE_KEY, JSON.stringify(value));
    } catch {
      // Safe retries are best effort when storage is unavailable.
    }
  }

  function operationFor(action, fingerprint) {
    const leagueId = String(currentLeague()?.id || '');
    const key = `${leagueId}:${action}`;
    const operations = readOperations();
    const existing = operations[key];
    if (existing?.operationKey && existing.fingerprint === fingerprint) return existing;
    const operation = {
      operationKey: root.crypto?.randomUUID?.() || `draft-${Date.now()}-${Math.random().toString(36).slice(2)}`,
      fingerprint,
      createdAt: Date.now()
    };
    operations[key] = operation;
    writeOperations(operations);
    return operation;
  }

  function clearOperation(action, operationKey) {
    const leagueId = String(currentLeague()?.id || '');
    const key = `${leagueId}:${action}`;
    const operations = readOperations();
    if (!operations[key] || (operationKey && operations[key].operationKey !== operationKey)) return;
    delete operations[key];
    writeOperations(operations);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut || error?.unavailable || error?.retryable || !status || status >= 500);
  }

  function ensureControls() {
    installClockRenderer();
    const lobby = root.document?.querySelector?.('#draft-room-content .card.card--accent');
    if (!lobby) return;

    let controls = root.document.getElementById('draft-phase7-controls');
    if (!controls) {
      controls = root.document.createElement('div');
      controls.id = 'draft-phase7-controls';
      controls.className = 'cta-row section-actions';
      controls.innerHTML = `
        <button class="button" id="draft-pause" type="button">Pause draft</button>
        <button class="button" id="draft-resume" type="button">Resume draft</button>
        <button class="button button--ghost" id="draft-cancel" type="button">Cancel draft</button>
        <span class="muted small" id="draft-phase7-status" role="status"></span>
      `;
      lobby.appendChild(controls);
      root.document.getElementById('draft-pause')?.addEventListener('click', () => mutate('pause', '/draft/pause'));
      root.document.getElementById('draft-resume')?.addEventListener('click', () => mutate('resume', '/draft/resume'));
      root.document.getElementById('draft-cancel')?.addEventListener('click', () => mutate('cancel', '/draft/cancel'));
    }

    let settings = root.document.getElementById('draft-phase7-settings');
    if (!settings) {
      settings = root.document.createElement('div');
      settings.id = 'draft-phase7-settings';
      settings.className = 'row';
      settings.innerHTML = `
        <div>
          <strong>Draft settings</strong>
          <div class="muted small">Pick duration and timezone are stored by the server before the draft starts.</div>
        </div>
        <div class="actions">
          <label class="muted small">Pick time
            <select id="draft-pick-duration" aria-label="Draft pick duration">
              <option value="15">15 sec</option>
              <option value="30">30 sec</option>
              <option value="45">45 sec</option>
              <option value="60">60 sec</option>
              <option value="90">90 sec</option>
              <option value="120">2 min</option>
              <option value="180">3 min</option>
              <option value="300">5 min</option>
              <option value="600">10 min</option>
            </select>
          </label>
          <label class="muted small">Timezone
            <input id="draft-timezone" type="text" maxlength="64" aria-label="Draft timezone" />
          </label>
          <button class="button" id="draft-save-settings" type="button">Save settings</button>
        </div>
      `;
      lobby.appendChild(settings);
      root.document.getElementById('draft-save-settings')?.addEventListener('click', () => {
        const seconds = Number(root.document.getElementById('draft-pick-duration')?.value || 90);
        const timezone = String(root.document.getElementById('draft-timezone')?.value || 'UTC').trim();
        return mutate('settings', '/draft/settings', { pickClockSeconds: seconds, timezone });
      });
    }

    let recap = root.document.getElementById('draft-phase7-recap');
    if (!recap) {
      recap = root.document.createElement('section');
      recap.id = 'draft-phase7-recap';
      recap.className = 'card draft-side';
      recap.hidden = true;
      recap.innerHTML = `
        <div class="card__header">
          <h2>Draft Recap</h2>
          <span class="pill">Final</span>
        </div>
        <div id="draft-phase7-recap-body" class="list"></div>
      `;
      root.document.querySelector('.draft-dashboard')?.appendChild(recap);
    }
  }

  function render(snapshot = currentSnapshot()) {
    ensureControls();
    if (!snapshot) return;
    const state = lifecycleState(snapshot);
    const commissioner = isCommissioner();
    const preStart = ['NOT_SCHEDULED', 'SCHEDULED', 'LOBBY_OPEN', 'CANCELLED'].includes(state);
    const badge = root.document?.getElementById?.('draft-lobby-badge');
    const status = root.document?.getElementById?.('draft-phase7-status');
    const pause = root.document?.getElementById?.('draft-pause');
    const resume = root.document?.getElementById?.('draft-resume');
    const cancel = root.document?.getElementById?.('draft-cancel');
    const settings = root.document?.getElementById?.('draft-phase7-settings');
    const saveSettings = root.document?.getElementById?.('draft-save-settings');
    const duration = root.document?.getElementById?.('draft-pick-duration');
    const timezone = root.document?.getElementById?.('draft-timezone');
    const clock = root.document?.getElementById?.('draft-clock');

    if (badge) badge.textContent = stateLabel(state);
    if (status) status.textContent = state === 'PAUSED'
      ? `Clock paused with ${Number(snapshot.pausedRemainingSeconds || 0)} seconds remaining.`
      : '';
    const persistentClockLabel = clockLabel(snapshot);
    if (clock && persistentClockLabel) clock.textContent = persistentClockLabel;
    if (pause) pause.hidden = !commissioner || state !== 'IN_PROGRESS';
    if (resume) resume.hidden = !commissioner || state !== 'PAUSED';
    if (cancel) cancel.hidden = !commissioner || !['NOT_SCHEDULED', 'SCHEDULED', 'LOBBY_OPEN'].includes(state);
    if (settings) settings.hidden = !commissioner || !preStart;
    if (saveSettings) saveSettings.disabled = !commissioner || !preStart;
    if (duration && String(duration.value) !== String(snapshot.pickClockSeconds || 90)) {
      duration.value = String(snapshot.pickClockSeconds || 90);
    }
    if (timezone && root.document.activeElement !== timezone) {
      timezone.value = String(snapshot.draftTimezone || root.Intl?.DateTimeFormat?.().resolvedOptions?.().timeZone || 'UTC');
    }

    const recap = root.document?.getElementById?.('draft-phase7-recap');
    const recapBody = root.document?.getElementById?.('draft-phase7-recap-body');
    if (recap) recap.hidden = state !== 'COMPLETED';
    if (recapBody && state === 'COMPLETED') {
      const summaries = Array.isArray(snapshot.recap?.managers) ? snapshot.recap.managers : [];
      recapBody.innerHTML = summaries.length
        ? summaries.map((entry) => `
          <div class="row">
            <strong>${root.escapeHtml?.(root.managerDisplayName?.(entry.managerEmail) || entry.managerEmail) || entry.managerEmail}</strong>
            <span class="badge">${Number(entry.totalSelections || 0)} picks</span>
          </div>
        `).join('')
        : `<div class="muted small">${Number(snapshot.recap?.totalSelections || 0)} selections finalized.</div>`;
    }
  }

  async function refresh() {
    const league = currentLeague();
    if (loading || !root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return currentSnapshot();
    loading = true;
    try {
      const incoming = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft`);
      const accepted = acceptSnapshot(incoming);
      if (accepted === incoming) render(incoming);
      return accepted;
    } catch (error) {
      const status = root.document?.getElementById?.('draft-phase7-status');
      if (status) status.textContent = root.mutationErrorMessage?.(error, 'Could not refresh authoritative draft state.') || error?.message || 'Draft refresh failed.';
      return currentSnapshot();
    } finally {
      loading = false;
    }
  }

  async function mutate(action, path, extra = {}) {
    const league = currentLeague();
    if (!league?.id || !isCommissioner()) return null;
    if (!currentSnapshot()) await refresh();
    const authoritative = currentSnapshot();
    const body = { expectedVersion: Math.max(0, snapshotVersion(authoritative)), ...extra };
    const fingerprint = JSON.stringify(body);
    const operation = operationFor(action, fingerprint);
    const status = root.document?.getElementById?.('draft-phase7-status');
    try {
      const snapshot = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}${path}`, {
        method: 'POST',
        headers: { 'Idempotency-Key': operation.operationKey },
        body: JSON.stringify(body)
      });
      clearOperation(action, operation.operationKey);
      let accepted = acceptSnapshot(snapshot);
      accepted = await syncLifecycleCache() || accepted;
      root.renderAll?.();
      render(accepted);
      return accepted;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, operation.operationKey);
      if (status) status.textContent = root.mutationErrorMessage?.(error, 'Draft control could not be applied.') || error?.data?.error || error?.message || 'Draft control failed.';
      if (Number(error?.status || 0) === 409) await refresh();
      return null;
    }
  }

  function startPolling() {
    installClockRenderer();
    if (pollTimer) root.clearInterval?.(pollTimer);
    refresh();
    pollTimer = root.setInterval?.(refresh, POLL_MS);
  }

  root.CFFDraftPhase7 = {
    lifecycleState,
    snapshotVersion,
    shouldReplaceSnapshot,
    currentSnapshot,
    clockLabel,
    refresh,
    mutate,
    render,
    operationFor,
    uncertainFailure
  };

  if (root.document) {
    root.addEventListener?.('online', refresh);
    root.document.addEventListener?.('visibilitychange', () => {
      if (!root.document.hidden) refresh();
    });
    if (root.document.readyState === 'loading') {
      root.document.addEventListener('DOMContentLoaded', startPolling, { once: true });
    } else {
      startPolling();
    }
  }
})(typeof window !== 'undefined' ? window : globalThis);
