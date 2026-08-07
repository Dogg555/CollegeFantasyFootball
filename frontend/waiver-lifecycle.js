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
      waiver_deadline_passed: 'This waiver period is closed for new, cancelled, or reordered claims.',
      waiver_claim_limit: 'Too many waiver claims are pending for this manager.',
      waiver_reorder_conflict: 'Claim order changed. Reorder the complete latest pending list.',
      player_unavailable: 'That player was acquired by another manager.',
      player_inactive: 'That player is no longer active in the authoritative player pool.',
      player_ineligible: 'That player is not eligible for this league roster.',
      player_locked: 'That player cannot be awarded because the active-week game has started.',
      drop_player_not_rostered: 'The selected drop player is no longer on your roster.',
      drop_player_locked: 'The selected drop player is locked and can no longer be removed.',
      roster_full: 'Your roster is full. Include a valid player to drop.',
      commissioner_required: 'Only the league commissioner can process waivers.'
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

  function claimFailureMessage(claim, fallback = 'This waiver claim was not awarded.') {
    if (claim?.failureReason) return String(claim.failureReason);
    if (claim?.failureCode) return waiverErrorMessage({ code: claim.failureCode }, fallback);
    return fallback;
  }

  function claimShowsFailureDetails(claim) {
    const status = String(claim?.status || '');
    return (status === 'Failed' || status === 'Expired')
      && Boolean(claim?.failureReason || claim?.failureCode);
  }

  function waiverPanelModel(state = latestState) {
    const claims = Array.isArray(state?.claims) ? state.claims : [];
    const claimsMutable = state?.claimsMutable !== false;
    const periodProcessed = state?.periodProcessed === true;
    return {
      claims,
      claimsMutable,
      periodProcessed,
      canProcess: state?.canProcess === true && !periodProcessed,
      processingPeriod: String(state?.processingPeriod || ''),
      claimDeadline: String(state?.waiverRules?.claimDeadline || ''),
      pendingCount: Number(state?.pendingCount || claims.filter((claim) => claim?.status === 'Pending').length || 0)
    };
  }

  function orderedClaimsForPanel(claims = []) {
    return claims.slice().sort((a, b) => (
      Number(a?.priority || 999) - Number(b?.priority || 999)
      || Number(a?.claimOrder || 999) - Number(b?.claimOrder || 999)
      || new Date(a?.createdAt || a?.submittedAt || 0).getTime() - new Date(b?.createdAt || b?.submittedAt || 0).getTime()
      || String(a?.id || '').localeCompare(String(b?.id || ''))
    ));
  }

  function applyWaiverPanelState(state = latestState) {
    const documentObject = root.document;
    if (!documentObject || !state || typeof state !== 'object') return waiverPanelModel(state);
    const model = waiverPanelModel(state);
    const addSelect = documentObject.getElementById?.('waiver-add-player');
    const dropSelect = documentObject.getElementById?.('waiver-drop-player');
    const form = documentObject.getElementById?.('waiver-form');
    const submitButton = form?.querySelector?.('button[type="submit"]');
    const status = documentObject.getElementById?.('waiver-status');
    const list = documentObject.getElementById?.('waiver-list');
    const hasAddOptions = Number(addSelect?.options?.length || 0) > 0;

    if (addSelect) addSelect.disabled = !model.claimsMutable || !hasAddOptions;
    if (dropSelect) dropSelect.disabled = !model.claimsMutable;
    if (submitButton) submitButton.disabled = !model.claimsMutable || !hasAddOptions;

    if (list) {
      list.querySelectorAll?.('[data-cancel-waiver]').forEach((button) => {
        button.disabled = !model.claimsMutable;
      });
      if (!model.claimsMutable) {
        list.querySelectorAll?.('[data-waiver-up], [data-waiver-down]').forEach((button) => {
          button.disabled = true;
        });
      }
      list.querySelectorAll?.('[data-process-all-waivers], [data-process-waiver]').forEach((button) => {
        button.disabled = !model.canProcess;
      });

      const processButton = list.querySelector?.('[data-process-all-waivers]');
      const processRow = processButton?.closest?.('.row');
      const processCopy = processRow?.querySelector?.('.muted');
      if (processCopy) {
        if (model.periodProcessed) {
          processCopy.textContent = 'This waiver processing period has already been completed.';
        } else if (model.claimsMutable) {
          const deadline = model.claimDeadline ? new Date(model.claimDeadline) : null;
          processCopy.textContent = deadline && !Number.isNaN(deadline.getTime())
            ? `Claims can be changed until ${deadline.toLocaleString()}.`
            : 'Claims remain editable until the configured waiver deadline.';
        } else {
          processCopy.textContent = 'Ready to process by priority, claim order, then submitted time.';
        }
      }

      const claimRows = Array.from(list.querySelectorAll?.('.row') || [])
        .filter((row) => !row.querySelector?.('[data-process-all-waivers]'));
      const orderedClaims = orderedClaimsForPanel(model.claims);
      claimRows.forEach((row, index) => {
        const claim = orderedClaims[index];
        if (!claim) return;
        const badge = row.querySelector?.('.badge');
        if (badge && claim.status) badge.textContent = String(claim.status);
        row.querySelector?.('[data-waiver-failure]')?.remove?.();
        if (claimShowsFailureDetails(claim)) {
          const details = row.querySelector?.('div');
          if (details && typeof documentObject.createElement === 'function') {
            const reason = documentObject.createElement('div');
            reason.className = 'muted small';
            reason.dataset.waiverFailure = 'true';
            reason.setAttribute?.('data-waiver-failure', 'true');
            reason.textContent = claimFailureMessage(claim);
            details.appendChild?.(reason);
          }
        }
      });
    }

    if (status && /waivers are locked after finalized matchups/i.test(String(status.textContent || ''))) {
      if (model.periodProcessed) {
        status.textContent = 'This waiver processing period is complete.';
      } else if (!model.claimsMutable && model.pendingCount > 0) {
        status.textContent = 'Claim window closed. Pending claims are awaiting processing.';
      } else {
        status.textContent = '';
      }
    }
    return model;
  }

  function queueWaiverPanelRefresh() {
    if (typeof root.setTimeout === 'function') root.setTimeout(() => applyWaiverPanelState(), 0);
    else applyWaiverPanelState();
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
    queueWaiverPanelRefresh();
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
      applyWaiverPanelState();
      return state;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, operation.operationKey);
      const code = String(error?.data?.code || error?.code || '');
      if (error?.status === 409 || code.startsWith('waiver_') || code.startsWith('player_') || code.startsWith('drop_player_')) {
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

  async function submitAuthoritativeWaiverForm(event, stateOverride = null) {
    if (root.isLocalDemoSession?.() || !root.getAuthState?.()?.token) return false;
    event?.preventDefault?.();
    event?.stopImmediatePropagation?.();

    const documentObject = root.document;
    const status = documentObject?.getElementById?.('waiver-status');
    let state = stateOverride;
    const league = currentLeague();
    if (!state || String(state?.leagueId || '') !== String(league?.id || '')) {
      try {
        state = await syncState();
      } catch (error) {
        if (status) status.textContent = waiverErrorMessage(error, 'Could not load the current waiver state.');
        return true;
      }
    }
    if (!state) {
      if (status) status.textContent = 'Could not load the current waiver state.';
      return true;
    }

    const model = waiverPanelModel(state);
    if (!model.claimsMutable) {
      if (status) status.textContent = 'This waiver period is closed for new claims.';
      applyWaiverPanelState(state);
      return true;
    }

    const addSelect = documentObject?.getElementById?.('waiver-add-player');
    const dropSelect = documentObject?.getElementById?.('waiver-drop-player');
    const playerId = String(addSelect?.value || '');
    if (!playerId) {
      if (status) status.textContent = 'No player selected.';
      return true;
    }

    try {
      await root.submitWaiverClaimApi?.({ id: playerId, playerId }, String(dropSelect?.value || ''));
      root.renderLeague?.();
      applyWaiverPanelState();
      if (status) status.textContent = 'Waiver claim submitted.';
    } catch (error) {
      if (status) status.textContent = waiverErrorMessage(error, 'Could not submit waiver claim. No local changes were made.');
    }
    return true;
  }

  function installWaiverSubmitOverlay() {
    const form = root.document?.getElementById?.('waiver-form');
    if (!form || form.__cffWaiverPhase5Submit) return;
    form.__cffWaiverPhase5Submit = true;
    form.addEventListener?.('submit', (event) => {
      if (root.isLocalDemoSession?.() || !root.getAuthState?.()?.token) return;
      void submitAuthoritativeWaiverForm(event);
    }, true);
  }

  function installRenderOverlay() {
    const originalRenderLeague = root.renderLeague;
    if (typeof originalRenderLeague !== 'function' || originalRenderLeague.__cffWaiverPhase5) return;
    const wrappedRenderLeague = function phase5WaiverAwareRender(...args) {
      const result = originalRenderLeague.apply(this, args);
      applyWaiverPanelState();
      return result;
    };
    wrappedRenderLeague.__cffWaiverPhase5 = true;
    root.renderLeague = wrappedRenderLeague;
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
      const playerId = String(player?.id || player?.playerId || '');
      return requestMutation('create', { addPlayerId: playerId, dropPlayerId },
        `${playerId}:${String(dropPlayerId || '')}`);
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
      return requestMutation('process', {}, `process:${String(latestState?.processingPeriod || '')}:${stateVersion()}`);
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

    installWaiverSubmitOverlay();
    installRenderOverlay();
    root.addEventListener?.('online', () => {
      void syncState().then(() => { root.renderLeague?.(); applyWaiverPanelState(); }).catch(() => {});
    });
    root.addEventListener?.('storage', (event) => {
      if (event.key !== REVISION_STORAGE_KEY) return;
      void syncState().then(() => { root.renderLeague?.(); applyWaiverPanelState(); }).catch(() => {});
    });
    root.document?.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') {
        void syncState().then(() => { root.renderLeague?.(); applyWaiverPanelState(); }).catch(() => {});
      }
    });

    root.CFFWaiverLifecycle = Object.freeze({
      installed: true,
      sync: syncState,
      latest: () => latestState,
      currentVersion: () => stateVersion(),
      errorMessage: waiverErrorMessage,
      claimFailureMessage,
      panelModel: waiverPanelModel,
      applyPanelState: applyWaiverPanelState,
      submitForm: submitAuthoritativeWaiverForm
    });
    root.document?.documentElement?.setAttribute?.('data-cff-waiver-lifecycle', 'true');
    void syncState().then(() => { root.renderLeague?.(); applyWaiverPanelState(); }).catch(() => {});
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
    waiverErrorMessage,
    claimFailureMessage,
    claimShowsFailureDetails,
    waiverPanelModel,
    orderedClaimsForPanel,
    applyWaiverPanelState,
    submitAuthoritativeWaiverForm
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
