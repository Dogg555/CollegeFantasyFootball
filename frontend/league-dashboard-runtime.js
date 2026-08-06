(function initLeagueDashboardRuntime(root) {
  'use strict';
  const helpers = root.CFFLeagueDashboard;
  if (!helpers || typeof document === 'undefined') return;
  if (!/(?:^|\/)league\.html$/.test(root.location?.pathname || '')) return;
  const {
    dashboardViewModel,
    saveCache,
    loadCache,
    clearCache,
    cacheScope,
    isAuthorizationFailure,
    formatDate,
    readStore,
    writeStore,
    VALIDATED_KEY
  } = helpers;

  function escapeHtml(value) {
    return String(value ?? '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#039;');
  }

  function activeTabName(explicit = '') {
    if (explicit) return String(explicit).replace(/^#/, '') || 'overview';
    const active = document.querySelector('[data-league-tab].is-active');
    return active?.dataset?.leagueTab
      || String(root.location?.hash || '').replace(/^#/, '')
      || 'overview';
  }

  function syncHubVisibility(explicitTab = '') {
    const hub = document.getElementById('league-command-center');
    if (hub) hub.hidden = activeTabName(explicitTab) !== 'overview';
  }

  function ensureHub() {
    let hub = document.getElementById('league-command-center');
    if (hub) {
      syncHubVisibility();
      return hub;
    }
    const tabs = document.querySelector('.league-tabs');
    if (!tabs) return null;
    hub = document.createElement('section');
    hub.id = 'league-command-center';
    hub.className = 'league-command-center';
    hub.dataset.leaguePanel = 'overview';
    hub.innerHTML = `
      <div class="dashboard-state" id="league-dashboard-state" role="status">Loading league dashboard…</div>
      <article class="card card--accent dashboard-next-action" id="league-dashboard-next">
        <div>
          <span class="label">Next action</span>
          <h2 id="league-dashboard-next-label">Loading…</h2>
          <p class="muted" id="league-dashboard-next-detail"></p>
        </div>
        <a class="button button--primary" id="league-dashboard-next-link" href="league.html">Open</a>
      </article>
      <div class="dashboard-summary-grid">
        <article class="card dashboard-summary"><span class="label">Current matchup</span><strong id="dashboard-matchup-title">Loading…</strong><span class="muted small" id="dashboard-matchup-detail"></span></article>
        <article class="card dashboard-summary"><span class="label">Lineup</span><strong id="dashboard-lineup-title">Loading…</strong><span class="muted small" id="dashboard-lineup-detail"></span></article>
        <article class="card dashboard-summary"><span class="label">Pending actions</span><strong id="dashboard-pending-title">Loading…</strong><span class="muted small" id="dashboard-pending-detail"></span></article>
        <article class="card dashboard-summary"><span class="label">Standings</span><strong id="dashboard-standings-title">Loading…</strong><span class="muted small" id="dashboard-standings-detail"></span></article>
      </div>
      <div class="dashboard-detail-grid">
        <article class="card"><div class="card__header"><h2>Deadlines</h2><span class="pill pill--muted" id="dashboard-deadline-count">0</span></div><div class="list" id="dashboard-deadlines"></div></article>
        <article class="card"><div class="card__header"><h2>Notices</h2><span class="pill pill--muted" id="dashboard-notice-count">0</span></div><div class="list" id="dashboard-notices"></div></article>
        <article class="card dashboard-activity-card"><div class="card__header"><h2>Recent activity</h2><span class="pill pill--muted" id="dashboard-activity-count">0</span></div><div class="list" id="dashboard-activity"></div></article>
      </div>`;
    tabs.insertAdjacentElement('afterend', hub);
    syncHubVisibility();
    return hub;
  }

  function setText(id, value) {
    const element = document.getElementById(id);
    if (element) element.textContent = value || '';
  }

  function renderRows(id, rows, emptyMessage, render) {
    const element = document.getElementById(id);
    if (!element) return;
    element.innerHTML = rows.length ? rows.map(render).join('') : `<div class="muted small dashboard-empty">${escapeHtml(emptyMessage)}</div>`;
  }

  function resetDashboardContent(message, state = 'loading') {
    const hub = ensureHub();
    if (!hub) return;
    hub.dataset.state = state;
    setText('league-dashboard-state', message);
    setText('league-dashboard-next-label', state === 'loading' ? 'Loading…' : 'Dashboard unavailable');
    setText('league-dashboard-next-detail', '');
    const nextLink = document.getElementById('league-dashboard-next-link');
    if (nextLink) nextLink.href = 'league.html';
    ['dashboard-matchup-title', 'dashboard-lineup-title', 'dashboard-pending-title', 'dashboard-standings-title']
      .forEach((id) => setText(id, state === 'loading' ? 'Loading…' : 'Unavailable'));
    ['dashboard-matchup-detail', 'dashboard-lineup-detail', 'dashboard-pending-detail', 'dashboard-standings-detail']
      .forEach((id) => setText(id, ''));
    ['dashboard-deadline-count', 'dashboard-notice-count', 'dashboard-activity-count']
      .forEach((id) => setText(id, '0'));
    ['dashboard-deadlines', 'dashboard-notices', 'dashboard-activity'].forEach((id) => {
      const element = document.getElementById(id);
      if (element) element.innerHTML = '';
    });
  }

  function renderDashboard(payload) {
    const hub = ensureHub();
    if (!hub) return;
    const view = dashboardViewModel(payload);
    hub.dataset.state = view.dashboard.readOnly ? 'stale' : view.dashboard.freshness.partial ? 'partial' : 'ready';
    setText('league-dashboard-state', view.freshness);
    setText('league-dashboard-next-label', view.nextAction.label);
    setText('league-dashboard-next-detail', view.nextAction.detail);
    const nextLink = document.getElementById('league-dashboard-next-link');
    if (nextLink) nextLink.href = view.nextAction.href;
    setText('dashboard-matchup-title', view.matchup.title);
    setText('dashboard-matchup-detail', view.matchup.detail);
    setText('dashboard-lineup-title', view.lineup.title);
    setText('dashboard-lineup-detail', view.lineup.detail);
    setText('dashboard-pending-title', view.pending.title);
    setText('dashboard-pending-detail', view.pending.detail);
    setText('dashboard-standings-title', view.standings.title);
    setText('dashboard-standings-detail', view.standings.detail);
    setText('dashboard-deadline-count', String(view.deadlines.length));
    setText('dashboard-notice-count', String(view.notices.length));
    setText('dashboard-activity-count', String(view.activity.length));

    renderRows('dashboard-deadlines', view.deadlines, 'No upcoming deadlines.', (item) => `
      <div class="row dashboard-row"><div><strong>${escapeHtml(item.label || item.type || 'Deadline')}</strong><div class="muted small">${escapeHtml(formatDate(item.at))}</div></div></div>`);
    renderRows('dashboard-notices', view.notices, 'No commissioner notices.', (item) => `
      <div class="row dashboard-row"><div><strong>${escapeHtml(item.type === 'pending_members' ? 'Manager action' : 'League notice')}</strong><div class="muted small">${escapeHtml(item.message || '')}</div></div>${item.href ? `<a class="button button--ghost" href="${escapeHtml(item.href)}">Review</a>` : ''}</div>`);
    renderRows('dashboard-activity', view.activity, 'No recent league activity.', (item) => `
      <div class="row dashboard-row"><div><strong>${escapeHtml(item.summary || 'League activity')}</strong><div class="muted small">${escapeHtml(item.actor || 'League')} ${item.createdAt ? `· ${escapeHtml(formatDate(item.createdAt))}` : ''}</div></div></div>`);
  }

  function renderUnavailable(message) {
    resetDashboardContent(message || 'Dashboard data is temporarily unavailable.', 'unavailable');
  }

  function validatedScope(email, leagueId) {
    const values = readStore(root.sessionStorage, VALIDATED_KEY, {});
    return values[cacheScope(email, leagueId)] === true;
  }

  function markValidated(email, leagueId) {
    const values = readStore(root.sessionStorage, VALIDATED_KEY, {});
    values[cacheScope(email, leagueId)] = true;
    writeStore(root.sessionStorage, VALIDATED_KEY, values);
  }

  function clearValidated(email, leagueId) {
    const values = readStore(root.sessionStorage, VALIDATED_KEY, {});
    delete values[cacheScope(email, leagueId)];
    writeStore(root.sessionStorage, VALIDATED_KEY, values);
  }

  function invalidateScope(email, leagueId) {
    clearValidated(email, leagueId);
    clearCache(root.localStorage, email, leagueId);
  }

  function liveScope() {
    const auth = root.getAuthState?.();
    const league = root.getLeagueState?.();
    if (!auth?.email || !league?.id) return '';
    return cacheScope(auth.email, league.id);
  }

  const pendingRequests = new Map();
  let requestGeneration = 0;

  async function refreshDashboard({ allowCached = true } = {}) {
    const auth = root.getAuthState?.();
    const league = root.getLeagueState?.();
    if (!auth?.token || !auth?.email || !league?.id || String(auth.token).startsWith('local-demo-')) return null;
    const scope = cacheScope(auth.email, league.id);
    if (pendingRequests.has(scope)) return pendingRequests.get(scope);
    const generation = ++requestGeneration;

    const requestPromise = (async () => {
      const validation = typeof root.validateAuthSessionResult === 'function'
        ? await root.validateAuthSessionResult()
        : { authenticated: false, unavailable: true };
      if (generation !== requestGeneration || liveScope() !== scope) return null;
      if (!validation?.authenticated) {
        if (!validation?.unavailable) invalidateScope(auth.email, league.id);
        if (validation?.unavailable && allowCached && validatedScope(auth.email, league.id)) {
          const cached = loadCache(root.localStorage, auth.email, league.id);
          if (cached) {
            renderDashboard(cached);
            return cached;
          }
        }
        renderUnavailable(validation?.unavailable
          ? 'Authentication could not be verified. Protected dashboard details remain hidden.'
          : 'Sign in again to view this league dashboard.');
        return null;
      }

      try {
        const payload = await root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/dashboard`, {
          timeoutMs: 12000
        });
        if (generation !== requestGeneration || liveScope() !== scope) return null;
        if (String(payload?.leagueId || '') !== String(league.id)) {
          const mismatch = new Error('The dashboard response did not match the active league.');
          mismatch.status = 409;
          throw mismatch;
        }
        markValidated(auth.email, league.id);
        saveCache(root.localStorage, auth.email, league.id, payload);
        renderDashboard(payload);
        return payload;
      } catch (error) {
        if (generation !== requestGeneration || liveScope() !== scope) return null;
        if (isAuthorizationFailure(error)) {
          invalidateScope(auth.email, league.id);
          renderUnavailable('League access could not be confirmed. Cached dashboard details were cleared.');
          return null;
        }
        const cached = allowCached && validatedScope(auth.email, league.id)
          ? loadCache(root.localStorage, auth.email, league.id)
          : null;
        if (cached) {
          renderDashboard(cached);
          return cached;
        }
        renderUnavailable(root.CFFApiClient?.normalizedUserMessage?.(error, 'Dashboard data is temporarily unavailable.')
          || error?.message);
        return null;
      }
    })().finally(() => {
      if (pendingRequests.get(scope) === requestPromise) pendingRequests.delete(scope);
    });
    pendingRequests.set(scope, requestPromise);
    return requestPromise;
  }

  function installActiveLeagueRefresh() {
    const original = root.setActiveLeague;
    if (typeof original !== 'function' || original.__cffDashboardWrapped) return;
    function setActiveLeagueWithDashboardRefresh(...args) {
      const before = root.getLeagueState?.()?.id || '';
      const result = original.apply(this, args);
      const after = root.getLeagueState?.()?.id || '';
      if (after !== before) {
        requestGeneration += 1;
        resetDashboardContent('Loading the selected league dashboard…', 'loading');
        root.setTimeout(() => refreshDashboard({ allowCached: false }), 0);
      }
      return result;
    }
    Object.defineProperty(setActiveLeagueWithDashboardRefresh, '__cffDashboardWrapped', { value: true });
    root.setActiveLeague = setActiveLeagueWithDashboardRefresh;
  }

  function initialize(attempt = 0) {
    ensureHub();
    if (typeof root.getLeagueState !== 'function'
        || typeof root.getAuthState !== 'function'
        || typeof root.apiRequest !== 'function'
        || typeof root.validateAuthSessionResult !== 'function'
        || typeof root.setActiveLeague !== 'function') {
      if (attempt < 200) root.setTimeout(() => initialize(attempt + 1), 25);
      return;
    }
    installActiveLeagueRefresh();
    syncHubVisibility();
    refreshDashboard();
  }

  document.addEventListener('click', (event) => {
    const tab = event.target?.closest?.('[data-league-tab]');
    if (!tab) return;
    root.setTimeout(() => syncHubVisibility(tab.dataset?.leagueTab || ''), 0);
  });
  root.addEventListener('online', () => refreshDashboard({ allowCached: false }));
  root.addEventListener('focus', () => refreshDashboard());
  root.addEventListener('hashchange', () => {
    syncHubVisibility();
    if (activeTabName() === 'overview') refreshDashboard();
  });
  root.addEventListener('storage', (event) => {
    if (event.key === 'cff_data_revision') refreshDashboard();
  });
  root.refreshLeagueDashboard = refreshDashboard;
  root.syncLeagueDashboardVisibility = syncHubVisibility;
  root.setTimeout(initialize, 0);
})(typeof window !== 'undefined' ? window : globalThis);
