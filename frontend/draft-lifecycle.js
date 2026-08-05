(function initDraftLifecycle(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_draft_lifecycle_operations';
  const MAX_OPERATION_AGE_MS = 15 * 60 * 1000;
  let latestSnapshot = null;
  let installAttempts = 0;
  let installing = false;
  let renderAdapterAttempts = 0;

  function normalizeVersion(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  }

  function snapshotVersion(snapshot = latestSnapshot) {
    return normalizeVersion(snapshot?.version ?? snapshot?.revision ?? 0);
  }

  function shouldApplySnapshot(current, incoming) {
    if (!incoming || typeof incoming !== 'object') return false;
    if (!current) return true;
    const incomingVersion = snapshotVersion(incoming);
    const currentVersion = snapshotVersion(current);
    if (!incomingVersion && currentVersion) return false;
    return incomingVersion >= currentVersion;
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    const stamp = typeof now === 'function' ? now() : now;
    const entropy = typeof random === 'function' ? random() : random;
    return `draft-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
  }

  function readOperations(storage = root.sessionStorage) {
    try {
      return JSON.parse(storage?.getItem?.(OPERATION_STORAGE_KEY) || '{}') || {};
    } catch {
      return {};
    }
  }

  function writeOperations(operations, storage = root.sessionStorage) {
    try {
      storage?.setItem?.(OPERATION_STORAGE_KEY, JSON.stringify(operations));
    } catch {
      // Operation persistence is best-effort inside this tab.
    }
  }

  function operationFor(action, leagueId, fingerprint = '', storage = root.sessionStorage, createId = createOperationId) {
    const operations = readOperations(storage);
    const key = `${String(leagueId || '')}:${String(action || '')}`;
    const existing = operations[key];
    const age = Date.now() - Number(existing?.createdAt || 0);
    if (existing?.operationKey && existing.fingerprint === fingerprint && age >= 0 && age < MAX_OPERATION_AGE_MS) {
      return existing;
    }
    const operation = {
      action,
      leagueId: String(leagueId || ''),
      fingerprint,
      operationKey: createId(),
      createdAt: Date.now()
    };
    operations[key] = operation;
    writeOperations(operations, storage);
    return operation;
  }

  function clearOperation(action, leagueId, operationKey = '', storage = root.sessionStorage) {
    const operations = readOperations(storage);
    const key = `${String(leagueId || '')}:${String(action || '')}`;
    if (!operations[key]) return;
    if (operationKey && operations[key].operationKey !== operationKey) return;
    delete operations[key];
    writeOperations(operations, storage);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut
      || error?.unavailable
      || error?.retryable
      || !status
      || status >= 500);
  }

  function draftErrorMessage(error, fallback = 'The draft request could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      draft_managers_not_ready: 'Every active manager must mark ready before the commissioner starts the draft.',
      draft_state_conflict: 'The draft advanced before this action completed. The latest board has been loaded.',
      draft_not_your_turn: 'Another manager is currently on the clock.',
      player_already_drafted: 'That player was selected by another manager.',
      draft_pick_conflict: 'Another pick reached the server first. The latest board has been loaded.',
      draft_order_mismatch: 'Draft order must include every active manager exactly once.',
      draft_order_locked: 'Draft order is locked after the draft starts.',
      draft_lobby_closed: 'Open the draft lobby before starting.',
      draft_reset_required: 'Reset the completed draft before starting another test.',
      draft_no_pick_to_undo: 'There is no confirmed pick to undo.',
      draft_roster_full: 'This roster is already full.',
      commissioner_required: 'Only the league commissioner can perform this action.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this action. Retry safely; the same operation will not run twice.';
    }
    return error?.userMessage
      || root.mutationErrorMessage?.(error, fallback)
      || error?.data?.error
      || error?.message
      || fallback;
  }

  function currentLeague() {
    return root.getLeagueState?.() || null;
  }

  function currentEmail() {
    return String(root.getAuthState?.()?.email || '').trim().toLowerCase();
  }

  function currentMeta() {
    return root.getDraftMeta?.() || {};
  }

  function mutationContext(extra = {}) {
    const meta = currentMeta();
    return {
      expectedPick: Number(latestSnapshot?.currentPick ?? meta.currentPick ?? 1),
      expectedVersion: snapshotVersion(),
      ...extra
    };
  }

  function applySnapshot(snapshot) {
    if (!shouldApplySnapshot(latestSnapshot, snapshot)) return latestSnapshot;
    latestSnapshot = snapshot;
    root.__cffDraftLifecycleVersion = snapshotVersion(snapshot);
    root.applyDraftState?.(snapshot);
    renderReadiness(snapshot);
    return snapshot;
  }

  async function syncDraft() {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id) return null;
    const snapshot = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft`);
    return applySnapshot(snapshot);
  }

  async function mutate(action, path, body, fingerprint) {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    const operation = operationFor(action, league.id, fingerprint);
    try {
      const snapshot = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}${path}`, {
        method: path.endsWith('/order') ? 'PUT' : 'POST',
        headers: { 'Idempotency-Key': operation.operationKey },
        body: body === undefined ? undefined : JSON.stringify(body)
      });
      clearOperation(action, league.id, operation.operationKey);
      return applySnapshot(snapshot);
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, operation.operationKey);
      const code = String(error?.data?.code || error?.code || '');
      if (error?.status === 409 || code.startsWith('draft_') || code === 'player_already_drafted') {
        try {
          await syncDraft();
        } catch {
          // Preserve the last confirmed snapshot if recovery fetch fails.
        }
      }
      error.userMessage = draftErrorMessage(error);
      throw error;
    }
  }

  function safeEscape(value) {
    if (typeof root.escapeHtml === 'function') return root.escapeHtml(value);
    return String(value ?? '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#39;');
  }

  function installRenderAdapter() {
    renderAdapterAttempts += 1;
    const current = root.renderAll;
    if (typeof current !== 'function') {
      if (renderAdapterAttempts < 400) root.setTimeout?.(installRenderAdapter, 0);
      return;
    }
    if (current.__cffDraftLifecycleRender) return;
    const wrapped = function draftLifecycleRenderAll(...args) {
      const result = current.apply(this, args);
      renderReadiness();
      return result;
    };
    wrapped.__cffDraftLifecycleRender = true;
    wrapped.__cffOriginal = current;
    root.renderAll = wrapped;
    renderReadiness();
  }

  function ensureReadinessPanel() {
    if (root.document?.getElementById?.('draft-readiness-panel')) return;
    const lobbyMembers = root.document?.getElementById?.('draft-lobby-members');
    const row = lobbyMembers?.closest?.('.row');
    const card = row?.closest?.('.card');
    if (!card) return;
    const panel = root.document.createElement('div');
    panel.id = 'draft-readiness-panel';
    panel.className = 'list';
    panel.style.marginTop = '12px';
    panel.innerHTML = `
      <div class="row">
        <div>
          <strong>Manager readiness</strong>
          <div class="muted small" id="draft-readiness-summary">Waiting for manager status.</div>
          <div class="muted small" id="draft-auto-warning"></div>
        </div>
        <div class="actions">
          <button class="button" id="draft-auto-toggle" type="button">Enable auto-draft</button>
          <button class="button" id="draft-ready-toggle" type="button">Mark ready</button>
        </div>
      </div>
      <div id="draft-readiness-list" class="list"></div>
    `;
    card.appendChild(panel);
    root.document.getElementById('draft-ready-toggle')?.addEventListener('click', async () => {
      const league = currentLeague();
      if (!league?.id) return;
      const me = currentEmail();
      const current = (latestSnapshot?.readiness || []).find((entry) => String(entry.email || '').toLowerCase() === me);
      const button = root.document.getElementById('draft-ready-toggle');
      if (button) button.disabled = true;
      try {
        const snapshot = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/readiness`, {
          method: 'POST',
          body: JSON.stringify({ ready: !Boolean(current?.ready) })
        });
        applySnapshot(snapshot);
        root.renderAll?.();
      } catch (error) {
        const summary = root.document.getElementById('draft-readiness-summary');
        if (summary) summary.textContent = draftErrorMessage(error, 'Could not update readiness.');
      } finally {
        if (button) button.disabled = false;
      }
    });
    root.document.getElementById('draft-auto-toggle')?.addEventListener('click', async () => {
      const league = currentLeague();
      if (!league?.id) return;
      const me = currentEmail();
      const current = (latestSnapshot?.readiness || []).find((entry) => String(entry.email || '').toLowerCase() === me);
      const button = root.document.getElementById('draft-auto-toggle');
      if (button) button.disabled = true;
      try {
        const snapshot = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/auto-draft`, {
          method: 'POST',
          body: JSON.stringify({ enabled: !Boolean(current?.autoDraftEnabled) })
        });
        applySnapshot(snapshot);
        root.renderAll?.();
      } catch (error) {
        const warning = root.document.getElementById('draft-auto-warning');
        if (warning) warning.textContent = draftErrorMessage(error, 'Could not update auto-draft mode.');
      } finally {
        if (button) button.disabled = false;
      }
    });
  }

  function renderReadiness(snapshot = latestSnapshot) {
    ensureReadinessPanel();
    const readiness = Array.isArray(snapshot?.readiness) ? snapshot.readiness : [];
    const summary = root.document?.getElementById?.('draft-readiness-summary');
    const list = root.document?.getElementById?.('draft-readiness-list');
    const toggle = root.document?.getElementById?.('draft-ready-toggle');
    const autoToggle = root.document?.getElementById?.('draft-auto-toggle');
    const autoWarning = root.document?.getElementById?.('draft-auto-warning');
    const active = Number(snapshot?.activeManagerCount || readiness.length || 0);
    const ready = Number(snapshot?.readyCount ?? readiness.filter((entry) => entry.ready).length);
    const connected = Number(snapshot?.connectedCount ?? readiness.filter((entry) => entry.connected).length);
    if (summary) summary.textContent = `${ready} of ${active} ready · ${connected} connected`;
    if (list) {
      list.innerHTML = readiness.length
        ? readiness.map((entry) => `
          <div class="row">
            <div>
              <strong>${safeEscape(entry.teamName || entry.email)}</strong>
              <div class="muted small">${safeEscape(entry.email)}${entry.role === 'commissioner' ? ' · Commissioner' : ''}</div>
            </div>
            <div class="actions">
              <span class="pill ${entry.connected ? '' : 'pill--muted'}">${entry.connected ? 'Connected' : 'Reconnecting'}</span>
              <span class="pill ${entry.ready ? '' : 'pill--muted'}">${entry.ready ? 'Ready' : 'Not ready'}</span>
              <span class="pill ${entry.autoDraftEnabled ? '' : 'pill--muted'}">${entry.autoDraftEnabled ? 'Auto-draft' : `${Number(entry.consecutiveMissedPicks || 0)} missed`}</span>
            </div>
          </div>
        `).join('')
        : '<div class="muted small">No active managers are available.</div>';
    }
    if (toggle) {
      const mine = readiness.find((entry) => String(entry.email || '').toLowerCase() === currentEmail());
      toggle.textContent = mine?.ready ? 'Mark not ready' : 'Mark ready';
      toggle.hidden = snapshot?.status !== 'not_started';
    }
    if (autoToggle || autoWarning) {
      const mine = readiness.find((entry) => String(entry.email || '').toLowerCase() === currentEmail());
      if (autoToggle) {
        autoToggle.textContent = mine?.autoDraftEnabled ? 'Disable auto-draft' : 'Enable auto-draft';
        autoToggle.hidden = snapshot?.status === 'complete';
      }
      if (autoWarning) {
        autoWarning.textContent = mine?.autoDraftEnabled
          ? 'Auto-draft mode is active for your team. Future picks may be made automatically until you disable it.'
          : '';
      }
    }
    const start = root.document?.getElementById?.('draft-start');
    if (start && !start.hidden && snapshot?.status === 'not_started') {
      start.disabled = active < 2 || snapshot?.allReady !== true;
      start.title = start.disabled ? 'Every active manager must be ready.' : '';
    }
  }

  function installAdapters() {
    if (installing) return;
    installAttempts += 1;
    const required = ['apiRequest', 'syncDraftFromApi', 'draftPlayerApi', 'startDraftApi', 'saveDraftOrderApi', 'resetDraftApi', 'undoLastDraftPickApi', 'applyDraftState'];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(installAdapters, 0);
      return;
    }
    if (root.draftPlayerApi.__cffDraftLifecycle) return;
    installing = true;

    const originals = {
      draftPlayerApi: root.draftPlayerApi,
      startDraftApi: root.startDraftApi,
      saveDraftOrderApi: root.saveDraftOrderApi,
      resetDraftApi: root.resetDraftApi,
      undoLastDraftPickApi: root.undoLastDraftPickApi
    };

    root.syncDraftFromApi = async function resilientDraftSync() {
      return syncDraft();
    };

    root.draftPlayerApi = async function resilientDraftPick(player) {
      if (root.isLocalDemoSession?.()) return originals.draftPlayerApi.call(this, player);
      const normalized = root.normalizePlayer?.(player) || player;
      return mutate(
        'pick',
        '/draft/picks',
        mutationContext({ player: normalized }),
        String(normalized?.id || '')
      );
    };

    root.startDraftApi = async function resilientDraftStart() {
      if (root.isLocalDemoSession?.()) return originals.startDraftApi.call(this);
      return mutate('start', '/draft/start', mutationContext({ force: false }), 'start');
    };

    root.saveDraftOrderApi = async function resilientDraftOrder(draftOrder = []) {
      if (root.isLocalDemoSession?.()) return originals.saveDraftOrderApi.call(this, draftOrder);
      return mutate('order', '/draft/order', mutationContext({ draftOrder }), JSON.stringify(draftOrder));
    };

    root.resetDraftApi = async function resilientDraftReset() {
      if (root.isLocalDemoSession?.()) return originals.resetDraftApi.call(this);
      return mutate('reset', '/draft/reset', mutationContext(), 'reset');
    };

    root.undoLastDraftPickApi = async function resilientDraftUndo() {
      if (root.isLocalDemoSession?.()) return originals.undoLastDraftPickApi.call(this);
      const picks = Array.isArray(latestSnapshot?.picks) ? latestSnapshot.picks : root.getDraftPicks?.() || [];
      const lastPick = picks[picks.length - 1];
      return mutate('undo', '/draft/undo', mutationContext(), String(lastPick?.pickNumber || picks.length || 0));
    };

    [root.draftPlayerApi, root.startDraftApi, root.saveDraftOrderApi, root.resetDraftApi, root.undoLastDraftPickApi]
      .forEach((fn) => { fn.__cffDraftLifecycle = true; });

    root.setTimeout?.(installRenderAdapter, 0);
    ensureReadinessPanel();
    root.addEventListener?.('online', () => { void syncDraft().then(() => root.renderAll?.()).catch(() => {}); });
    root.document?.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') {
        void syncDraft().then(() => root.renderAll?.()).catch(() => {});
      }
    });

    root.CFFDraftLifecycle = Object.freeze({
      installed: true,
      sync: syncDraft,
      latest: () => latestSnapshot,
      currentVersion: () => snapshotVersion(),
      renderReadiness
    });
    root.document?.documentElement?.setAttribute?.('data-cff-draft-lifecycle', 'true');
    installing = false;
  }

  const helpers = {
    normalizeVersion,
    snapshotVersion,
    shouldApplySnapshot,
    createOperationId,
    operationFor,
    clearOperation,
    uncertainFailure,
    draftErrorMessage
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  root.setTimeout?.(installAdapters, 0);
})(typeof window !== 'undefined' ? window : globalThis);
