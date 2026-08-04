(function initWaiverLifecycle(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_waiver_lifecycle_operations';
  const REVISION_STORAGE_KEY = 'cff_waiver_lifecycle_revision';
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
    return `waiver-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
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

  function waiverErrorMessage(error, fallback = 'The waiver action could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      waiver_state_conflict: 'Waiver priority or claims changed. The latest server state has been loaded.',
      duplicate_waiver_claim: 'You already have a pending claim for that player.',
      waiver_claim_out_of_order: 'Claims must be processed in the current priority order.',
      waiver_claim_required: 'This player must be acquired through waivers.',
      waivers_not_active: 'This league currently uses open free agency.',
      waiver_deadline_pending: 'The waiver deadline has not passed yet.',
      waiver_claim_limit: 'Too many waiver claims are pending for this manager.',
      waiver_reorder_conflict: 'Claim order changed. Reorder the complete latest pending list.',
      player_unavailable: 'That player was acquired by another manager.',
      drop_player_not_rostered: 'The selected drop player is no longer on your roster.',
      roster_full: 'Your roster is full. Include a valid player to drop.',
      commissioner_required: 'Only the league commissioner can process waivers.',
      waivers_locked: 'Waivers are locked after a finalized matchup.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this waiver action. Retry safely; the same operation will not run twice.';
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
      root.dispatchEvent?.(new CustomEvent('cff:waiver-lifecycle', { detail: payload }));
    } catch {
      // Storage and CustomEvent may be unavailable in focused tests.
    }
  }

  function applyState(state) {
    if (!shouldApplyState(latestState, state)) return latestState;
    latestState = state;
    root.__cffWaiverLifecycleVersion = stateVersion(state);
    if (Array.isArray(state?.claims)) root.saveWaiverClaims?.(state.claims);
    if (Array.isArray(state?.priority)) root.saveWaiverPriority?.(state.priority);
    if (Array.isArray(state?.roster)) root.setRoster?.(state.roster.map((player) => root.normalizePlayer?.(player) || player));
    root.writeApiCacheMeta?.('league', state?.leagueId || currentLeague()?.id || '');
    publishState(state);
    return state;
  }

  async function syncState() {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return null;
    const state = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/state`);
    applyState(state);
    return state;
  }

  async function requestMutation(action, details = {}, fingerprint = '') {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    if (!latestState || String(latestState.leagueId || '') !== String(league.id)) await syncState();
    const operation = operationFor(action, league.id, fingerprint);
    const request = () => root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/transactions`, {
      method: 'POST',
      headers: { 'Idempotency-Key': operation.operationKey },
      body: JSON.stringify({
        action,
        expectedVersion: stateVersion(),
        ...details
      }),
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
        // The confirmed state remains authoritative if a broader refresh fails.
      }
      root.renderLeague?.();
      return state;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, operation.operationKey);
      const code = String(error?.data?.code || error?.code || '');
      if (error?.status === 409 || code.startsWith('waiver_') || code === 'player_unavailable') {
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
      error.userMessage = waiverErrorMessage(error);
      throw error;
    }
  }

  function install() {
    installAttempts += 1;
    const required = [
      'apiRequest',
      'submitWaiverClaimApi',
      'processWaiverClaimApi',
      'cancelWaiverClaimApi',
      'reorderWaiverClaimsApi',
      'processWaiversApi',
      'resetWaiverPriorityApi'
    ];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(install, 0);
      return;
    }
    if (root.submitWaiverClaimApi.__cffWaiverLifecycle) return;

    const originals = Object.fromEntries(required.slice(1).map((name) => [name, root[name]]));

    root.submitWaiverClaimApi = async function resilientWaiverCreate(addPlayer, dropPlayerId = '') {
      if (root.isLocalDemoSession?.()) return originals.submitWaiverClaimApi.call(this, addPlayer, dropPlayerId);
      const player = root.normalizePlayer?.(addPlayer) || addPlayer;
      return requestMutation('create', { addPlayer: player, dropPlayerId },
        `${String(player?.id || '')}:${String(dropPlayerId || '')}`);
    };

    root.cancelWaiverClaimApi = async function resilientWaiverCancel(claimId) {
      if (root.isLocalDemoSession?.()) return originals.cancelWaiverClaimApi.call(this, claimId);
      return requestMutation('cancel', { claimId }, String(claimId || ''));
    };

    root.reorderWaiverClaimsApi = async function resilientWaiverReorder(claimIds = []) {
      if (root.isLocalDemoSession?.()) return originals.reorderWaiverClaimsApi.call(this, claimIds);
      return requestMutation('reorder', { claimIds }, JSON.stringify(claimIds));
    };

    root.processWaiverClaimApi = async function resilientWaiverProcessOne(claimId) {
      if (root.isLocalDemoSession?.()) return originals.processWaiverClaimApi.call(this, claimId);
      return requestMutation('process_one', { claimId }, String(claimId || ''));
    };

    root.processWaiversApi = async function resilientWaiverProcessAll() {
      if (root.isLocalDemoSession?.()) return originals.processWaiversApi.call(this);
      return requestMutation('process', {}, `process:${stateVersion()}`);
    };

    root.resetWaiverPriorityApi = async function resilientPriorityReset() {
      if (root.isLocalDemoSession?.()) return originals.resetWaiverPriorityApi.call(this);
      return requestMutation('reset_priority', {}, `reset:${stateVersion()}`);
    };

    [
      root.submitWaiverClaimApi,
      root.cancelWaiverClaimApi,
      root.reorderWaiverClaimsApi,
      root.processWaiverClaimApi,
      root.processWaiversApi,
      root.resetWaiverPriorityApi
    ].forEach((fn) => { fn.__cffWaiverLifecycle = true; });

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

    root.CFFWaiverLifecycle = Object.freeze({
      installed: true,
      sync: syncState,
      latest: () => latestState,
      currentVersion: () => stateVersion(),
      errorMessage: waiverErrorMessage
    });
    root.document?.documentElement?.setAttribute?.('data-cff-waiver-lifecycle', 'true');
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
    waiverErrorMessage
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
