(() => {
  'use strict';

  const PENDING_JOIN_KEY = 'cff_pending_join_requests';
  const initialRequestedLeagueId = (() => {
    try {
      const params = new URLSearchParams(window.location?.search || '');
      return params.get('leagueId') || params.get('league') || '';
    } catch {
      return '';
    }
  })();

  let installed = false;
  let dismissedRouteWarning = false;
  let lastContextError = null;
  let currentState = Object.freeze({
    kind: 'loading',
    reason: 'initializing',
    blocking: true,
    retryable: false,
    leagueId: '',
    fallbackLeagueId: ''
  });

  function authState() {
    return window.getAuthState?.() || null;
  }

  function accountEmail() {
    return String(authState()?.email || '').trim().toLowerCase();
  }

  function leagues() {
    const value = window.getLeaguesForCurrentAccount?.();
    return Array.isArray(value) ? value : [];
  }

  function selectedLeague() {
    return window.getLeagueState?.() || null;
  }

  function serverSessionActive() {
    const auth = authState();
    return Boolean(auth?.token && !window.isLocalDemoSession?.());
  }

  function pendingJoinRequests() {
    const email = accountEmail() || 'anonymous';
    try {
      const store = JSON.parse(window.localStorage?.getItem(PENDING_JOIN_KEY) || '{}');
      return Array.isArray(store?.[email]) ? store[email] : [];
    } catch {
      return [];
    }
  }

  function inviteRequested() {
    try {
      return Boolean(new URLSearchParams(window.location?.search || '').get('invite'));
    } catch {
      return false;
    }
  }

  function contextErrorState(error, league) {
    const status = Number(error?.status || 0);
    const reasonCode = String(error?.data?.code || '');
    if (status === 401 || status === 403) {
      return {
        kind: 'league_unavailable',
        reason: 'unauthorized',
        blocking: true,
        retryable: false,
        leagueId: league?.id || '',
        fallbackLeagueId: ''
      };
    }
    if (status === 404) {
      return {
        kind: 'league_unavailable',
        reason: 'deleted_or_access_removed',
        blocking: true,
        retryable: false,
        leagueId: league?.id || initialRequestedLeagueId,
        fallbackLeagueId: ''
      };
    }
    if (status === 503 || error?.unavailable || error?.timedOut) {
      return {
        kind: 'service_failure',
        reason: error?.timedOut ? 'timeout' : 'unavailable',
        blocking: false,
        retryable: true,
        leagueId: league?.id || '',
        fallbackLeagueId: ''
      };
    }
    if (reasonCode === 'LEAGUE_CONTEXT_REQUIRED') {
      return deriveState(null);
    }
    return {
      kind: 'error',
      reason: reasonCode || 'unexpected',
      blocking: false,
      retryable: true,
      leagueId: league?.id || '',
      fallbackLeagueId: ''
    };
  }

  function deriveState(error = lastContextError) {
    const auth = authState();
    const knownLeagues = leagues();
    const active = selectedLeague();
    const activeId = String(active?.id || '');
    const requestedKnown = initialRequestedLeagueId
      && knownLeagues.some((league) => String(league?.id || '') === initialRequestedLeagueId);

    if (!auth?.token) {
      if (inviteRequested()) {
        return {
          kind: 'pending_invite',
          reason: 'signin_required',
          blocking: true,
          retryable: false,
          leagueId: initialRequestedLeagueId,
          fallbackLeagueId: ''
        };
      }
      return {
        kind: 'signed_out',
        reason: 'authentication_required',
        blocking: true,
        retryable: false,
        leagueId: '',
        fallbackLeagueId: ''
      };
    }

    if (!knownLeagues.length) {
      if (inviteRequested() || pendingJoinRequests().length) {
        return {
          kind: 'pending_invite',
          reason: pendingJoinRequests().length ? 'approval_pending' : 'invite_ready',
          blocking: true,
          retryable: false,
          leagueId: initialRequestedLeagueId,
          fallbackLeagueId: ''
        };
      }
      return {
        kind: 'no_leagues',
        reason: 'empty_account',
        blocking: true,
        retryable: false,
        leagueId: '',
        fallbackLeagueId: ''
      };
    }

    if (initialRequestedLeagueId && !requestedKnown && !dismissedRouteWarning) {
      return {
        kind: 'league_unavailable',
        reason: 'unauthorized_or_missing',
        blocking: false,
        retryable: false,
        leagueId: initialRequestedLeagueId,
        fallbackLeagueId: activeId
      };
    }

    if (error) return contextErrorState(error, active);

    if (serverSessionActive()) {
      const context = window.getLeagueContext?.();
      if (context?.leagueId === activeId) {
        return {
          kind: 'ready',
          reason: 'authoritative',
          blocking: false,
          retryable: false,
          leagueId: activeId,
          fallbackLeagueId: ''
        };
      }
      const cache = window.apiCacheMeta?.('leagueContext') || window.apiCacheMeta?.('league');
      if (cache?.stale || window.mutationControlsDisabled?.()) {
        return {
          kind: 'service_failure',
          reason: 'cached_data',
          blocking: false,
          retryable: true,
          leagueId: activeId,
          fallbackLeagueId: ''
        };
      }
      return {
        kind: 'loading',
        reason: 'authoritative_context',
        blocking: false,
        retryable: false,
        leagueId: activeId,
        fallbackLeagueId: ''
      };
    }

    return {
      kind: 'ready',
      reason: 'local_demo',
      blocking: false,
      retryable: false,
      leagueId: activeId,
      fallbackLeagueId: ''
    };
  }

  function stateCopy(state) {
    const active = selectedLeague();
    const fallbackName = state.fallbackLeagueId && String(active?.id || '') === state.fallbackLeagueId
      ? active?.name || 'your available league'
      : 'your available league';
    const copies = {
      loading: {
        title: 'Loading league',
        message: 'Confirming your league access and current team assignment with the server.',
        badge: 'Loading'
      },
      signed_out: {
        title: 'Sign in required',
        message: 'Sign in to view private league information and manager workflows.',
        badge: 'Private'
      },
      no_leagues: {
        title: 'No leagues yet',
        message: 'Create a league or join one with a commissioner invite to get started.',
        badge: 'Empty'
      },
      pending_invite: {
        title: 'League access pending',
        message: state.reason === 'approval_pending'
          ? 'Your join request is waiting for commissioner approval.'
          : 'Complete the invite flow to request or confirm league access.',
        badge: 'Pending'
      },
      league_unavailable: {
        title: 'League unavailable',
        message: state.fallbackLeagueId
          ? `The requested league was deleted or you no longer have access. Showing ${fallbackName} instead.`
          : 'This league was deleted or your account no longer has access to it.',
        badge: 'Unavailable'
      },
      service_failure: {
        title: 'League service unavailable',
        message: 'Cached league information may be shown, but changes are disabled until the server recovers.',
        badge: 'Offline'
      },
      error: {
        title: 'League could not be loaded',
        message: 'The league returned an unexpected error. Retry before making any changes.',
        badge: 'Error'
      }
    };
    return copies[state.kind] || copies.error;
  }

  function ensurePanel() {
    if (!window.document?.createElement) return null;
    let panel = window.document.getElementById?.('league-workspace-state');
    if (panel) return panel;
    const main = window.document.querySelector?.('main.league-dashboard');
    if (!main) return null;
    panel = window.document.createElement('section');
    panel.id = 'league-workspace-state';
    panel.className = 'card card--accent';
    panel.setAttribute('role', 'status');
    panel.setAttribute('aria-live', 'polite');
    const tabs = main.querySelector?.('.league-tabs');
    if (tabs?.parentNode) tabs.parentNode.insertBefore(panel, tabs);
    else main.prepend?.(panel);
    return panel;
  }

  function render() {
    const panel = ensurePanel();
    if (!panel) return;
    if (currentState.kind === 'ready') {
      panel.hidden = true;
      panel.innerHTML = '';
      return;
    }
    panel.hidden = false;
    const copy = stateCopy(currentState);
    const actions = [];
    if (currentState.kind === 'signed_out') {
      actions.push('<a class="button button--primary" href="signin.html">Sign in</a>');
    } else if (currentState.kind === 'no_leagues') {
      actions.push('<a class="button button--primary" href="index.html">Create a league</a>');
    } else if (currentState.kind === 'pending_invite') {
      actions.push('<a class="button button--ghost" href="league.html">Review league access</a>');
    } else if (currentState.kind === 'league_unavailable' && currentState.fallbackLeagueId) {
      actions.push('<button class="button button--primary" data-league-state-dismiss type="button">Continue to current league</button>');
    } else if (currentState.kind === 'league_unavailable') {
      actions.push('<a class="button button--primary" href="league.html">Choose another league</a>');
    } else if (currentState.retryable) {
      actions.push('<button class="button button--primary" data-league-state-retry type="button">Retry</button>');
    }
    panel.innerHTML = `
      <div class="card__header">
        <div>
          <h2>${copy.title}</h2>
          <div class="muted small">${copy.message}</div>
        </div>
        <span class="pill pill--muted">${copy.badge}</span>
      </div>
      ${actions.length ? `<div class="cta-row section-actions">${actions.join('')}</div>` : ''}
    `;
    panel.querySelector?.('[data-league-state-dismiss]')?.addEventListener('click', () => {
      dismissedRouteWarning = true;
      lastContextError = null;
      refresh();
    });
    panel.querySelector?.('[data-league-state-retry]')?.addEventListener('click', () => {
      retry();
    });
  }

  function publish(next) {
    currentState = Object.freeze({ ...next });
    render();
    window.dispatchEvent?.(new CustomEvent('cff:league-workspace-state', {
      detail: currentState
    }));
    return currentState;
  }

  function refresh(error = lastContextError) {
    if (error !== undefined) lastContextError = error;
    return publish(deriveState(lastContextError));
  }

  async function retry() {
    lastContextError = null;
    publish({
      kind: 'loading',
      reason: 'retry',
      blocking: false,
      retryable: false,
      leagueId: String(selectedLeague()?.id || ''),
      fallbackLeagueId: ''
    });
    try {
      const result = await window.syncLeagueContextFromApi?.(String(selectedLeague()?.id || ''));
      lastContextError = null;
      refresh(null);
      return result;
    } catch (error) {
      lastContextError = error;
      refresh(error);
      return null;
    }
  }

  function install() {
    if (installed) return true;
    if (typeof window.getAuthState !== 'function'
      || typeof window.getLeagueState !== 'function'
      || typeof window.getLeaguesForCurrentAccount !== 'function') {
      return false;
    }
    installed = true;

    const originalSyncContext = window.syncLeagueContextFromApi;
    const originalSyncCollections = window.syncActiveLeagueCollectionsFromApi;
    const originalSyncDraft = window.syncDraftFromApi;
    const originalReplaceLeagues = window.replaceLeaguesForCurrentAccount;
    const originalClearSessionState = window.clearSessionState;

    if (typeof originalSyncContext === 'function') {
      window.syncLeagueContextFromApi = async function syncContextWithState(...args) {
        publish({
          kind: 'loading',
          reason: 'context_sync',
          blocking: false,
          retryable: false,
          leagueId: String(args[0] || selectedLeague()?.id || ''),
          fallbackLeagueId: ''
        });
        try {
          const result = await originalSyncContext(...args);
          lastContextError = null;
          refresh(null);
          return result;
        } catch (error) {
          lastContextError = error;
          refresh(error);
          throw error;
        }
      };
    }

    if (typeof originalSyncCollections === 'function') {
      window.syncActiveLeagueCollectionsFromApi = async function syncCollectionsWithState(...args) {
        try {
          const result = await originalSyncCollections(...args);
          lastContextError = null;
          refresh(null);
          return result;
        } catch (error) {
          lastContextError = error;
          refresh(error);
          throw error;
        }
      };
    }

    if (typeof originalSyncDraft === 'function') {
      window.syncDraftFromApi = async function syncDraftWithState(...args) {
        try {
          const result = await originalSyncDraft(...args);
          lastContextError = null;
          refresh(null);
          return result;
        } catch (error) {
          lastContextError = error;
          refresh(error);
          throw error;
        }
      };
    }

    if (typeof originalReplaceLeagues === 'function') {
      window.replaceLeaguesForCurrentAccount = function replaceLeaguesWithState(...args) {
        const result = originalReplaceLeagues(...args);
        lastContextError = null;
        refresh(null);
        return result;
      };
    }

    window.clearSessionState = function clearLeagueWorkspaceState(...args) {
      dismissedRouteWarning = false;
      lastContextError = null;
      const result = originalClearSessionState?.(...args);
      refresh(null);
      return result;
    };

    window.addEventListener?.('cff:league-authority-changed', () => {
      lastContextError = null;
      refresh(null);
    });
    window.addEventListener?.('cff:league-context-changed', () => {
      dismissedRouteWarning = true;
      lastContextError = null;
      refresh(null);
    });
    window.addEventListener?.('online', () => retry());
    window.addEventListener?.('offline', () => {
      lastContextError = { unavailable: true };
      refresh(lastContextError);
    });
    window.addEventListener?.('storage', (event) => {
      if ([PENDING_JOIN_KEY, 'cff_leagues_by_account', 'cff_api_cache_meta'].includes(event?.key)) {
        refresh();
      }
    });

    window.CFF_LEAGUE_WORKSPACE_STATE = Object.freeze({
      current: () => currentState,
      refresh,
      retry,
      dismissRouteWarning: () => {
        dismissedRouteWarning = true;
        return refresh(null);
      },
      initialRequestedLeagueId
    });

    refresh(null);
    if (window.document?.readyState === 'loading') {
      window.document.addEventListener?.('DOMContentLoaded', render, { once: true });
    } else {
      render();
    }
    return true;
  }

  if (!install()) {
    const timer = window.setInterval(() => {
      if (install()) window.clearInterval(timer);
    }, 0);
    window.addEventListener?.('load', () => {
      if (install()) window.clearInterval(timer);
    }, { once: true });
  }
})();