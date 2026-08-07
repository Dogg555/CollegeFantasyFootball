(function initMultiPlayerTrades(root) {
  'use strict';

  const MAX_PLAYERS_PER_SIDE = 20;
  const OPERATION_STORAGE_KEY = 'cff_trade_lifecycle_operations';
  const MAX_OPERATION_AGE_MS = 15 * 60 * 1000;
  let installAttempts = 0;
  let installed = false;

  function playerId(player) {
    return String(player?.id || player?.playerId || '').trim();
  }

  function normalizePackage(value, fallback = null) {
    const source = Array.isArray(value) ? value : fallback ? [fallback] : [];
    const seen = new Set();
    return source.filter((player) => {
      const id = playerId(player);
      if (!id || seen.has(id)) return false;
      seen.add(id);
      return true;
    }).map((player) => ({ ...player, id: playerId(player), playerId: playerId(player) }));
  }

  function offerPlayers(offer) {
    return normalizePackage(offer?.offerPlayers, offer?.offerPlayer);
  }

  function requestPlayers(offer) {
    return normalizePackage(offer?.requestPlayers, offer?.requestPlayer);
  }

  function packageNames(players) {
    return normalizePackage(players).map((player) => player.name || playerId(player) || 'Player').join(', ');
  }

  function selectedIds(select) {
    if (!select) return [];
    return Array.from(select.selectedOptions || [])
      .map((option) => String(option.value || '').trim())
      .filter(Boolean);
  }

  function selectIds(select, ids) {
    const selected = new Set((ids || []).map(String));
    Array.from(select?.options || []).forEach((option) => {
      option.selected = selected.has(String(option.value || ''));
    });
  }

  function packageValid(offeredIds, requestedIds, maximum = MAX_PLAYERS_PER_SIDE) {
    if (!Array.isArray(offeredIds) || !Array.isArray(requestedIds)
      || offeredIds.length < 1 || requestedIds.length < 1
      || offeredIds.length > maximum || requestedIds.length > maximum) return false;
    const all = [...offeredIds, ...requestedIds].map((id) => String(id || '').trim());
    return all.every(Boolean) && new Set(all).size === all.length;
  }

  function counterPackageReady(lineupIsLocked, offeredIds, requestedIds) {
    return !lineupIsLocked && packageValid(offeredIds, requestedIds);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut || error?.unavailable || error?.retryable || !status || status >= 500);
  }

  function createOperationId() {
    if (root.crypto?.randomUUID) return root.crypto.randomUUID();
    return `trade-package-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
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
      // Session persistence is best-effort, matching the core trade lifecycle.
    }
  }

  function packageFingerprint(action, tradeId, targetManager, offeredIds, requestedIds, note = '') {
    return JSON.stringify({
      action: String(action || ''),
      tradeId: String(tradeId || ''),
      targetManager: String(targetManager || '').trim().toLowerCase(),
      offeredIds: [...(offeredIds || [])].map(String).sort(),
      requestedIds: [...(requestedIds || [])].map(String).sort(),
      note: String(note || '').trim()
    });
  }

  function operationFor(action, leagueId, fingerprint, storage = root.sessionStorage, createId = createOperationId) {
    const operations = readOperations(storage);
    const key = `${String(leagueId || '')}:${String(action || '')}`;
    const existing = operations[key];
    const age = Date.now() - Number(existing?.createdAt || 0);
    if (existing?.operationKey && existing.fingerprint === fingerprint && age >= 0 && age < MAX_OPERATION_AGE_MS) {
      return existing;
    }
    const operation = {
      action: String(action || ''),
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

  function errorMessage(error) {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      invalid_trade_players: 'Select at least one unique player on each side of the trade.',
      offered_player_not_owned: 'One of the offered players is no longer on your roster.',
      requested_player_not_owned: 'One of the requested players is no longer on that manager’s roster.',
      trade_player_locked: 'One of those players is already included in another open trade.',
      trade_roster_invalid: 'The package would leave at least one roster over capacity.',
      trade_roster_conflict: 'A roster changed while the package was being completed.',
      trade_ownership_changed: 'A player changed teams before the trade completed.',
      trade_counter_not_allowed: 'That offer can no longer be countered.',
      trade_state_conflict: 'Trade offers changed. The latest server state has been loaded.'
    };
    return messages[code]
      || root.CFFTradeLifecycle?.errorMessage?.(error)
      || root.mutationErrorMessage?.(error, 'The trade package could not be saved.')
      || error?.message
      || 'The trade package could not be saved.';
  }

  function enhanceSelect(select, labelText) {
    if (!select) return;
    select.multiple = true;
    select.size = Math.max(4, Math.min(8, select.options?.length || 4));
    select.setAttribute('aria-label', labelText);
    const label = select.closest?.('label');
    const title = label?.querySelector?.('span');
    if (title) title.textContent = labelText;
  }

  function ensurePlayerOptions(select, players) {
    if (!select) return;
    const existing = new Set(Array.from(select.options || [], (option) => String(option.value || '')));
    normalizePackage(players).forEach((player) => {
      const id = playerId(player);
      if (!id || existing.has(id)) return;
      const option = root.document?.createElement?.('option');
      if (!option) return;
      option.value = id;
      const position = String(player.position || '').trim();
      option.textContent = `${player.name || id}${position ? ` (${position})` : ''}`;
      option.dataset.counterSource = 'true';
      select.appendChild(option);
      existing.add(id);
    });
  }

  function setStatus(message, error = false) {
    const status = root.document?.getElementById('trade-status');
    if (!status) return;
    status.textContent = message || '';
    status.classList.toggle('error', Boolean(error));
  }

  function clearCounter() {
    const form = root.document?.getElementById('trade-form');
    if (!form) return;
    delete form.dataset.counterTradeId;
    delete form.dataset.counterTarget;
    const submit = form.querySelector('button[type="submit"]');
    if (submit) submit.textContent = 'Send offer';
    form.querySelector('[data-cancel-trade-counter]')?.remove();
    // Counter-only source options represent players locked in the source offer;
    // remove them when counter mode ends so they cannot leak into a new offer.
    const offerSelect = root.document?.getElementById('trade-offer-player');
    Array.from(offerSelect?.querySelectorAll?.('option[data-counter-source="true"]') || [])
      .forEach((option) => option.remove());
  }

  function ensureCounterCancelButton() {
    const form = root.document?.getElementById('trade-form');
    const footer = form?.querySelector('.form__footer');
    const submit = form?.querySelector('button[type="submit"]');
    if (!footer || !submit || footer.querySelector('[data-cancel-trade-counter]')) return;
    const cancel = root.document.createElement('button');
    cancel.type = 'button';
    cancel.className = 'button button--ghost';
    cancel.dataset.cancelTradeCounter = 'true';
    cancel.textContent = 'Cancel counter';
    cancel.addEventListener('click', () => {
      clearCounter();
      setStatus('Counteroffer cancelled.');
      root.renderLeague?.();
    });
    footer.insertBefore(cancel, submit);
  }

  function enhanceForm() {
    const offerSelect = root.document?.getElementById('trade-offer-player');
    const requestSelect = root.document?.getElementById('trade-request-player-id');
    enhanceSelect(offerSelect, 'Offer players (select one or more)');
    enhanceSelect(requestSelect, 'Request players (select one or more)');
    const legacyRequest = root.document?.getElementById('trade-request-player');
    const legacyLabel = legacyRequest?.closest?.('label');
    if (legacyLabel) legacyLabel.hidden = true;
    const form = root.document?.getElementById('trade-form');
    const submit = form?.querySelector('button[type="submit"]');
    if (submit && form?.dataset.counterTradeId) submit.textContent = 'Send counter';
  }

  function findOfferRow(index) {
    const list = root.document?.getElementById('trade-list');
    return list?.children?.[index] || null;
  }

  function enhanceTradeRows(leagueState) {
    const offers = root.getTradeOffers?.() || [];
    const authEmail = String(root.getAuthState?.()?.email || '').toLowerCase();
    offers.forEach((offer, index) => {
      const row = findOfferRow(index);
      if (!row) return;
      const offered = offerPlayers(offer);
      const requested = requestPlayers(offer);
      const title = row.querySelector('strong');
      if (title) title.textContent = `${packageNames(offered)} for ${packageNames(requested)}`;
      const actions = row.querySelector('.actions');
      const recipient = String(offer.offeredToEmail || offer.targetManager || '').toLowerCase() === authEmail;
      const pending = String(offer.status || '').toLowerCase() === 'pending';
      if (!actions || !recipient || !pending || actions.querySelector(`[data-trade-counter="${offer.id}"]`)) return;
      const counter = root.document.createElement('button');
      counter.type = 'button';
      counter.className = 'button button--ghost';
      counter.dataset.tradeCounter = offer.id;
      counter.textContent = 'Counter';
      counter.disabled = Boolean(root.lineupLocked?.());
      counter.addEventListener('click', () => prepareCounter(offer, leagueState));
      actions.insertBefore(counter, actions.firstChild);
    });
  }

  async function loadRequestedRoster(target, selected = []) {
    const select = root.document?.getElementById('trade-request-player-id');
    if (!select) return [];
    select.disabled = true;
    select.innerHTML = '<option value="">Loading roster...</option>';
    const roster = await root.getManagerRosterApi(target);
    select.innerHTML = roster.length
      ? roster.map((player) => `<option value="${root.escapeHtml?.(playerId(player)) || playerId(player)}">${root.escapeHtml?.(player.name || playerId(player)) || player.name || playerId(player)} (${root.escapeHtml?.(player.position || '') || player.position || ''})</option>`).join('')
      : '<option value="">No tradeable players</option>';
    select.disabled = !roster.length;
    enhanceSelect(select, 'Request players (select one or more)');
    selectIds(select, selected);
    return roster;
  }

  async function prepareCounter(offer) {
    const form = root.document?.getElementById('trade-form');
    const target = root.document?.getElementById('trade-target-manager');
    const offerSelect = root.document?.getElementById('trade-offer-player');
    const note = root.document?.getElementById('trade-note');
    const submit = form?.querySelector('button[type="submit"]');
    if (!form || !target || !offerSelect) return;
    const counterTarget = String(offer.offeredByEmail || '');
    const reversedOffered = requestPlayers(offer);
    target.value = counterTarget;
    form.dataset.counterTradeId = String(offer.id || '');
    form.dataset.counterTarget = counterTarget;
    enhanceForm();
    // Source players are intentionally locked by the pending trade and therefore
    // absent from the normal selectable roster. Put those owned players back into
    // this counter form so the default reversed package is actually selectable.
    ensurePlayerOptions(offerSelect, reversedOffered);
    selectIds(offerSelect, reversedOffered.map(playerId));
    const lineupIsLocked = Boolean(root.lineupLocked?.());
    offerSelect.disabled = lineupIsLocked || !reversedOffered.length;
    try {
      await loadRequestedRoster(counterTarget, offerPlayers(offer).map(playerId));
      const requestSelect = root.document?.getElementById('trade-request-player-id');
      const ready = counterPackageReady(
        lineupIsLocked,
        selectedIds(offerSelect),
        selectedIds(requestSelect)
      );
      if (submit) submit.disabled = !ready;
      if (note && !note.value.trim()) note.value = `Counteroffer to ${offer.id}`;
      ensureCounterCancelButton();
      setStatus(ready
        ? 'Counteroffer loaded. Adjust either package, then send the counter.'
        : 'The counteroffer cannot be submitted with the current trade package.', !ready);
      form.scrollIntoView?.({ behavior: 'smooth', block: 'start' });
    } catch (error) {
      clearCounter();
      setStatus(errorMessage(error), true);
    }
  }

  async function sendPackageTrade(event) {
    if (root.isLocalDemoSession?.() || !root.getAuthState?.()?.token) return;
    event.preventDefault();
    event.stopPropagation();
    event.stopImmediatePropagation();

    const form = event.currentTarget;
    const league = root.getLeagueState?.();
    const offerSelect = root.document?.getElementById('trade-offer-player');
    const requestSelect = root.document?.getElementById('trade-request-player-id');
    const targetSelect = root.document?.getElementById('trade-target-manager');
    const note = root.document?.getElementById('trade-note');
    const submit = form?.querySelector('button[type="submit"]');
    if (!league?.id || !form || !offerSelect || !requestSelect || !targetSelect || !submit) return;

    const offeredIds = selectedIds(offerSelect);
    const requestedIds = selectedIds(requestSelect);
    const targetManager = String(targetSelect.value || '').trim();
    if (!targetManager || !packageValid(offeredIds, requestedIds)) {
      setStatus('Select at least one unique player on each side of the trade.', true);
      return;
    }

    submit.disabled = true;
    setStatus('Saving trade package...');
    let action = form.dataset.counterTradeId ? 'counter' : 'create';
    let operation = null;
    try {
      try {
        await root.CFFTradeLifecycle?.sync?.();
      } catch {
        // The request still uses the last confirmed version.
      }
      const ownRoster = root.getRoster?.() || [];
      const targetRoster = await root.getManagerRosterApi(targetManager);
      const offeredPlayers = offeredIds.map((id) => ownRoster.find((player) => playerId(player) === id)).filter(Boolean);
      const requestedPlayers = requestedIds.map((id) => targetRoster.find((player) => playerId(player) === id)).filter(Boolean);
      if (offeredPlayers.length !== offeredIds.length || requestedPlayers.length !== requestedIds.length) {
        throw Object.assign(new Error('A selected roster changed.'), { code: 'trade_ownership_changed' });
      }

      action = form.dataset.counterTradeId ? 'counter' : 'create';
      const counterTradeId = action === 'counter' ? String(form.dataset.counterTradeId || '') : '';
      const noteValue = String(note?.value || '').trim();
      const fingerprint = packageFingerprint(
        action, counterTradeId, targetManager, offeredIds, requestedIds, noteValue
      );
      operation = operationFor(action, league.id, fingerprint);
      const body = {
        action,
        expectedVersion: Number(root.CFFTradeLifecycle?.currentVersion?.() ?? root.__cffTradeLifecycleVersion ?? 0),
        targetManager,
        offerPlayers: offeredPlayers,
        requestPlayers: requestedPlayers,
        note: noteValue
      };
      if (counterTradeId) body.tradeId = counterTradeId;
      const request = () => root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades/transactions`, {
        method: 'POST',
        headers: { 'Idempotency-Key': operation.operationKey },
        body: JSON.stringify(body),
        cffSkipMutationRefresh: true
      });

      let state;
      try {
        state = await request();
      } catch (firstError) {
        if (!uncertainFailure(firstError)) throw firstError;
        state = await request();
      }
      clearOperation(action, league.id, operation.operationKey);
      clearCounter();
      if (note) note.value = '';
      if (Array.isArray(state?.offers)) root.saveTradeOffers?.(state.offers);
      if (Array.isArray(state?.roster)) {
        root.setRoster?.(state.roster.map((player) => root.normalizePlayer?.(player) || player));
      }
      if (Number.isFinite(Number(state?.version))) root.__cffTradeLifecycleVersion = Number(state.version);
      try {
        await root.CFFTradeLifecycle?.sync?.();
      } catch {
        // The confirmed mutation response remains authoritative.
      }
      root.renderLeague?.();
      setStatus(action === 'counter' ? 'Counteroffer sent.' : 'Trade offer sent.');
    } catch (error) {
      // Keep the same persisted operation after an uncertain response so the next
      // submit replays the exact server operation instead of creating a duplicate.
      if (operation && !uncertainFailure(error)) {
        clearOperation(action, league.id, operation.operationKey);
      }
      if (String(error?.data?.code || error?.code || '') === 'trade_state_conflict') {
        try {
          await root.CFFTradeLifecycle?.sync?.();
          root.renderLeague?.();
        } catch {
          // Keep the last confirmed state.
        }
      }
      setStatus(errorMessage(error), true);
    } finally {
      submit.disabled = false;
    }
  }

  function packagePlayerLocked(playerIdValue, trades = root.getTradeOffers?.() || []) {
    const id = String(playerIdValue || '');
    return trades.some((trade) => {
      const open = ['pending', 'accepted'].includes(String(trade?.status || '').toLowerCase());
      if (!open) return false;
      return [...offerPlayers(trade), ...requestPlayers(trade)].some((player) => playerId(player) === id);
    });
  }

  function install() {
    installAttempts += 1;
    const form = root.document?.getElementById('trade-form');
    const required = ['renderTrades', 'apiRequest', 'getLeagueState', 'getAuthState', 'getRoster', 'getTradeOffers', 'getManagerRosterApi'];
    if (!form || !required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 800) root.setTimeout?.(install, 0);
      return;
    }
    if (installed) return;
    installed = true;

    const originalRenderTrades = root.renderTrades;
    root.renderTrades = function renderMultiPlayerTrades(leagueState) {
      const result = originalRenderTrades.call(this, leagueState);
      enhanceForm();
      enhanceTradeRows(leagueState);
      return result;
    };
    root.playerLockedInTrade = packagePlayerLocked;
    form.addEventListener('submit', sendPackageTrade, true);
    form.addEventListener('reset', clearCounter);
    root.document.getElementById('trade-target-manager')?.addEventListener('change', (event) => {
      if (form.dataset.counterTradeId && String(event.target.value || '') !== form.dataset.counterTarget) {
        clearCounter();
        setStatus('Counteroffer cleared because the target manager changed.');
      }
      root.setTimeout?.(enhanceForm, 0);
    });

    enhanceForm();
    enhanceTradeRows(root.getLeagueState?.());
    root.CFFMultiPlayerTrades = Object.freeze({
      installed: true,
      normalizePackage,
      offerPlayers,
      requestPlayers,
      packageValid,
      selectedIds,
      playerLockedInTrade: packagePlayerLocked
    });
    root.document.documentElement.setAttribute('data-cff-multi-player-trades', 'true');
  }

  const helpers = {
    MAX_PLAYERS_PER_SIDE,
    OPERATION_STORAGE_KEY,
    playerId,
    normalizePackage,
    offerPlayers,
    requestPlayers,
    packageNames,
    selectedIds,
    packageValid,
    counterPackageReady,
    uncertainFailure,
    packageFingerprint,
    operationFor,
    clearOperation
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
