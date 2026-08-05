(function initTradeLifecycle(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_trade_lifecycle_operations';
  const REVISION_STORAGE_KEY = 'cff_trade_lifecycle_revision';
  const MAX_OPERATION_AGE_MS = 15 * 60 * 1000;
  let latestState = null;
  let installAttempts = 0;

  function normalizeVersion(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  }

  function stateVersion(state = latestState) {
    return normalizeVersion(state?.version ?? 0);
  }

  function shouldApplyState(current, incoming) {
    if (!incoming || typeof incoming !== 'object') return false;
    if (!current) return true;
    return stateVersion(incoming) >= stateVersion(current);
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    const stamp = typeof now === 'function' ? now() : now;
    const entropy = typeof random === 'function' ? random() : random;
    return `trade-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
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
      // Session persistence is best-effort.
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
    return Boolean(error?.timedOut || error?.unavailable || error?.retryable || !status || status >= 500);
  }

  function tradeErrorMessage(error, fallback = 'The trade action could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      trade_state_conflict: 'Trade offers changed. The latest server state has been loaded.',
      trade_player_locked: 'One of those players is already included in another open trade.',
      offered_player_not_owned: 'The offered player is no longer on your roster.',
      requested_player_not_owned: 'The requested player is no longer on that manager’s roster.',
      trade_ownership_changed: 'A roster changed before the trade completed.',
      trade_roster_invalid: 'The trade would create an invalid roster configuration.',
      trade_roster_conflict: 'The roster exchange conflicted with another transaction.',
      trade_closed: 'This trade has already been resolved or expired.',
      trade_recipient_required: 'Only the receiving manager can accept or decline this offer.',
      trade_offerer_required: 'Only the offering manager can cancel this offer.',
      commissioner_required: 'Only the league commissioner can approve or veto this trade.',
      trade_not_awaiting_approval: 'This trade is not waiting for commissioner approval.',
      trades_locked: 'Trades are locked after a finalized matchup.',
      invalid_trade_target: 'Select a different active league manager.',
      invalid_trade_players: 'Both sides of the trade must include different players.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this trade action. Retry safely; the same operation will not run twice.';
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

  function publishState(state) {
    const payload = {
      leagueId: String(state?.leagueId || currentLeague()?.id || ''),
      version: stateVersion(state),
      at: new Date().toISOString()
    };
    try {
      root.localStorage?.setItem?.(REVISION_STORAGE_KEY, JSON.stringify(payload));
      root.dispatchEvent?.(new CustomEvent('cff:trade-lifecycle', { detail: payload }));
    } catch {
      // Storage and CustomEvent may be unavailable in focused tests.
    }
  }

  function applyState(state) {
    if (!shouldApplyState(latestState, state)) return latestState;
    latestState = state;
    root.__cffTradeLifecycleVersion = stateVersion(state);
    if (Array.isArray(state?.offers)) root.saveTradeOffers?.(state.offers);
    if (Array.isArray(state?.roster)) {
      root.setRoster?.(state.roster.map((player) => root.normalizePlayer?.(player) || player));
    }
    root.writeApiCacheMeta?.('league', state?.leagueId || currentLeague()?.id || '');
    publishState(state);
    return state;
  }

  async function syncState() {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return null;
    const state = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades/state`);
    applyState(state);
    return state;
  }

  async function requestMutation(action, details = {}, fingerprint = '') {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    if (!latestState || String(latestState.leagueId || '') !== String(league.id)) await syncState();
    const operation = operationFor(action, league.id, fingerprint);
    const request = () => root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades/transactions`, {
      method: 'POST',
      headers: { 'Idempotency-Key': operation.operationKey },
      body: JSON.stringify({ action, expectedVersion: stateVersion(), ...details }),
      cffSkipMutationRefresh: true
    });

    try {
      let state;
      try {
        state = await request();
      } catch (firstError) {
        if (!uncertainFailure(firstError)) throw firstError;
        state = await request();
      }
      clearOperation(action, league.id, operation.operationKey);
      applyState(state);
      try {
        await root.syncActiveLeagueCollectionsFromApi?.();
      } catch {
        // The confirmed trade state remains authoritative.
      }
      root.renderLeague?.();
      return state;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, operation.operationKey);
      const code = String(error?.data?.code || error?.code || '');
      if (error?.status === 409 || code.startsWith('trade_') || code === 'commissioner_required') {
        const conflictState = error?.data?.state;
        if (conflictState && typeof conflictState === 'object') applyState(conflictState);
        else {
          try {
            await syncState();
          } catch {
            // Preserve the last confirmed state if recovery fails.
          }
        }
      }
      error.userMessage = tradeErrorMessage(error);
      throw error;
    }
  }

  function install() {
    installAttempts += 1;
    const required = ['apiRequest', 'submitTradeOfferApi', 'updateTradeStatusApi'];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(install, 0);
      return;
    }
    if (root.submitTradeOfferApi.__cffTradeLifecycle) return;

    const originalSubmit = root.submitTradeOfferApi;
    const originalStatus = root.updateTradeStatusApi;

    root.submitTradeOfferApi = async function resilientTradeCreate(
      offerPlayerId,
      requestPlayerName,
      targetManager,
      requestPlayer = null,
      note = ''
    ) {
      if (root.isLocalDemoSession?.()) {
        return originalSubmit.call(this, offerPlayerId, requestPlayerName, targetManager, requestPlayer, note);
      }
      const offerPlayer = root.getRoster?.().find((item) => String(item?.id || '') === String(offerPlayerId || ''));
      const normalizedRequest = root.normalizePlayer?.(requestPlayer) || requestPlayer;
      const fingerprint = [offerPlayerId, normalizedRequest?.id, targetManager, note].map((value) => String(value || '')).join(':');
      return requestMutation('create', {
        offerPlayer: root.normalizePlayer?.(offerPlayer) || offerPlayer,
        requestPlayer: normalizedRequest,
        requestPlayerName,
        targetManager,
        note
      }, fingerprint);
    };

    root.updateTradeStatusApi = async function resilientTradeStatus(tradeId, status) {
      if (root.isLocalDemoSession?.()) return originalStatus.call(this, tradeId, status);
      const normalizedStatus = String(status || '').trim();
      return requestMutation('status', { tradeId, status: normalizedStatus },
        `${String(tradeId || '')}:${normalizedStatus.toLowerCase()}`);
    };

    root.submitTradeOfferApi.__cffTradeLifecycle = true;
    root.updateTradeStatusApi.__cffTradeLifecycle = true;

    root.addEventListener?.('online', () => {
      void syncState().then(() => root.renderLeague?.()).catch(() => {});
    });
    root.addEventListener?.('storage', (event) => {
      if (event.key !== REVISION_STORAGE_KEY) return;
      void syncState().then(() => root.renderLeague?.()).catch(() => {});
    });
    root.document?.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') {
        void syncState().then(() => root.renderLeague?.()).catch(() => {});
      }
    });

    root.CFFTradeLifecycle = Object.freeze({
      installed: true,
      sync: syncState,
      latest: () => latestState,
      currentVersion: () => stateVersion(),
      errorMessage: tradeErrorMessage
    });
    root.document?.documentElement?.setAttribute?.('data-cff-trade-lifecycle', 'true');
    void syncState().then(() => root.renderLeague?.()).catch(() => {});
  }

  const helpers = {
    OPERATION_STORAGE_KEY,
    REVISION_STORAGE_KEY,
    normalizeVersion,
    stateVersion,
    shouldApplyState,
    createOperationId,
    operationFor,
    clearOperation,
    uncertainFailure,
    tradeErrorMessage
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
