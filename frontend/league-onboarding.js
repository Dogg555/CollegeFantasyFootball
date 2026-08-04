(function initLeagueOnboarding(root) {
  'use strict';

  const ALLOWED_TEAM_COUNTS = Object.freeze([4, 6, 8, 10, 12, 14, 16]);
  const CREATE_OPERATION_KEY = 'cff_league_create_operation';
  const JOIN_OPERATION_KEY = 'cff_league_join_operations';
  const MAX_LEAGUE_NAME_LENGTH = 80;
  const MAX_EMAIL_LENGTH = 254;
  const DEFAULT_ROSTER_RULES = Object.freeze({ qb: 1, rb: 2, wr: 2, te: 1, flex: 2, bench: 6 });

  function canonicalEmail(value = '') {
    return String(value || '').trim().toLowerCase();
  }

  function validEmail(value = '') {
    const email = canonicalEmail(value);
    const at = email.indexOf('@');
    return Boolean(email
      && email.length <= MAX_EMAIL_LENGTH
      && at > 0
      && at < email.length - 1
      && !/\s/.test(email));
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    return `league-${Math.max(0, Number(now()) || 0).toString(36)}-${Math.floor((Number(random()) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
  }

  function normalizeInviteList(values = [], ownerEmail = '') {
    const owner = canonicalEmail(ownerEmail);
    const seen = new Set();
    const invites = [];
    const invalid = [];
    (Array.isArray(values) ? values : []).forEach((value) => {
      const email = canonicalEmail(value);
      if (!email || email === owner || seen.has(email)) return;
      if (!validEmail(email)) {
        invalid.push(String(value || '').trim());
        return;
      }
      seen.add(email);
      invites.push(email);
    });
    return { invites, invalid };
  }

  function parseInviteText(value = '') {
    return String(value || '')
      .split(/[,\n]/)
      .map((item) => item.trim())
      .filter(Boolean);
  }

  function validateCreatePayload(payload = {}, ownerEmail = '', now = Date.now()) {
    const errors = [];
    const name = String(payload.name || '').trim();
    const teams = Number(payload.teams);
    const normalized = normalizeInviteList(payload.invitedEmails || [], ownerEmail);

    if (!name || name.length > MAX_LEAGUE_NAME_LENGTH) {
      errors.push('League name must contain 1 to 80 characters.');
    }
    if (!ALLOWED_TEAM_COUNTS.includes(teams)) {
      errors.push('League size must be 4, 6, 8, 10, 12, 14, or 16 teams.');
    }
    if (normalized.invalid.length) {
      errors.push(`Fix invalid manager email${normalized.invalid.length === 1 ? '' : 's'}: ${normalized.invalid.join(', ')}.`);
    }
    if (ALLOWED_TEAM_COUNTS.includes(teams) && normalized.invites.length > teams - 1) {
      errors.push(`A ${teams}-team league can invite at most ${teams - 1} other managers.`);
    }
    if (payload.draftDate) {
      const draftAt = new Date(payload.draftDate).getTime();
      if (!Number.isFinite(draftAt) || draftAt <= Number(now())) {
        errors.push('Choose a draft date in the future.');
      }
    }

    return {
      ok: errors.length === 0,
      errors,
      payload: {
        ...payload,
        name,
        teams,
        invitedEmails: normalized.invites
      }
    };
  }

  function stableCreateFingerprint(payload = {}, ownerEmail = '') {
    return JSON.stringify({
      ownerEmail: canonicalEmail(ownerEmail),
      name: String(payload.name || '').trim(),
      teams: Number(payload.teams || 0),
      scoring: String(payload.scoring || ''),
      draftType: String(payload.draftType || ''),
      draftDate: String(payload.draftDate || ''),
      notes: String(payload.notes || ''),
      invitedEmails: [...(payload.invitedEmails || [])].map(canonicalEmail).sort()
    });
  }

  function readJson(storage, key, fallback = null) {
    try {
      const raw = storage?.getItem?.(key);
      return raw ? JSON.parse(raw) : fallback;
    } catch {
      return fallback;
    }
  }

  function writeJson(storage, key, value) {
    try {
      storage?.setItem?.(key, JSON.stringify(value));
    } catch {
      // Session persistence is best-effort; the request remains usable in this tab.
    }
  }

  function pendingCreate(storage, payload, ownerEmail, createId = createOperationId) {
    const fingerprint = stableCreateFingerprint(payload, ownerEmail);
    const existing = readJson(storage, CREATE_OPERATION_KEY, null);
    if (existing?.fingerprint === fingerprint && existing?.operationKey) return existing;
    const operation = {
      operationKey: createId(),
      fingerprint,
      ownerEmail: canonicalEmail(ownerEmail),
      payload,
      createdAt: new Date().toISOString()
    };
    writeJson(storage, CREATE_OPERATION_KEY, operation);
    return operation;
  }

  function clearPendingCreate(storage, operationKey = '') {
    const current = readJson(storage, CREATE_OPERATION_KEY, null);
    if (!operationKey || current?.operationKey === operationKey) storage?.removeItem?.(CREATE_OPERATION_KEY);
  }

  function joinOperation(storage, leagueId, ownerEmail, createId = createOperationId) {
    const key = `${canonicalEmail(ownerEmail)}|${String(leagueId || '')}`;
    const operations = readJson(storage, JOIN_OPERATION_KEY, {}) || {};
    if (!operations[key]?.operationKey) {
      operations[key] = {
        operationKey: createId(),
        leagueId: String(leagueId || ''),
        ownerEmail: canonicalEmail(ownerEmail),
        createdAt: new Date().toISOString()
      };
      writeJson(storage, JOIN_OPERATION_KEY, operations);
    }
    return operations[key];
  }

  function clearJoinOperation(storage, leagueId, ownerEmail) {
    const key = `${canonicalEmail(ownerEmail)}|${String(leagueId || '')}`;
    const operations = readJson(storage, JOIN_OPERATION_KEY, {}) || {};
    if (!Object.prototype.hasOwnProperty.call(operations, key)) return;
    delete operations[key];
    writeJson(storage, JOIN_OPERATION_KEY, operations);
  }

  function uncertainFailure(error) {
    return Boolean(error?.timedOut
      || error?.unavailable
      || error?.retryable
      || !Number(error?.status || 0)
      || Number(error?.status || 0) >= 500
      || error?.data?.code === 'league_create_conflict');
  }

  function onboardingMessage(error, fallback = 'The league setup request could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    if (code === 'league_limit_reached') return 'This account already has the maximum of three leagues.';
    if (code === 'unsupported_team_count') return 'Choose a supported league size: 4, 6, 8, 10, 12, 14, or 16 teams.';
    if (code === 'league_invite_capacity') return 'The invite list uses more manager slots than this league allows.';
    if (code === 'league_full') return 'This league is full. A commissioner must remove or decline another manager first.';
    if (code === 'invite_not_found') return 'This invitation is not assigned to the signed-in account.';
    if (code === 'join_request_conflict') return 'The join request changed while it was being approved. Refresh and try again.';
    if (uncertainFailure(error)) {
      return 'The server may have accepted this request. Retry safely; the same operation will not create a duplicate.';
    }
    return root.mutationErrorMessage?.(error, fallback)
      || error?.data?.error
      || error?.message
      || fallback;
  }

  function setFormStatus(message, error = false) {
    if (typeof root.setFormStatus === 'function') {
      root.setFormStatus(message, error);
      return;
    }
    const status = root.document?.getElementById?.('form-status');
    if (!status) return;
    status.textContent = message;
    status.classList.toggle('is-error', error);
  }

  function setFormBusy(form, busy, label = 'Creating league...') {
    if (!form) return;
    const button = form.querySelector('button[type="submit"]');
    if (busy) {
      form.dataset.onboardingBusy = 'true';
      form.setAttribute('aria-busy', 'true');
      if (button) {
        button.dataset.originalLabel = button.dataset.originalLabel || button.textContent || 'Create league';
        button.disabled = true;
        button.textContent = label;
      }
      return;
    }
    delete form.dataset.onboardingBusy;
    form.removeAttribute('aria-busy');
    if (button) {
      button.disabled = false;
      button.textContent = button.dataset.originalLabel || 'Create league';
    }
  }

  function payloadFromForm(form, ownerEmail = '') {
    const value = (id, fallback = '') => root.document?.getElementById?.(id)?.value ?? fallback;
    const scoring = value('league-scoring', 'ppr');
    const raw = {
      name: value('league-name', 'New League'),
      teams: Number.parseInt(value('league-size', '10'), 10),
      scoring,
      scoringSettings: typeof root.normalizeScoringSettings === 'function'
        ? root.normalizeScoringSettings(scoring)
        : {},
      draftType: value('draft-type', 'snake'),
      draftDate: value('draft-date', ''),
      notes: value('league-notes', ''),
      invitedEmails: parseInviteText(value('invite-emails', '')),
      rosterRules: { ...DEFAULT_ROSTER_RULES }
    };
    return validateCreatePayload(raw, ownerEmail);
  }

  async function createLeagueFromForm(form) {
    if (!form || form.dataset.onboardingBusy === 'true') return null;
    const auth = root.getAuthState?.();
    if (!auth?.token) {
      setFormStatus('Sign in before creating a league.', true);
      return null;
    }
    const validation = payloadFromForm(form, auth.email || '');
    if (!validation.ok) {
      setFormStatus(validation.errors[0], true);
      return null;
    }
    const storage = root.sessionStorage;
    const operation = pendingCreate(storage, validation.payload, auth.email || '');
    setFormBusy(form, true, operation.createdAt ? 'Creating league...' : 'Retrying safely...');
    setFormStatus('Saving league and confirming manager slots...');

    try {
      const result = await root.apiRequest('/leagues', {
        method: 'POST',
        headers: { 'Idempotency-Key': operation.operationKey },
        body: JSON.stringify(validation.payload)
      });
      const league = root.normalizeLeague?.(result) || result;
      root.saveLeagueForAccount?.(league);
      clearPendingCreate(storage, operation.operationKey);
      await root.syncLeaguesFromApi?.();
      root.setActiveLeague?.(league.id);
      await root.syncActiveLeagueCollectionsFromApi?.();
      root.loadStoredLeague?.();
      root.updateAuthUi?.();
      root.renderLeagueSummary?.();
      setFormStatus(result?.idempotentReplay
        ? 'League creation was already completed; the confirmed league is now loaded.'
        : 'League created and confirmed. Manager invitations are ready.');
      root.CFF_UI?.notify?.(`${league.name || 'League'} is ready.`, 'success');
      root.setTimeout?.(() => root.closeModal?.(), 650);
      return league;
    } catch (error) {
      if (!uncertainFailure(error)) clearPendingCreate(storage, operation.operationKey);
      const message = onboardingMessage(error, 'The server rejected these league settings.');
      setFormStatus(message, true);
      if (uncertainFailure(error)) {
        const button = form.querySelector('button[type="submit"]');
        if (button) button.dataset.originalLabel = 'Retry create safely';
      }
      return null;
    } finally {
      setFormBusy(form, false);
    }
  }

  function installCreateCapture() {
    root.document?.addEventListener?.('submit', (event) => {
      const form = event.target;
      if (!form || form.id !== 'create-league-form') return;
      event.preventDefault();
      event.stopImmediatePropagation();
      void createLeagueFromForm(form);
    }, true);
  }

  let joinAdapterAttempts = 0;
  function installJoinAdapter() {
    joinAdapterAttempts += 1;
    const original = root.joinLeagueApi;
    if (typeof original !== 'function') {
      if (joinAdapterAttempts < 200) root.setTimeout?.(installJoinAdapter, 0);
      return;
    }
    if (original.__cffLeagueOnboarding) return;

    const wrapped = async function resilientJoinLeague(leagueId) {
      const auth = root.getAuthState?.();
      const token = String(auth?.token || '');
      if (!token || token.startsWith('local-demo-')) return original.call(this, leagueId);
      const operation = joinOperation(root.sessionStorage, leagueId, auth.email || '');
      try {
        const payload = await root.apiRequest(`/leagues/${encodeURIComponent(leagueId)}/join`, {
          method: 'POST',
          headers: { 'Idempotency-Key': operation.operationKey }
        });
        if (payload?.joinStatus === 'pending_approval') return payload;
        const league = root.normalizeLeague?.(payload) || payload;
        root.saveLeagueForAccount?.(league);
        root.setActiveLeague?.(league.id);
        clearJoinOperation(root.sessionStorage, leagueId, auth.email || '');
        await root.syncLeaguesFromApi?.();
        await root.syncActiveLeagueCollectionsFromApi?.();
        return league;
      } catch (error) {
        if (!uncertainFailure(error)) clearJoinOperation(root.sessionStorage, leagueId, auth.email || '');
        error.userMessage = onboardingMessage(error, 'The league invitation could not be joined.');
        throw error;
      }
    };
    wrapped.__cffLeagueOnboarding = true;
    wrapped.__cffOriginal = original;
    root.joinLeagueApi = wrapped;
  }

  const helpers = {
    ALLOWED_TEAM_COUNTS,
    canonicalEmail,
    validEmail,
    createOperationId,
    normalizeInviteList,
    parseInviteText,
    validateCreatePayload,
    stableCreateFingerprint,
    pendingCreate,
    clearPendingCreate,
    joinOperation,
    clearJoinOperation,
    uncertainFailure,
    onboardingMessage
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  installCreateCapture();
  root.setTimeout?.(installJoinAdapter, 0);
  root.CFFLeagueOnboarding = Object.freeze({
    ...helpers,
    installed: true,
    createLeagueFromForm
  });
  document.documentElement.dataset.cffLeagueOnboarding = 'true';
})(typeof window !== 'undefined' ? window : globalThis);
