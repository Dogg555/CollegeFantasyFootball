(() => {
  'use strict';

  const QUEUE_STORE_KEY = 'cff_v2_draft_queue_by_account_league';
  const ROSTER_STORE_KEY = 'cff_v2_roster_by_account_league';
  const PLAYER_POOL_STORE_KEY = 'cff_v2_player_pool_by_account_league';
  const LEGACY_QUEUE_KEY = 'cff_draft_queue';
  const LEGACY_ROSTER_KEY = 'cff_roster';
  const joinInfoCache = new Map();

  function readStore(key) {
    try {
      return JSON.parse(localStorage.getItem(key) || '{}');
    } catch {
      return {};
    }
  }

  function writeStore(key, value) {
    localStorage.setItem(key, JSON.stringify(value));
  }

  function activeScope() {
    const leagueId = window.getLeagueState?.()?.id || '';
    if (!leagueId) return '';
    const email = String(window.getAuthState?.()?.email || 'anonymous').trim().toLowerCase();
    return `${email}::${leagueId}`;
  }

  function readScopedItems(key, legacyKey = '') {
    const scope = activeScope();
    if (!scope) return [];
    const store = readStore(key);
    if (!Array.isArray(store[scope])) {
      const allowLegacyDemo = Boolean(window.isLocalDemoSession?.());
      let initial = [];
      if (allowLegacyDemo && legacyKey) {
        try {
          const legacy = JSON.parse(localStorage.getItem(legacyKey) || '[]');
          if (Array.isArray(legacy)) initial = legacy;
        } catch {
          initial = [];
        }
      }
      store[scope] = initial;
      writeStore(key, store);
      if (legacyKey) localStorage.removeItem(legacyKey);
    }
    return store[scope];
  }

  function writeScopedItems(key, items) {
    const scope = activeScope();
    if (!scope) return;
    const store = readStore(key);
    store[scope] = Array.isArray(items) ? items : [];
    writeStore(key, store);
  }

  function deleteCurrentScope(key) {
    const scope = activeScope();
    if (!scope) return;
    const store = readStore(key);
    delete store[scope];
    writeStore(key, store);
  }

  if (!window.isLocalDemoSession?.()) {
    localStorage.removeItem(LEGACY_QUEUE_KEY);
    localStorage.removeItem(LEGACY_ROSTER_KEY);
  }

  const originalGetQueue = window.getQueue;
  const originalSetQueue = window.setQueue;
  const originalGetRoster = window.getRoster;
  const originalSetRoster = window.setRoster;
  const originalClearSessionState = window.clearSessionState;
  const originalClearDraftState = window.clearDraftState;
  const originalGetAvailablePlayers = window.getAvailablePlayers;
  const originalNormalizeLeague = window.normalizeLeague;
  const originalSaveLeagueToApi = window.saveLeagueToApi;
  const originalSyncLeagueCollections = window.syncActiveLeagueCollectionsFromApi;

  window.getQueue = function getScopedQueue() {
    if (window.isLocalDemoSession?.() && !activeScope()) return originalGetQueue?.() || [];
    return readScopedItems(QUEUE_STORE_KEY, LEGACY_QUEUE_KEY);
  };

  window.setQueue = function setScopedQueue(queue) {
    if (window.isLocalDemoSession?.() && !activeScope()) {
      originalSetQueue?.(queue);
      return;
    }
    writeScopedItems(QUEUE_STORE_KEY, queue);
  };

  window.getRoster = function getScopedRoster() {
    if (window.isLocalDemoSession?.() && !activeScope()) return originalGetRoster?.() || [];
    return readScopedItems(ROSTER_STORE_KEY, LEGACY_ROSTER_KEY);
  };

  window.setRoster = function setScopedRoster(roster) {
    if (window.isLocalDemoSession?.() && !activeScope()) {
      originalSetRoster?.(roster);
      return;
    }
    writeScopedItems(ROSTER_STORE_KEY, roster);
  };

  window.clearDraftState = function clearScopedDraftState() {
    deleteCurrentScope(QUEUE_STORE_KEY);
    deleteCurrentScope(ROSTER_STORE_KEY);
    deleteCurrentScope(PLAYER_POOL_STORE_KEY);
    localStorage.removeItem(LEGACY_QUEUE_KEY);
    localStorage.removeItem(LEGACY_ROSTER_KEY);
    if (typeof originalClearDraftState === 'function') {
      const queue = window.getQueue;
      const roster = window.getRoster;
      try {
        originalClearDraftState();
      } finally {
        window.getQueue = queue;
        window.getRoster = roster;
      }
    }
  };

  window.clearSessionState = function clearScopedSessionState() {
    originalClearSessionState?.();
    [QUEUE_STORE_KEY, ROSTER_STORE_KEY, PLAYER_POOL_STORE_KEY].forEach((key) => localStorage.removeItem(key));
  };

  function datetimeLocalValue(value) {
    if (!value) return '';
    const text = String(value);
    const direct = text.match(/^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2})/);
    if (direct) return direct[1];
    const parsed = new Date(text);
    return Number.isNaN(parsed.getTime()) ? '' : parsed.toISOString().slice(0, 16);
  }

  window.normalizeLeague = function normalizeStableLeague(league = {}) {
    const normalized = originalNormalizeLeague ? originalNormalizeLeague(league) : { ...league };
    normalized.draftDate = datetimeLocalValue(normalized.draftDate);
    normalized.draftLobbyStartedAt = datetimeLocalValue(normalized.draftLobbyStartedAt);
    if (league.joinCode) normalized.joinCode = league.joinCode;
    return normalized;
  };

  function playerPool() {
    if (window.isLocalDemoSession?.()) return originalGetAvailablePlayers?.() || [];
    return readScopedItems(PLAYER_POOL_STORE_KEY);
  }

  window.getAvailablePlayers = function getServerBackedAvailablePlayers() {
    const rostered = new Set((window.getRoster?.() || []).map((player) => String(player.id)));
    return playerPool().filter((player) => !rostered.has(String(player.id)));
  };

  async function syncPlayerPoolFromApi() {
    if (!window.getAuthState?.()?.token || window.isLocalDemoSession?.()) return window.getAvailablePlayers();
    const league = window.getLeagueState?.();
    if (!league?.id) return [];
    try {
      const players = await window.apiRequest(`/leagues/${encodeURIComponent(league.id)}/player-pool`);
      const normalized = Array.isArray(players) ? players.map((player) => window.normalizePlayer(player)) : [];
      writeScopedItems(PLAYER_POOL_STORE_KEY, normalized);
      return normalized;
    } catch (error) {
      writeScopedItems(PLAYER_POOL_STORE_KEY, []);
      throw error;
    }
  }

  window.syncLeaguePlayerPoolFromApi = syncPlayerPoolFromApi;

  window.syncActiveLeagueCollectionsFromApi = async function syncStableLeagueCollections() {
    const result = await originalSyncLeagueCollections?.();
    try {
      await syncPlayerPoolFromApi();
    } catch {
      // Never reintroduce demo players when the real player catalog is unavailable.
    }
    return result;
  };

  function sortedObject(value) {
    if (Array.isArray(value)) return value.map(sortedObject);
    if (!value || typeof value !== 'object') return value;
    return Object.keys(value).sort().reduce((result, key) => {
      result[key] = sortedObject(value[key]);
      return result;
    }, {});
  }

  function canonicalSetting(field, value) {
    if (field === 'draftDate') return datetimeLocalValue(value);
    if (field === 'teams') return Number(value);
    if (field === 'invitedEmails') {
      return [...new Set((Array.isArray(value) ? value : [])
        .map((email) => String(email).trim().toLowerCase())
        .filter(Boolean))].sort();
    }
    if (['scoringSettings', 'rosterRules', 'waiverRules', 'tradeRules'].includes(field)) {
      return sortedObject(value || {});
    }
    return value ?? '';
  }

  function settingsMismatches(requested, saved) {
    const fields = [
      'name',
      'teams',
      'scoring',
      'draftDate',
      'draftLobbyOpen',
      'draftLobbyStartedAt',
      'notes',
      'invitedEmails',
      'scoringSettings',
      'rosterRules',
      'waiverRules',
      'tradeRules'
    ];
    return fields.filter((field) => JSON.stringify(canonicalSetting(field, requested[field]))
      !== JSON.stringify(canonicalSetting(field, saved[field])));
  }

  window.saveLeagueToApi = async function saveVerifiedLeagueSettings(league) {
    const normalized = window.normalizeLeague(league);
    if (!window.getAuthState?.()?.token || window.isLocalDemoSession?.()) {
      return originalSaveLeagueToApi(normalized);
    }
    if (!normalized.id || normalized.id.startsWith('local-')) {
      throw new Error('A server league must be selected before settings can be saved.');
    }
    const response = await window.apiRequest(`/leagues/${encodeURIComponent(normalized.id)}/settings`, {
      method: 'PUT',
      body: JSON.stringify(normalized)
    });
    const saved = window.normalizeLeague(response);
    const mismatches = settingsMismatches(normalized, saved);
    if (mismatches.length) {
      const error = new Error(`The server did not persist: ${mismatches.join(', ')}.`);
      error.status = 409;
      error.data = {
        error: error.message,
        code: 'SETTINGS_NOT_VERIFIED',
        fields: mismatches
      };
      throw error;
    }
    return window.saveLeagueForAccount(saved);
  };

  window.joinLeagueApi = async function joinLeagueWithCode(code) {
    if (!window.getAuthState?.()?.token || !code) return null;
    const payload = await window.apiRequest('/leagues/join', {
      method: 'POST',
      body: JSON.stringify({ code: String(code).trim() })
    });
    if (payload?.joinStatus === 'pending_approval') return payload;
    const league = window.normalizeLeague(payload);
    window.saveLeagueForAccount(league, { activate: true });
    window.setActiveLeague?.(league.id);
    await window.syncActiveLeagueCollectionsFromApi?.();
    return league;
  };

  function addSmallTeamOptions() {
    const select = document.getElementById('settings-teams');
    if (!select) return;
    [4, 6].reverse().forEach((teams) => {
      if (select.querySelector(`option[value="${teams}"]`)) return;
      const option = document.createElement('option');
      option.value = String(teams);
      option.textContent = `${teams} teams`;
      select.prepend(option);
    });
  }

  function injectJoinForm() {
    if (document.getElementById('join-league-by-code')) return;
    const inviteFlow = document.getElementById('invite-flow');
    const tabs = document.querySelector('.league-tabs');
    if (!inviteFlow && !tabs) return;

    const card = document.createElement('section');
    card.id = 'join-league-by-code';
    card.className = 'card';
    card.innerHTML = `
      <div class="card__header">
        <div>
          <h2>Join a league</h2>
          <div class="muted small">Enter the commissioner's join code. Uninvited accounts send an approval request.</div>
        </div>
        <span class="pill pill--muted">Join code</span>
      </div>
      <form id="join-league-code-form" class="form">
        <label class="field">
          <span>League code</span>
          <input id="join-league-code" type="text" maxlength="32" autocomplete="off" placeholder="ABCD-EFGH" />
        </label>
        <div class="form__footer">
          <div id="join-league-code-status" class="muted small" role="status"></div>
          <button class="button button--primary" type="submit">Join league</button>
        </div>
      </form>
    `;
    (inviteFlow || tabs).insertAdjacentElement('afterend', card);

    const form = card.querySelector('#join-league-code-form');
    const input = card.querySelector('#join-league-code');
    const status = card.querySelector('#join-league-code-status');
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      const code = input.value.trim();
      if (!code) {
        status.textContent = 'Enter a join code.';
        input.focus();
        return;
      }
      if (!window.getAuthState?.()?.token) {
        window.location.href = `signin.html?invite=${encodeURIComponent(code)}`;
        return;
      }
      const button = form.querySelector('button[type="submit"]');
      button.disabled = true;
      status.textContent = 'Checking league code...';
      try {
        const joined = await window.joinLeagueApi(code);
        if (joined?.joinStatus === 'pending_approval') {
          window.recordPendingJoin?.(joined);
          status.textContent = joined.message || 'Join request submitted.';
          window.CFF_UI?.notify(status.textContent, 'info');
        } else if (joined?.id) {
          status.textContent = `Joined ${joined.name}.`;
          input.value = '';
          window.clearPendingJoin?.(joined.id);
          window.renderLeague?.();
          window.CFF_UI?.notify(status.textContent, 'success');
        }
      } catch (error) {
        status.textContent = window.mutationErrorMessage?.(error, 'Could not join this league.') || error.message;
      } finally {
        button.disabled = false;
      }
    });
  }

  async function refreshJoinCodePanel() {
    const settings = document.getElementById('commissioner-settings');
    if (!settings) return;
    let panel = document.getElementById('commissioner-join-code');
    const league = window.getLeagueState?.();
    const commissioner = Boolean(league && window.isCurrentCommissioner?.(league));
    if (!commissioner) {
      panel?.remove();
      return;
    }
    if (!panel) {
      panel = document.createElement('div');
      panel.id = 'commissioner-join-code';
      panel.className = 'row';
      settings.querySelector('.card__header')?.insertAdjacentElement('afterend', panel);
    }
    panel.innerHTML = '<div><strong>League join code</strong><div class="muted">Loading...</div></div>';
    try {
      let info = joinInfoCache.get(league.id);
      if (!info) {
        info = await window.apiRequest(`/leagues/${encodeURIComponent(league.id)}/join-info`);
        joinInfoCache.set(league.id, info);
      }
      const link = `${window.location.origin}/signin.html?invite=${encodeURIComponent(info.joinCode)}`;
      panel.innerHTML = `
        <div>
          <strong>League join code: ${window.escapeHtml?.(info.joinCode) || info.joinCode}</strong>
          <div class="muted">Share the code or link. Invited users join immediately; others request approval.</div>
        </div>
        <div class="actions">
          <button class="button button--ghost" id="copy-stable-join-code" type="button">Copy code</button>
          <button class="button button--ghost" id="copy-stable-join-link" type="button">Copy link</button>
        </div>
      `;
      panel.querySelector('#copy-stable-join-code')?.addEventListener('click', async () => {
        await navigator.clipboard.writeText(info.joinCode);
        window.CFF_UI?.notify('Join code copied.', 'success');
      });
      panel.querySelector('#copy-stable-join-link')?.addEventListener('click', async () => {
        await navigator.clipboard.writeText(link);
        window.CFF_UI?.notify('Join link copied.', 'success');
      });
      window.inviteUrl = () => link;
    } catch {
      panel.innerHTML = '<div><strong>League join code</strong><div class="muted">Join code is temporarily unavailable.</div></div>';
    }
  }

  function installRealPlayerActions() {
    document.addEventListener('click', (event) => {
      const button = event.target.closest?.('[data-add-free-agent]');
      if (!button || window.isLocalDemoSession?.()) return;
      const player = window.getAvailablePlayers?.().find((item) => String(item.id) === String(button.dataset.addFreeAgent));
      if (!player) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      button.disabled = true;
      (async () => {
        try {
          await window.addFreeAgentApi(player);
          await syncPlayerPoolFromApi().catch(() => []);
        } catch (error) {
          const status = document.getElementById('waiver-status');
          if (status) status.textContent = window.mutationErrorMessage?.(error, 'Could not add player.') || error.message;
        }
        window.renderLeague?.();
      })();
    }, true);

    document.getElementById('waiver-form')?.addEventListener('submit', (event) => {
      if (window.isLocalDemoSession?.()) return;
      const addSelect = document.getElementById('waiver-add-player');
      const player = window.getAvailablePlayers?.().find((item) => String(item.id) === String(addSelect?.value));
      if (!player) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      const status = document.getElementById('waiver-status');
      const dropPlayerId = document.getElementById('waiver-drop-player')?.value || '';
      (async () => {
        try {
          await window.submitWaiverClaimApi(player, dropPlayerId);
          if (status) status.textContent = 'Waiver claim submitted.';
        } catch (error) {
          if (status) status.textContent = window.mutationErrorMessage?.(error, 'Could not submit waiver claim.') || error.message;
        }
        window.renderLeague?.();
      })();
    }, true);
  }

  function wrapLeagueRender() {
    if (typeof window.renderLeague !== 'function' || window.renderLeague.__cffStableWrapped) return;
    const original = window.renderLeague;
    const wrapped = function stableRenderLeague(...args) {
      const result = original.apply(this, args);
      queueMicrotask(() => refreshJoinCodePanel());
      return result;
    };
    wrapped.__cffStableWrapped = true;
    window.renderLeague = wrapped;
  }

  document.addEventListener('DOMContentLoaded', () => {
    addSmallTeamOptions();
    injectJoinForm();
    installRealPlayerActions();
    wrapLeagueRender();
    refreshJoinCodePanel();
  });
})();
