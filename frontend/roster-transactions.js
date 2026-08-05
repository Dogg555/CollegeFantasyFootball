(function initRosterTransactions(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_roster_transaction_operations';
  const VERSION_STORAGE_KEY = 'cff_roster_transaction_versions';
  const REVISION_STORAGE_KEY = 'cff_roster_transaction_revision';
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
    if (!incoming || typeof incoming !== 'object' || !Array.isArray(incoming.roster)) return false;
    if (!current) return true;
    return stateVersion(incoming) >= stateVersion(current);
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    const stamp = typeof now === 'function' ? now() : now;
    const entropy = typeof random === 'function' ? random() : random;
    return `roster-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
  }

  function readJson(storage, key, fallback = {}) {
    try {
      return JSON.parse(storage?.getItem?.(key) || '') || fallback;
    } catch {
      return fallback;
    }
  }

  function writeJson(storage, key, value) {
    try {
      storage?.setItem?.(key, JSON.stringify(value));
    } catch {
      // Storage is an optimization; server state remains authoritative.
    }
  }

  function operationFor(action, leagueId, fingerprint = '', storage = root.sessionStorage, createId = createOperationId) {
    const operations = readJson(storage, OPERATION_STORAGE_KEY, {});
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
    writeJson(storage, OPERATION_STORAGE_KEY, operations);
    return operation;
  }

  function clearOperation(action, leagueId, operationKey = '', storage = root.sessionStorage) {
    const operations = readJson(storage, OPERATION_STORAGE_KEY, {});
    const key = `${String(leagueId || '')}:${String(action || '')}`;
    if (!operations[key]) return;
    if (operationKey && operations[key].operationKey !== operationKey) return;
    delete operations[key];
    writeJson(storage, OPERATION_STORAGE_KEY, operations);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut
      || error?.unavailable
      || error?.retryable
      || !status
      || status >= 500);
  }

  function rosterErrorMessage(error, fallback = 'The roster transaction could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      roster_state_conflict: 'Your roster changed before this action completed. The latest roster has been loaded.',
      roster_full: 'Your roster is full. Select a player to drop in the same transaction.',
      player_unavailable: 'Another manager added that player first.',
      player_already_rostered: 'That player is already on your roster.',
      waiver_claim_required: 'This league requires a waiver claim instead of a direct free-agent add.',
      drop_player_not_rostered: 'The selected drop player is no longer on your roster.',
      drop_player_conflict: 'The selected drop changed before the transaction completed.',
      invalid_roster_slot: 'That player is not eligible for the selected slot, or the slot is full.',
      lineup_locked: 'Lineup changes are locked after a matchup is finalized.',
      idempotency_key_conflict: 'This roster action conflicts with an earlier request. Reload and try again.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this action. Retry safely; the same roster operation will not run twice.';
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

  function storedVersion(leagueId, storage = root.localStorage) {
    const versions = readJson(storage, VERSION_STORAGE_KEY, {});
    return normalizeVersion(versions[String(leagueId || '')]);
  }

  function saveVersion(leagueId, version, storage = root.localStorage) {
    const versions = readJson(storage, VERSION_STORAGE_KEY, {});
    versions[String(leagueId || '')] = normalizeVersion(version);
    writeJson(storage, VERSION_STORAGE_KEY, versions);
  }

  function publishState(state, storage = root.localStorage) {
    const payload = {
      leagueId: String(state?.leagueId || currentLeague()?.id || ''),
      version: stateVersion(state),
      at: new Date().toISOString()
    };
    writeJson(storage, REVISION_STORAGE_KEY, payload);
    try {
      root.dispatchEvent?.(new CustomEvent('cff:roster-transaction', { detail: payload }));
    } catch {
      // CustomEvent is optional in focused runtime tests.
    }
    return payload;
  }

  function applyState(state) {
    if (!shouldApplyState(latestState, state)) return latestState;
    latestState = state;
    const leagueId = String(state.leagueId || currentLeague()?.id || '');
    root.setRoster?.((state.roster || []).map((player) => root.normalizePlayer?.(player) || player));
    saveVersion(leagueId, stateVersion(state));
    root.__cffRosterTransactionVersion = stateVersion(state);
    publishState(state);
    return state;
  }

  async function syncRosterState() {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return null;
    const state = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/roster/state`);
    return applyState(state);
  }

  function delay(milliseconds) {
    return new Promise((resolve) => (root.setTimeout || setTimeout)(resolve, milliseconds));
  }

  async function requestWithUncertainRetry(path, options) {
    try {
      return await root.apiRequest(path, options);
    } catch (error) {
      if (!uncertainFailure(error)) throw error;
      await delay(150);
      return root.apiRequest(path, options);
    }
  }

  async function mutate(action, payload, fingerprint) {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    if (!latestState || String(latestState.leagueId || '') !== String(league.id)) {
      await syncRosterState();
    }
    const operation = operationFor(action, league.id, fingerprint);
    const body = {
      action,
      expectedVersion: Math.max(stateVersion(), storedVersion(league.id)),
      ...payload
    };
    const path = `/leagues/${encodeURIComponent(league.id)}/roster/transactions`;
    const options = {
      method: 'POST',
      headers: { 'Idempotency-Key': operation.operationKey },
      body: JSON.stringify(body)
    };

    try {
      const state = await requestWithUncertainRetry(path, options);
      clearOperation(action, league.id, operation.operationKey);
      return applyState(state);
    } catch (error) {
      const code = String(error?.data?.code || error?.code || '');
      if (!uncertainFailure(error)) clearOperation(action, league.id, operation.operationKey);
      if (error?.data?.state) {
        applyState(error.data.state);
      } else if (error?.status === 409 || code.includes('roster') || code.includes('player_')) {
        try {
          await syncRosterState();
        } catch {
          // Preserve the last confirmed roster during a recovery outage.
        }
      }
      error.userMessage = rosterErrorMessage(error);
      throw error;
    }
  }

  function installAdapters() {
    installAttempts += 1;
    const required = ['apiRequest', 'addFreeAgentApi', 'dropPlayerApi', 'updateRosterSlotApi', 'setRoster'];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(installAdapters, 0);
      return;
    }
    if (root.addFreeAgentApi.__cffRosterTransactions) return;

    const originals = {
      addFreeAgentApi: root.addFreeAgentApi,
      dropPlayerApi: root.dropPlayerApi,
      updateRosterSlotApi: root.updateRosterSlotApi
    };

    root.addFreeAgentApi = async function resilientFreeAgentAdd(player, dropPlayerId = '') {
      if (root.isLocalDemoSession?.()) return originals.addFreeAgentApi.call(this, player);
      const normalized = root.normalizePlayer?.(player) || player;
      const dropId = String(dropPlayerId || '').trim();
      await mutate(
        dropId ? 'swap' : 'add',
        dropId ? { addPlayer: normalized, dropPlayerId: dropId } : { addPlayer: normalized },
        `${String(normalized?.id || '')}:${dropId}`
      );
      return true;
    };

    root.dropPlayerApi = async function resilientRosterDrop(playerId) {
      if (root.isLocalDemoSession?.()) return originals.dropPlayerApi.call(this, playerId);
      await mutate('drop', { playerId: String(playerId || '') }, String(playerId || ''));
      return true;
    };

    root.updateRosterSlotApi = async function resilientRosterSlot(playerId, slot) {
      if (root.isLocalDemoSession?.()) return originals.updateRosterSlotApi.call(this, playerId, slot);
      const normalizedSlot = String(slot || '').trim().toLowerCase();
      await mutate(
        'slot',
        { playerId: String(playerId || ''), slot: normalizedSlot },
        `${String(playerId || '')}:${normalizedSlot}`
      );
      return true;
    };

    root.rosterTransactionApi = async function rosterSwapApi(addPlayer, dropPlayerId = '') {
      return root.addFreeAgentApi(addPlayer, dropPlayerId);
    };
    root.syncRosterTransactionState = syncRosterState;

    [root.addFreeAgentApi, root.dropPlayerApi, root.updateRosterSlotApi]
      .forEach((fn) => { fn.__cffRosterTransactions = true; });

    root.addEventListener?.('storage', (event) => {
      if (event.key !== REVISION_STORAGE_KEY) return;
      const revision = readJson(root.localStorage, REVISION_STORAGE_KEY, {});
      const league = currentLeague();
      if (String(revision.leagueId || '') !== String(league?.id || '')) return;
      if (normalizeVersion(revision.version) > stateVersion()) {
        void syncRosterState().catch(() => {});
      }
    });
    root.addEventListener?.('online', () => { void syncRosterState().catch(() => {}); });
    root.document?.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') void syncRosterState().catch(() => {});
    });

    root.CFFRosterTransactions = Object.freeze({
      installed: true,
      sync: syncRosterState,
      latest: () => latestState,
      currentVersion: () => stateVersion(),
      mutate
    });
    root.document?.documentElement?.setAttribute?.('data-cff-roster-transactions', 'true');
  }

  const helpers = {
    normalizeVersion,
    stateVersion,
    shouldApplyState,
    createOperationId,
    operationFor,
    clearOperation,
    uncertainFailure,
    rosterErrorMessage,
    storedVersion,
    requestWithUncertainRetry
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  root.setTimeout?.(installAdapters, 0);
})(typeof window !== 'undefined' ? window : globalThis);
