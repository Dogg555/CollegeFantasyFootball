(() => {
  'use strict';

  let installed = false;
  let activeContext = null;
  let contextPromise = null;

  function serverSessionActive() {
    const auth = window.getAuthState?.();
    return Boolean(auth?.token && !window.isLocalDemoSession?.());
  }

  function selectedLeagueId() {
    return String(
      window.CFF_LEAGUE_CONTEXT?.currentLeagueId?.()
      || window.getLeagueState?.()?.id
      || ''
    );
  }

  function contextFor(leagueId = selectedLeagueId()) {
    const candidate = String(leagueId || '');
    return activeContext?.leagueId === candidate ? activeContext : null;
  }

  function leaguePath(path) {
    const match = String(path || '').match(/^\/leagues\/([^/?#]+)(?:\/|$)/);
    if (!match || match[1] === 'join') return null;
    return {
      leagueId: decodeURIComponent(match[1]),
      suffix: String(path).slice(match[0].length - 1)
    };
  }

  function contextError(message, code, status) {
    const error = new Error(message);
    error.status = status;
    error.data = { error: message, code };
    return error;
  }

  function requiresAssignedTeam(path, method) {
    if (['GET', 'HEAD', 'OPTIONS'].includes(method)) return false;
    return /^\/leagues\/[^/]+\/(?:roster(?:\/|$)|waivers(?:\/|$)|trades(?:\/|$))/.test(path);
  }

  function loadWorkspaceStates() {
    if (!window.document?.createElement || !window.document?.head) return;
    const page = String(window.location?.pathname || '').split('/').pop();
    if (page !== 'league.html') return;
    if (window.document.querySelector?.('script[data-cff-league-workspace-states="true"]')) return;
    const script = window.document.createElement('script');
    script.src = 'league-workspace-states.js';
    script.dataset.cffLeagueWorkspaceStates = 'true';
    script.async = false;
    window.document.head.appendChild(script);
  }

  function install() {
    if (installed) return true;
    if (typeof window.apiRequest !== 'function'
      || typeof window.getAuthState !== 'function'
      || typeof window.getLeagueState !== 'function') {
      return false;
    }

    installed = true;
    const originalApiRequest = window.apiRequest;
    const originalCurrentMemberRole = window.currentMemberRole;
    const originalIsCurrentCommissioner = window.isCurrentCommissioner;
    const originalSyncCollections = window.syncActiveLeagueCollectionsFromApi;
    const originalSyncDraft = window.syncDraftFromApi;
    const originalClearSessionState = window.clearSessionState;

    async function syncLeagueContextFromApi(leagueId = selectedLeagueId()) {
      const candidate = String(leagueId || '');
      if (!serverSessionActive()) {
        activeContext = null;
        contextPromise = null;
        return null;
      }
      if (!candidate) {
        throw contextError('No league is selected.', 'LEAGUE_CONTEXT_REQUIRED', 409);
      }
      if (contextFor(candidate)) return activeContext;
      if (contextPromise?.leagueId === candidate) return contextPromise.value;

      const pending = originalApiRequest(`/leagues/${encodeURIComponent(candidate)}/context`)
        .then((context) => {
          if (!context?.leagueId || String(context.leagueId) !== candidate) {
            throw contextError(
              'The server returned an invalid league context.',
              'LEAGUE_CONTEXT_INVALID',
              502
            );
          }
          activeContext = Object.freeze({
            ...context,
            permissions: Object.freeze({ ...(context.permissions || {}) })
          });
          window.dispatchEvent?.(new CustomEvent('cff:league-authority-changed', {
            detail: { leagueId: candidate, context: activeContext }
          }));
          return activeContext;
        })
        .finally(() => {
          if (contextPromise?.leagueId === candidate) contextPromise = null;
        });
      contextPromise = { leagueId: candidate, value: pending };
      return pending;
    }

    window.apiRequest = async function authoritativeApiRequest(path, options = {}) {
      const method = String(options.method || 'GET').toUpperCase();
      const scoped = leaguePath(path);
      if (serverSessionActive() && scoped && !['GET', 'HEAD', 'OPTIONS'].includes(method)) {
        const selected = selectedLeagueId();
        if (!selected || scoped.leagueId !== selected) {
          throw contextError(
            'This action targets a different league than the active page.',
            'LEAGUE_CONTEXT_MISMATCH',
            409
          );
        }
        const context = contextFor(selected) || await syncLeagueContextFromApi(selected);
        if (requiresAssignedTeam(path, method) && !context?.teamAssigned) {
          throw contextError(
            'A fantasy team must be assigned before using this workflow.',
            'TEAM_ASSIGNMENT_REQUIRED',
            403
          );
        }
      }
      return originalApiRequest(path, options);
    };

    window.getLeagueContext = function getAuthoritativeLeagueContext() {
      return contextFor();
    };
    window.hasAuthoritativeLeagueContext = function hasAuthoritativeLeagueContext(leagueId = selectedLeagueId()) {
      return Boolean(contextFor(leagueId));
    };
    window.syncLeagueContextFromApi = syncLeagueContextFromApi;

    window.currentMemberRole = function authoritativeMemberRole(league = window.getLeagueState?.()) {
      const context = league?.id ? contextFor(league.id) : null;
      if (context) return context.userRole === 'COMMISSIONER' ? 'commissioner' : 'member';
      return originalCurrentMemberRole?.(league) || null;
    };

    window.isCurrentCommissioner = function authoritativeCommissioner(league = window.getLeagueState?.()) {
      const context = league?.id ? contextFor(league.id) : null;
      if (context) return Boolean(context.isCommissioner);
      return Boolean(originalIsCurrentCommissioner?.(league));
    };

    if (typeof originalSyncCollections === 'function') {
      window.syncActiveLeagueCollectionsFromApi = async function syncAuthoritativeCollections(...args) {
        if (serverSessionActive() && selectedLeagueId()) {
          await syncLeagueContextFromApi(selectedLeagueId());
        }
        return originalSyncCollections(...args);
      };
    }

    if (typeof originalSyncDraft === 'function') {
      window.syncDraftFromApi = async function syncAuthoritativeDraft(...args) {
        if (serverSessionActive() && selectedLeagueId()) {
          await syncLeagueContextFromApi(selectedLeagueId());
        }
        return originalSyncDraft(...args);
      };
    }

    window.clearSessionState = function clearAuthoritativeSession() {
      activeContext = null;
      contextPromise = null;
      return originalClearSessionState?.();
    };

    window.addEventListener?.('cff:league-context-changed', (event) => {
      const nextLeagueId = String(event?.detail?.leagueId || selectedLeagueId());
      if (activeContext?.leagueId !== nextLeagueId) activeContext = null;
      contextPromise = null;
    });

    window.CFF_LEAGUE_AUTHORITY = Object.freeze({
      current: () => contextFor(),
      has: (leagueId = selectedLeagueId()) => Boolean(contextFor(leagueId)),
      sync: syncLeagueContextFromApi
    });
    return true;
  }

  loadWorkspaceStates();

  if (!install()) {
    const timer = window.setInterval(() => {
      if (install()) window.clearInterval(timer);
    }, 0);
    window.addEventListener?.('load', () => {
      if (install()) window.clearInterval(timer);
    }, { once: true });
  }
})();
