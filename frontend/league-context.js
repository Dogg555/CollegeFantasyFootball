(() => {
  'use strict';

  const SESSION_KEY = 'cff_active_league_context_by_account';
  const CANONICAL_PARAM = 'leagueId';
  const LEGACY_PARAM = 'league';
  const SCOPED_PAGES = new Set(['league.html', 'draft.html', 'players.html']);
  let installed = false;

  function pageName(pathname = window.location?.pathname || '') {
    return String(pathname).split('/').pop() || 'index.html';
  }

  function readSessionStore() {
    try {
      const value = JSON.parse(window.sessionStorage.getItem(SESSION_KEY) || '{}');
      return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
    } catch {
      return {};
    }
  }

  function writeSessionStore(value) {
    window.sessionStorage.setItem(SESSION_KEY, JSON.stringify(value));
  }

  function accountKey() {
    return String(window.getAuthState?.()?.email || '').trim().toLowerCase();
  }

  function availableLeagues() {
    const leagues = window.getLeaguesForCurrentAccount?.();
    return Array.isArray(leagues) ? leagues : [];
  }

  function containsLeague(leagues, leagueId) {
    const candidate = String(leagueId || '');
    return Boolean(candidate && leagues.some((league) => String(league?.id || '') === candidate));
  }

  function routeLeagueId() {
    try {
      const params = new URLSearchParams(window.location.search || '');
      return params.get(CANONICAL_PARAM) || params.get(LEGACY_PARAM) || '';
    } catch {
      return '';
    }
  }

  function sessionLeagueId(email = accountKey()) {
    if (!email) return '';
    return String(readSessionStore()[email] || '');
  }

  function setSessionLeagueId(leagueId, email = accountKey()) {
    if (!email) return;
    const store = readSessionStore();
    if (leagueId) store[email] = String(leagueId);
    else delete store[email];
    writeSessionStore(store);
  }

  function canonicalLeagueId(leagues = availableLeagues(), fallbackLeague = null) {
    if (!leagues.length) return '';
    const routeId = routeLeagueId();
    if (containsLeague(leagues, routeId)) return routeId;
    const perTabId = sessionLeagueId();
    if (containsLeague(leagues, perTabId)) return perTabId;
    const fallbackId = String(fallbackLeague?.id || '');
    if (containsLeague(leagues, fallbackId)) return fallbackId;
    return String(leagues[0]?.id || '');
  }

  function scopedPage() {
    return SCOPED_PAGES.has(pageName());
  }

  function canonicalHref(leagueId, href = window.location?.href || '') {
    const candidate = String(leagueId || '');
    if (!candidate || !href) return href;
    const url = new URL(href, window.location.href);
    url.searchParams.set(CANONICAL_PARAM, candidate);
    url.searchParams.delete(LEGACY_PARAM);
    return `${url.pathname}${url.search}${url.hash}`;
  }

  function syncRoute(leagueId) {
    if (!scopedPage() || !leagueId || !window.history?.replaceState) return;
    const next = canonicalHref(leagueId);
    const current = `${window.location.pathname}${window.location.search}${window.location.hash}`;
    if (next !== current) {
      window.history.replaceState(window.history.state || {}, document.title, next);
    }
  }

  function decorateLeagueLinks(root = document, leagueId = window.getLeagueState?.()?.id || '') {
    if (!leagueId || !root?.querySelectorAll) return;
    root.querySelectorAll('a[href]').forEach((anchor) => {
      const raw = anchor.getAttribute('href');
      if (!raw || raw.startsWith('#') || /^(mailto:|tel:|javascript:)/i.test(raw)) return;
      let target;
      try {
        target = new URL(raw, window.location.href);
      } catch {
        return;
      }
      if (target.origin !== window.location.origin || !SCOPED_PAGES.has(pageName(target.pathname))) return;
      target.searchParams.set(CANONICAL_PARAM, String(leagueId));
      target.searchParams.delete(LEGACY_PARAM);
      const next = `${target.pathname.split('/').pop()}${target.search}${target.hash}`;
      if (next !== raw) anchor.setAttribute('href', next);
    });
  }

  function install() {
    if (installed) return true;
    if (typeof window.getLeagueState !== 'function'
      || typeof window.getLeaguesForCurrentAccount !== 'function'
      || typeof window.setActiveLeague !== 'function') {
      return false;
    }

    installed = true;
    const originalGetLeagueState = window.getLeagueState;
    const originalSetActiveLeague = window.setActiveLeague;
    const originalSaveLeagueForAccount = window.saveLeagueForAccount;
    const originalReplaceLeagues = window.replaceLeaguesForCurrentAccount;
    const originalRemoveLeague = window.removeLeagueForCurrentAccount;
    const originalClearSessionState = window.clearSessionState;

    window.getLeagueState = function getCanonicalLeagueState() {
      const fallback = originalGetLeagueState();
      const leagues = availableLeagues();
      if (!accountKey() || !leagues.length) return fallback;
      const selectedId = canonicalLeagueId(leagues, fallback);
      const selected = leagues.find((league) => String(league.id) === selectedId) || fallback;
      if (selected?.id) {
        setSessionLeagueId(selected.id);
        syncRoute(selected.id);
      }
      return selected || null;
    };

    window.setActiveLeague = function setCanonicalActiveLeague(leagueId) {
      const leagues = availableLeagues();
      if (!containsLeague(leagues, leagueId)) return false;
      originalSetActiveLeague(leagueId);
      setSessionLeagueId(leagueId);
      syncRoute(leagueId);
      decorateLeagueLinks(document, leagueId);
      window.dispatchEvent?.(new CustomEvent('cff:league-context-changed', {
        detail: { leagueId: String(leagueId) }
      }));
      return true;
    };

    if (typeof originalSaveLeagueForAccount === 'function') {
      window.saveLeagueForAccount = function saveCanonicalLeague(league, options = {}) {
        const result = originalSaveLeagueForAccount(league, options);
        const activate = options === true || options?.activate === true;
        if (result?.ok && activate && result.league?.id) {
          setSessionLeagueId(result.league.id);
          syncRoute(result.league.id);
        }
        return result;
      };
    }

    if (typeof originalReplaceLeagues === 'function') {
      window.replaceLeaguesForCurrentAccount = function replaceCanonicalLeagues(leagues = []) {
        const result = originalReplaceLeagues(leagues);
        const selected = window.getLeagueState();
        if (selected?.id) setSessionLeagueId(selected.id);
        return result;
      };
    }

    if (typeof originalRemoveLeague === 'function') {
      window.removeLeagueForCurrentAccount = function removeCanonicalLeague(leagueId) {
        const removedId = String(leagueId || '');
        const result = originalRemoveLeague(leagueId);
        if (sessionLeagueId() === removedId) setSessionLeagueId('');
        const selected = window.getLeagueState();
        if (selected?.id) syncRoute(selected.id);
        return result;
      };
    }

    window.clearSessionState = function clearCanonicalSessionState() {
      originalClearSessionState?.();
      window.sessionStorage.removeItem(SESSION_KEY);
    };

    const selected = window.getLeagueState();
    decorateLeagueLinks(document, selected?.id || '');

    if (typeof MutationObserver === 'function' && document?.documentElement) {
      const observer = new MutationObserver(() => {
        decorateLeagueLinks(document, window.getLeagueState()?.id || '');
      });
      observer.observe(document.documentElement, {
        childList: true,
        subtree: true,
        attributes: true,
        attributeFilter: ['href']
      });
    }

    window.CFF_LEAGUE_CONTEXT = Object.freeze({
      sessionKey: SESSION_KEY,
      routeParam: CANONICAL_PARAM,
      currentLeagueId: () => window.getLeagueState?.()?.id || '',
      urlFor: (href, leagueId = window.getLeagueState?.()?.id || '') => canonicalHref(leagueId, href),
      decorateLinks: decorateLeagueLinks
    });
    return true;
  }

  if (!install()) {
    let bootstrapObserver = null;
    const timer = window.setInterval(() => {
      if (!install()) return;
      window.clearInterval(timer);
      bootstrapObserver?.disconnect();
    }, 0);

    if (typeof MutationObserver === 'function' && document?.documentElement) {
      bootstrapObserver = new MutationObserver(() => {
        if (!install()) return;
        bootstrapObserver.disconnect();
        window.clearInterval(timer);
      });
      bootstrapObserver.observe(document.documentElement, { childList: true, subtree: true });
    }

    window.addEventListener?.('load', () => {
      if (!install()) return;
      window.clearInterval(timer);
      bootstrapObserver?.disconnect();
    }, { once: true });
  }
})();
