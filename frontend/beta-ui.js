(function initAlphaUi(root) {
  'use strict';

  function formatAge(value) {
    const seconds = Number(value);
    if (!Number.isFinite(seconds) || seconds < 0) return 'unknown';
    if (seconds < 60) return `${Math.round(seconds)} sec ago`;
    if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
    if (seconds < 86400) return `${Math.round(seconds / 3600)} hr ago`;
    return `${Math.round(seconds / 86400)} day${Math.round(seconds / 86400) === 1 ? '' : 's'} ago`;
  }

  function requestMethod(options = {}) {
    return String(options?.method || 'GET').trim().toUpperCase() || 'GET';
  }

  function requestStateMessage(path = '', method = 'GET') {
    const normalizedPath = String(path || '').toLowerCase();
    const normalizedMethod = String(method || 'GET').toUpperCase();
    if (normalizedMethod === 'GET') return 'Latest data loaded.';
    if (normalizedPath.includes('/draft/order')) return 'Draft order saved.';
    if (normalizedPath.includes('/draft/undo')) return 'Last draft pick undone.';
    if (normalizedPath.includes('/draft/reset')) return 'Draft reset completed.';
    if (normalizedPath.includes('/draft')) return 'Draft changes saved.';
    if (normalizedPath.includes('/waiver')) return 'Waiver changes saved.';
    if (normalizedPath.includes('/trade')) return 'Trade changes saved.';
    if (normalizedPath.includes('/feed')) return 'League update posted.';
    if (normalizedPath.includes('/roster')) return 'Roster changes saved.';
    if (normalizedPath.includes('/league')) return 'League changes saved.';
    return 'Changes saved.';
  }

  const emptyStateDefinitions = Object.freeze({
    'league-empty': {
      title: 'No active league',
      body: 'Create or join a league to unlock rosters, matchups, waivers, trades, and the draft room.',
      actionLabel: 'Create league',
      actionHref: 'index.html'
    },
    'league-list': {
      title: 'No leagues yet',
      body: 'Create your first private league or accept an invitation to get started.',
      actionLabel: 'Create league',
      actionHref: 'index.html'
    },
    'team-roster': {
      title: 'Roster empty',
      body: 'Draft or add players to begin building your lineup.',
      actionLabel: 'Browse players',
      actionHref: 'players.html'
    },
    'scoreboard-list': {
      title: 'No matchups yet',
      body: 'Invite managers and generate the season schedule to populate the scoreboard.'
    },
    'standings-list': {
      title: 'No standings yet',
      body: 'Standings appear after managers join and matchups are created.'
    },
    'free-agent-list': {
      title: 'No free agents available',
      body: 'Available players will appear here when the player pool is loaded.'
    },
    'drop-player-list': {
      title: 'No rostered players',
      body: 'There are no players available to drop from the current roster.'
    },
    'waiver-list': {
      title: 'No waiver claims',
      body: 'Submitted waiver claims and their processing status will appear here.'
    },
    'waiver-priority-list': {
      title: 'No waiver order',
      body: 'The priority order appears after league managers are active.'
    },
    'trade-list': {
      title: 'No trade offers',
      body: 'Sent and received trade offers will appear here.'
    },
    'league-feed-list': {
      title: 'No league activity',
      body: 'Transactions, waiver claims, trades, final scores, and commissioner posts will appear here.'
    },
    'transaction-list': {
      title: 'No transactions',
      body: 'Completed roster and league transactions will appear here.'
    },
    'manager-list': {
      title: 'No managers yet',
      body: 'Invite managers to fill the league and prepare the draft order.'
    },
    'draft-queue': {
      title: 'Draft queue empty',
      body: 'Add targets from player search or the recommended board.',
      actionLabel: 'Find players',
      actionHref: 'players.html'
    },
    'roster-list': {
      title: 'No draft picks yet',
      body: 'Players selected during the draft will appear on your roster.'
    },
    'draft-order-list': {
      title: 'Draft order unavailable',
      body: 'The order appears after active managers join the league.'
    },
    'draft-pick-list': {
      title: 'No picks made',
      body: 'Completed draft selections will appear here in pick order.'
    },
    'upcoming-pick-list': {
      title: 'No upcoming picks',
      body: 'Upcoming managers appear after the draft order is set.'
    },
    'recommended-list': {
      title: 'No recommendations available',
      body: 'Every recommended player is already queued or drafted.'
    }
  });

  function emptyStateDefinition(id) {
    const definition = emptyStateDefinitions[String(id || '')];
    return definition ? { ...definition } : null;
  }

  function emptyStateTitle(id, text = '') {
    const normalized = String(text || '').trim().toLowerCase();
    if (normalized.includes('draft complete')) return 'Draft complete';
    return emptyStateDefinitions[String(id || '')]?.title || 'Nothing here yet';
  }

  const helpers = {
    formatAge,
    requestMethod,
    requestStateMessage,
    emptyStateDefinition,
    emptyStateTitle
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  const pageName = root.location.pathname.split('/').pop() || 'index.html';
  const privatePages = new Set(['league.html', 'draft.html']);
  const isPrivatePage = privatePages.has(pageName);
  const originalValidateAuthSession = typeof root.validateAuthSession === 'function'
    ? root.validateAuthSession
    : null;
  const originalValidateAuthSessionResult = typeof root.validateAuthSessionResult === 'function'
    ? root.validateAuthSessionResult
    : null;
  let privatePageRecovery = null;
  let authUnavailablePanel = null;

  function ensureAuthenticatedStateStyles() {
    if (!isPrivatePage || document.querySelector('link[data-cff-authenticated-states]')) return;
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = 'authenticated-states.css';
    link.dataset.cffAuthenticatedStates = 'true';
    document.head.appendChild(link);
  }

  ensureAuthenticatedStateStyles();

  function recoverPrivatePageSession() {
    if (!isPrivatePage || typeof root.CFFAuthSessionSync?.recover !== 'function') {
      return Promise.resolve(null);
    }
    if (!privatePageRecovery) {
      privatePageRecovery = Promise.resolve()
        .then(() => root.CFFAuthSessionSync.recover())
        .catch(() => null);
    }
    return privatePageRecovery;
  }

  if (isPrivatePage) {
    document.documentElement.classList.add('cff-private-pending');
  }

  function currentDestination() {
    return `${pageName}${root.location.search || ''}${root.location.hash || ''}`;
  }

  function redirectToSignIn(reason = 'signin-required') {
    const params = new URLSearchParams({
      next: currentDestination(),
      reason
    });
    root.location.replace(`signin.html?${params.toString()}`);
  }

  function hideAuthUnavailablePanel() {
    authUnavailablePanel?.remove();
    authUnavailablePanel = null;
  }

  function showAuthUnavailablePanel() {
    if (authUnavailablePanel) return authUnavailablePanel;

    const panel = document.createElement('section');
    panel.className = 'cff-auth-gate';
    panel.setAttribute('role', 'alert');
    panel.setAttribute('aria-live', 'assertive');
    Object.assign(panel.style, {
      position: 'fixed',
      inset: '0',
      zIndex: '10000',
      display: 'grid',
      placeItems: 'center',
      padding: '24px',
      background: '#0d1116',
      color: '#f4f7fb',
      fontFamily: 'system-ui, sans-serif'
    });

    const card = document.createElement('div');
    Object.assign(card.style, {
      width: 'min(520px, 100%)',
      padding: '28px',
      border: '1px solid rgba(255, 255, 255, 0.14)',
      borderRadius: '14px',
      background: '#151c24',
      boxShadow: '0 24px 80px rgba(0, 0, 0, 0.45)'
    });
    card.innerHTML = `
      <div style="font-size: 0.78rem; font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; color: #f0b84a;">Session check unavailable</div>
      <h1 style="margin: 10px 0 8px; font-size: clamp(1.5rem, 5vw, 2rem);">We could not verify your session</h1>
      <p style="margin: 0; color: #b9c3cf; line-height: 1.6;">The authentication service is temporarily unreachable. Your saved session has not been cleared, and private league data will remain hidden until verification succeeds.</p>
      <div style="display: flex; flex-wrap: wrap; gap: 10px; margin-top: 22px;">
        <button class="button button--primary" type="button" data-auth-retry>Retry session check</button>
        <a class="button button--ghost" href="signin.html">Go to sign in</a>
      </div>
      <div data-auth-retry-status role="status" style="min-height: 1.4em; margin-top: 12px; color: #b9c3cf;"></div>
    `;
    panel.appendChild(card);
    document.documentElement.appendChild(panel);
    authUnavailablePanel = panel;
    return panel;
  }

  function waitForRetry() {
    const panel = showAuthUnavailablePanel();
    const retryButton = panel.querySelector('[data-auth-retry]');
    const retryStatus = panel.querySelector('[data-auth-retry-status]');

    return new Promise((resolve) => {
      retryButton?.addEventListener('click', () => {
        retryButton.disabled = true;
        retryButton.textContent = 'Checking...';
        if (retryStatus) retryStatus.textContent = 'Checking the authentication service again...';
        resolve();
      }, { once: true });
    });
  }

  async function validatePrivateSession() {
    const auth = typeof root.getAuthState === 'function' ? root.getAuthState() : null;
    if (!auth?.token) {
      return { authenticated: false, unavailable: false, expired: false, missing: true };
    }

    if (originalValidateAuthSessionResult) {
      try {
        return await originalValidateAuthSessionResult();
      } catch (error) {
        return {
          authenticated: false,
          unavailable: true,
          expired: false,
          message: error?.message || 'Session validation failed'
        };
      }
    }

    if (originalValidateAuthSession) {
      try {
        const authenticated = await originalValidateAuthSession();
        return { authenticated, unavailable: false, expired: !authenticated };
      } catch (error) {
        return {
          authenticated: false,
          unavailable: true,
          expired: false,
          message: error?.message || 'Session validation failed'
        };
      }
    }

    return { authenticated: Boolean(auth?.token), unavailable: false, expired: false };
  }

  async function guardPrivatePage() {
    if (!isPrivatePage) return true;

    await recoverPrivatePageSession();

    while (true) {
      const result = await validatePrivateSession();
      if (result.authenticated) {
        hideAuthUnavailablePanel();
        document.documentElement.classList.remove('cff-private-pending');
        return true;
      }

      if (result.unavailable) {
        await waitForRetry();
        hideAuthUnavailablePanel();
        privatePageRecovery = null;
        await recoverPrivatePageSession();
        continue;
      }

      redirectToSignIn(result.expired ? 'session-expired' : 'signin-required');
      return false;
    }
  }

  const privateGuard = guardPrivatePage();
  root.CFFPrivatePageGuard = privateGuard;

  if (isPrivatePage && originalValidateAuthSession) {
    root.validateAuthSession = async function validateAuthAfterPrivatePageRecovery() {
      return privateGuard;
    };
  }

  function escapeStateHtml(value) {
    return String(value || '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#039;');
  }

  function installAuthenticatedStateController() {
    if (!isPrivatePage || root.CFFAsyncStates) return root.CFFAsyncStates || null;

    const state = {
      activeReads: 0,
      activeWrites: 0,
      initialSettled: false,
      readFailed: false,
      settleTimer: null,
      hideTimer: null,
      banner: null,
      scanQueued: false
    };
    const pageLabel = pageName === 'draft.html' ? 'draft room' : 'league';
    const draftTargets = new Set([
      'draft-queue',
      'roster-list',
      'draft-order-list',
      'draft-pick-list',
      'upcoming-pick-list',
      'recommended-list'
    ]);
    const targetIds = Object.keys(emptyStateDefinitions).filter((id) => {
      return pageName === 'draft.html' ? draftTargets.has(id) : !draftTargets.has(id);
    });

    function pageHost() {
      if (pageName === 'draft.html') {
        return document.querySelector('main.layout') || document.getElementById('draft-room-content');
      }
      return document.querySelector('main.league-dashboard') || document.querySelector('main.layout');
    }

    function ensureBanner() {
      if (state.banner?.isConnected) return state.banner;
      const host = pageHost();
      if (!host) return null;
      const banner = document.createElement('section');
      banner.id = 'cff-page-state';
      banner.className = 'cff-page-state cff-page-state--loading';
      banner.setAttribute('role', 'status');
      banner.setAttribute('aria-live', 'polite');
      banner.innerHTML = `
        <div class="cff-page-state__icon" aria-hidden="true"></div>
        <div class="cff-page-state__copy">
          <strong>Loading ${escapeStateHtml(pageLabel)}...</strong>
          <span>Checking the latest server data.</span>
        </div>
        <div class="cff-page-state__actions"></div>
      `;
      const tabs = host.querySelector(':scope > .league-tabs');
      if (tabs) tabs.insertAdjacentElement('afterend', banner);
      else host.prepend(banner);
      state.banner = banner;
      return banner;
    }

    function setBusy(busy) {
      const host = pageHost();
      if (!host) return;
      if (busy) host.setAttribute('aria-busy', 'true');
      else host.removeAttribute('aria-busy');
    }

    function showPageState(kind, title, message, action = null, autoHideMs = 0) {
      clearTimeout(state.hideTimer);
      const banner = ensureBanner();
      if (!banner) return;
      banner.hidden = false;
      banner.className = `cff-page-state cff-page-state--${kind}`;
      banner.setAttribute('role', kind === 'error' ? 'alert' : 'status');
      banner.querySelector('.cff-page-state__copy').innerHTML = `
        <strong>${escapeStateHtml(title)}</strong>
        <span>${escapeStateHtml(message)}</span>
      `;
      const actions = banner.querySelector('.cff-page-state__actions');
      actions.replaceChildren();
      if (action?.label && typeof action.onClick === 'function') {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = `button ${kind === 'error' ? 'button--primary' : 'button--ghost'}`;
        button.textContent = action.label;
        button.addEventListener('click', action.onClick, { once: Boolean(action.once) });
        actions.appendChild(button);
      }
      setBusy(kind === 'loading');
      if (autoHideMs > 0) {
        state.hideTimer = setTimeout(() => {
          banner.hidden = true;
          setBusy(false);
        }, autoHideMs);
      }
    }

    function hidePageState() {
      clearTimeout(state.hideTimer);
      const banner = ensureBanner();
      if (banner) banner.hidden = true;
      setBusy(false);
    }

    function scheduleInitialSettle() {
      clearTimeout(state.settleTimer);
      state.settleTimer = setTimeout(() => {
        if (state.activeReads || state.activeWrites || state.readFailed) return;
        state.initialSettled = true;
        showPageState(
          'success',
          pageName === 'draft.html' ? 'Draft room ready' : 'League ready',
          'The latest available data is displayed.',
          null,
          1800
        );
        queueEmptyStateScan();
      }, 900);
    }

    function beginRead() {
      if (state.activeReads === 0) state.readFailed = false;
      state.activeReads += 1;
      clearTimeout(state.settleTimer);
      if (!state.initialSettled) {
        showPageState('loading', `Loading ${pageLabel}...`, 'Checking the latest server data.');
      }
    }

    function finishRead(error = null) {
      state.activeReads = Math.max(0, state.activeReads - 1);
      if (error) {
        state.readFailed = true;
        state.initialSettled = true;
        showPageState(
          'error',
          `Could not refresh ${pageLabel}`,
          'Showing the last saved data where available. Retry when the service recovers.',
          {
            label: 'Retry page',
            once: true,
            onClick: () => root.location.reload()
          }
        );
      } else if (!state.activeReads && !state.readFailed) {
        scheduleInitialSettle();
      }
    }

    function beginWrite(path) {
      state.activeWrites += 1;
      const label = String(path || '').includes('/draft') ? 'Saving draft changes...' : 'Saving changes...';
      showPageState('loading', label, 'Keep this page open until the server confirms the update.');
    }

    function finishWrite(path, method, error = null) {
      state.activeWrites = Math.max(0, state.activeWrites - 1);
      if (error) {
        showPageState(
          'error',
          'Changes were not saved',
          'The server did not confirm this update. No local success state was recorded.',
          {
            label: 'Dismiss',
            once: true,
            onClick: hidePageState
          }
        );
        return;
      }
      showPageState('success', requestStateMessage(path, method), 'The server confirmed this update.', null, 2400);
      queueEmptyStateScan();
    }

    function renderEmptyState(element, definition) {
      const rawText = element.textContent.trim();
      if (element.querySelector('[data-cff-state-card]')) return;
      if (element.querySelector('.row, table, tbody, [data-state-item]')) {
        element.classList.remove('cff-state-host');
        delete element.dataset.cffState;
        return;
      }
      const body = rawText || definition.body;
      const title = emptyStateTitle(element.id, rawText);
      element.classList.add('cff-state-host');
      element.dataset.cffState = 'empty';
      element.innerHTML = `
        <div class="cff-state cff-state--empty" data-cff-state-card role="status">
          <div class="cff-state__icon" aria-hidden="true">-</div>
          <div class="cff-state__copy">
            <strong>${escapeStateHtml(title)}</strong>
            <span>${escapeStateHtml(body === title ? definition.body : body)}</span>
          </div>
          ${definition.actionLabel && definition.actionHref
            ? `<a class="button button--ghost" href="${escapeStateHtml(definition.actionHref)}">${escapeStateHtml(definition.actionLabel)}</a>`
            : ''}
        </div>
      `;
    }

    function scanEmptyStates() {
      state.scanQueued = false;
      targetIds.forEach((id) => {
        const element = document.getElementById(id);
        const definition = emptyStateDefinitions[id];
        if (!element || !definition) return;
        if (element.querySelector('.row, table, tbody, [data-state-item]')) {
          element.classList.remove('cff-state-host');
          delete element.dataset.cffState;
          return;
        }
        renderEmptyState(element, definition);
      });
    }

    function queueEmptyStateScan() {
      if (state.scanQueued) return;
      state.scanQueued = true;
      Promise.resolve().then(scanEmptyStates);
    }

    const originalApiRequest = typeof root.apiRequest === 'function' ? root.apiRequest : null;
    if (originalApiRequest) {
      root.apiRequest = async function trackedApiRequest(path, options = {}) {
        const method = requestMethod(options);
        const readOnly = method === 'GET';
        if (readOnly) beginRead(path);
        else beginWrite(path);
        try {
          const result = await originalApiRequest(path, options);
          if (readOnly) finishRead();
          else finishWrite(path, method);
          return result;
        } catch (error) {
          if (readOnly) finishRead(error);
          else finishWrite(path, method, error);
          throw error;
        }
      };
    }

    const observer = new MutationObserver(queueEmptyStateScan);
    observer.observe(document.body, { childList: true, subtree: true, characterData: true });
    showPageState('loading', `Loading ${pageLabel}...`, 'Checking the latest server data.');
    scheduleInitialSettle();
    queueEmptyStateScan();

    const controller = {
      show: showPageState,
      hide: hidePageState,
      scanEmptyStates,
      scheduleInitialSettle
    };
    root.CFFAsyncStates = controller;
    return controller;
  }

  const authenticatedStates = installAuthenticatedStateController();

  function setupBranding() {
    if (!document.querySelector('link[rel~="icon"]')) {
      const icon = document.createElement('link');
      icon.rel = 'icon';
      icon.type = 'image/svg+xml';
      icon.href = 'assets/favicon.svg';
      document.head.appendChild(icon);
    }

    if (!document.querySelector('meta[name="theme-color"]')) {
      const theme = document.createElement('meta');
      theme.name = 'theme-color';
      theme.content = '#0d1116';
      document.head.appendChild(theme);
    }

    document.querySelectorAll('.brand__logo').forEach((logo) => {
      logo.setAttribute('aria-hidden', 'true');
      logo.title = 'College Fantasy Football';
    });
  }

  function simplifyPageCopy() {
    const footer = document.querySelector('.footer');
    if (footer) footer.textContent = 'College Fantasy Football';

    if (pageName === 'index.html') {
      const heroPill = document.querySelector('.hero__copy > .pill');
      const heroSubtitle = document.querySelector('.hero__copy > .subtitle');
      const heroHint = document.querySelector('.hero__copy > .muted.small');
      const createCard = document.querySelector('main .card--accent');
      const createCopy = createCard?.querySelector(':scope > p.muted');
      const scheduleHint = document.querySelector('#scoreboard-heading + .muted.small');

      if (heroPill) heroPill.textContent = 'Closed beta opens August 22';
      if (heroSubtitle) heroSubtitle.textContent = 'A full-season fantasy platform built for college football: private leagues, draft rooms, FBS player search, Saturday scoring, waivers, trades, matchups, and commissioner controls.';
      if (heroHint) heroHint.textContent = 'Create a league, invite managers, draft players, set lineups, and follow each week from one hub.';
      if (createCopy) createCopy.textContent = 'Choose the league size, scoring format, draft type, and invite list.';
      if (scheduleHint) scheduleHint.textContent = 'Choose a week to view games grouped by kickoff time.';
    }

    if (pageName === 'players.html') {
      const heroHeading = document.querySelector('.hero__copy h1');
      const heroSubtitle = document.querySelector('.hero__copy .subtitle');
      const helper = document.querySelector('.card > p.muted.small');
      if (heroHeading) heroHeading.textContent = 'Players';
      if (heroSubtitle) heroSubtitle.textContent = 'Search current FBS rosters by player, team, position, or conference.';
      if (helper) helper.textContent = 'Leave the search blank to browse all active players.';
    }
  }

  function setupLeagueMobileNav() {
    const tabs = document.querySelector('.league-tabs');
    if (!tabs || document.querySelector('.league-tab-select')) return;
    const targets = [...tabs.querySelectorAll('.league-tab')];
    if (!targets.length) return;

    const wrap = document.createElement('div');
    wrap.className = 'league-tab-select-wrap';
    const label = document.createElement('label');
    label.className = 'field';
    label.innerHTML = '<span>League section</span>';
    const select = document.createElement('select');
    select.className = 'league-tab-select';
    select.setAttribute('aria-label', 'League section');
    targets.forEach((target, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = target.textContent.trim();
      option.selected = target.classList.contains('is-active');
      select.appendChild(option);
    });
    label.appendChild(select);
    wrap.appendChild(label);
    tabs.insertAdjacentElement('beforebegin', wrap);

    select.addEventListener('change', () => {
      const target = targets[Number(select.value)];
      if (!target) return;
      if (target.tagName === 'A') {
        root.location.href = target.href;
      } else {
        target.click();
      }
    });

    const sync = () => {
      const activeIndex = targets.findIndex((target) => target.classList.contains('is-active'));
      if (activeIndex >= 0) select.value = String(activeIndex);
    };
    targets.forEach((target) => new MutationObserver(sync).observe(target, { attributes: true, attributeFilter: ['class'] }));
  }

  function setupCollapsibleCards() {
    const cards = [...document.querySelectorAll('[data-mobile-collapsible]')];
    cards.forEach((card) => {
      const header = card.querySelector(':scope > .card__header');
      if (!header || header.querySelector('.mobile-card-toggle')) return;
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'button button--ghost mobile-card-toggle';
      button.textContent = 'Show';
      button.setAttribute('aria-expanded', 'false');
      header.appendChild(button);

      const applyDefault = () => {
        if (root.matchMedia('(max-width: 720px)').matches && !card.dataset.mobileExpanded) {
          card.classList.add('is-collapsed');
        } else if (!root.matchMedia('(max-width: 720px)').matches) {
          card.classList.remove('is-collapsed');
        }
        const expanded = !card.classList.contains('is-collapsed');
        button.textContent = expanded ? 'Hide' : 'Show';
        button.setAttribute('aria-expanded', String(expanded));
      };

      button.addEventListener('click', () => {
        card.dataset.mobileExpanded = 'true';
        card.classList.toggle('is-collapsed');
        applyDefault();
      });
      root.addEventListener('resize', applyDefault, { passive: true });
      applyDefault();
    });
  }

  function setupDraftStatusDock() {
    const content = document.getElementById('draft-room-content');
    if (!content || document.getElementById('draft-mobile-status')) return;
    const dock = document.createElement('div');
    dock.id = 'draft-mobile-status';
    dock.className = 'alpha-status-dock';
    dock.setAttribute('aria-live', 'polite');
    dock.innerHTML = '<span class="alpha-status-dock__manager">Manager TBD</span><span class="muted small alpha-status-dock__state">Waiting</span><span class="alpha-status-dock__clock">--</span>';
    content.appendChild(dock);

    const manager = document.getElementById('draft-current-manager');
    const status = document.getElementById('draft-status');
    const clock = document.getElementById('draft-clock');
    const update = () => {
      dock.querySelector('.alpha-status-dock__manager').textContent = manager?.textContent || 'Manager TBD';
      dock.querySelector('.alpha-status-dock__state').textContent = status?.textContent || 'Waiting';
      dock.querySelector('.alpha-status-dock__clock').textContent = clock?.textContent || '--';
    };
    [manager, status, clock].filter(Boolean).forEach((node) => new MutationObserver(update).observe(node, { childList: true, subtree: true }));
    update();
  }

  async function init() {
    const allowed = await privateGuard;
    if (!allowed) return;
    document.documentElement.classList.add('beta-ui-ready');
    setupBranding();
    simplifyPageCopy();
    setupLeagueMobileNav();
    setupCollapsibleCards();
    setupDraftStatusDock();
    authenticatedStates?.scanEmptyStates();
  }

  root.CFF_ALPHA_UI = helpers;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => { void init(); });
  else void init();
})(typeof window !== 'undefined' ? window : globalThis);

(() => {
  'use strict';

  function formatAge(value) {
    const seconds = Number(value);
    if (!Number.isFinite(seconds) || seconds < 0) return 'unknown';
    if (seconds < 60) return `${Math.round(seconds)} sec ago`;
    if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
    if (seconds < 86400) return `${Math.round(seconds / 3600)} hr ago`;
    return `${Math.round(seconds / 86400)} day${Math.round(seconds / 86400) === 1 ? '' : 's'} ago`;
  }

  function requestMethod(options = {}) {
    return String(options?.method || 'GET').trim().toUpperCase() || 'GET';
  }

  function requestStateMessage(path = '', method = 'GET') {
    const normalizedPath = String(path || '').toLowerCase();
    const normalizedMethod = String(method || 'GET').toUpperCase();
    if (normalizedMethod === 'GET') return 'Latest data loaded.';
    if (normalizedPath.includes('/draft/order')) return 'Draft order saved.';
    if (normalizedPath.includes('/draft/undo')) return 'Last draft pick undone.';
    if (normalizedPath.includes('/draft/reset')) return 'Draft reset completed.';
    if (normalizedPath.includes('/draft')) return 'Draft changes saved.';
    if (normalizedPath.includes('/waiver')) return 'Waiver changes saved.';
    if (normalizedPath.includes('/trade')) return 'Trade changes saved.';
    if (normalizedPath.includes('/feed')) return 'League update posted.';
    if (normalizedPath.includes('/roster')) return 'Roster changes saved.';
    if (normalizedPath.includes('/league')) return 'League changes saved.';
    return 'Changes saved.';
  }

  const emptyStateDefinitions = Object.freeze({
    'league-empty': {
      title: 'No active league',
      body: 'Create or join a league to unlock rosters, matchups, waivers, trades, and the draft room.',
      actionLabel: 'Create league',
      actionHref: 'index.html'
    },
    'league-list': {
      title: 'No leagues yet',
      body: 'Create your first private league or accept an invitation to get started.',
      actionLabel: 'Create league',
      actionHref: 'index.html'
    },
    'team-roster': {
      title: 'Roster empty',
      body: 'Draft or add players to begin building your lineup.',
      actionLabel: 'Browse players',
      actionHref: 'players.html'
    },
    'scoreboard-list': {
      title: 'No matchups yet',
      body: 'Invite managers and generate the season schedule to populate the scoreboard.'
    },
    'standings-list': {
      title: 'No standings yet',
      body: 'Standings appear after managers join and matchups are created.'
    },
    'free-agent-list': {
      title: 'No free agents available',
      body: 'Available players will appear here when the player pool is loaded.'
    },
    'drop-player-list': {
      title: 'No rostered players',
      body: 'There are no players available to drop from the current roster.'
    },
    'waiver-list': {
      title: 'No waiver claims',
      body: 'Submitted waiver claims and their processing status will appear here.'
    },
    'waiver-priority-list': {
      title: 'No waiver order',
      body: 'The priority order appears after league managers are active.'
    },
    'trade-list': {
      title: 'No trade offers',
      body: 'Sent and received trade offers will appear here.'
    },
    'league-feed-list': {
      title: 'No league activity',
      body: 'Transactions, waiver claims, trades, final scores, and commissioner posts will appear here.'
    },
    'transaction-list': {
      title: 'No transactions',
      body: 'Completed roster and league transactions will appear here.'
    },
    'manager-list': {
      title: 'No managers yet',
      body: 'Invite managers to fill the league and prepare the draft order.'
    },
    'draft-queue': {
      title: 'Draft queue empty',
      body: 'Add targets from player search or the recommended board.',
      actionLabel: 'Find players',
      actionHref: 'players.html'
    },
    'roster-list': {
      title: 'No draft picks yet',
      body: 'Players selected during the draft will appear on your roster.'
    },
    'draft-order-list': {
      title: 'Draft order unavailable',
      body: 'The order appears after active managers join the league.'
    },
    'draft-pick-list': {
      title: 'No picks made',
      body: 'Completed draft selections will appear here in pick order.'
    },
    'upcoming-pick-list': {
      title: 'No upcoming picks',
      body: 'Upcoming managers appear after the draft order is set.'
    },
    'recommended-list': {
      title: 'No recommendations available',
      body: 'Every recommended player is already queued or drafted.'
    }
  });

  function emptyStateDefinition(id) {
    const definition = emptyStateDefinitions[String(id || '')];
    return definition ? { ...definition } : null;
  }

  function emptyStateTitle(id, text = '') {
    const normalized = String(text || '').trim().toLowerCase();
    if (normalized.includes('draft complete')) return 'Draft complete';
    return emptyStateDefinitions[String(id || '')]?.title || 'Nothing here yet';
  }

  const helpers = {
    formatAge,
    requestMethod,
    requestStateMessage,
    emptyStateDefinition,
    emptyStateTitle
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  const pageName = window.location.pathname.split('/').pop() || 'index.html';
  const pageClass = `page-${pageName.replace(/\.html$/i, '').replace(/[^a-z0-9-]/gi, '-') || 'home'}`;

  function create(tag, className = '', text = '') {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text) element.textContent = text;
    return element;
  }

  function ensureMetaDescription() {
    if (document.querySelector('meta[name="description"]')) return;
    const meta = document.createElement('meta');
    meta.name = 'description';
    meta.content = pageName === 'signup.html'
      ? 'Create an account for the College Fantasy Football closed beta.'
      : 'Create private college fantasy football leagues, draft FBS players, and manage every Saturday.';
    document.head.appendChild(meta);
  }

  function improveSharedBranding() {
    document.body.classList.add('beta-ui', pageClass);
    const subtitle = document.querySelector('.brand__subtitle');
    if (subtitle) subtitle.textContent = 'Closed beta - 2026 season';

    const footerBrand = document.querySelector('.footer__brand span');
    if (footerBrand) footerBrand.textContent = 'Built for college football Saturdays.';
  }

  function addFieldFeedback(input, id, initialText) {
    if (!input || document.getElementById(id)) return null;
    const feedback = create('span', 'field-feedback', initialText);
    feedback.id = id;
    feedback.setAttribute('aria-live', 'polite');
    const describedBy = new Set((input.getAttribute('aria-describedby') || '').split(/\s+/).filter(Boolean));
    describedBy.add(id);
    input.setAttribute('aria-describedby', [...describedBy].join(' '));
    const field = input.closest('.field');
    field?.appendChild(feedback);
    return feedback;
  }

  function normalizeServiceStatus(status) {
    const original = String(status.textContent || '').trim();
    if (!original) return;
    status.title = status.title || original;
    status.classList.add('system-status');
    const hasError = /unreachable|unavailable|degraded|not configured|failed/i.test(original);
    status.classList.toggle('system-status--error', hasError);
    let next = original;
    if (/checking/i.test(original)) {
      next = 'Checking account services...';
    } else if (/auth ready|authentication is ready|account services are ready/i.test(original)) {
      next = 'Account services are ready.';
      status.classList.remove('system-status--error');
    } else if (/email.*not configured|verification.*not configured/i.test(original)) {
      next = 'Email verification is temporarily unavailable. Account services are being configured.';
    } else if (/unreachable|unavailable|degraded|failed|need attention/i.test(original)) {
      next = 'Account services need attention. Please try again shortly.';
    }
    if (status.textContent !== next) status.textContent = next;
  }

  function watchServiceStatus(status) {
    if (!status || status.dataset.betaObserved === 'true') return;
    status.dataset.betaObserved = 'true';
    normalizeServiceStatus(status);
    new MutationObserver(() => normalizeServiceStatus(status))
      .observe(status, { childList: true, characterData: true, subtree: true });
  }

  function enhanceSignup() {
    const form = document.getElementById('signup-form');
    const email = document.getElementById('signup-email');
    const password = document.getElementById('signup-password');
    if (!form || !email || !password || form.dataset.betaEnhanced === 'true') return;
    form.dataset.betaEnhanced = 'true';

    const card = form.closest('.auth-card');
    const header = card?.querySelector('.card__header');
    const heading = header?.querySelector('h2');
    const pill = header?.querySelector('.pill');
    if (heading) heading.textContent = 'Create your account';
    if (pill) pill.textContent = 'Closed beta';

    if (header && !card.querySelector('.auth-intro')) {
      const intro = create('div', 'auth-intro');
      intro.innerHTML = '<span class="auth-kicker">Your league starts here</span><p>Use one secure account for league invites, drafts, rosters, waivers, trades, and weekly scoring.</p>';
      header.insertAdjacentElement('afterend', intro);
    }

    let confirm = document.getElementById('signup-password-confirm');
    if (!confirm) {
      const confirmField = create('label', 'field');
      confirmField.innerHTML = '<span>Confirm password</span><input id="signup-password-confirm" type="password" name="passwordConfirm" placeholder="Re-enter your password" required autocomplete="new-password" aria-describedby="signup-confirm-feedback" />';
      password.closest('.field')?.insertAdjacentElement('afterend', confirmField);
      confirm = confirmField.querySelector('input');
    }

    confirm.minLength = password.minLength;
    confirm.maxLength = password.maxLength;

    const emailFeedback = addFieldFeedback(email, 'signup-email-feedback', 'Use the email where you want league invites and verification links.');
    const passwordFeedback = addFieldFeedback(password, 'signup-password-feedback', 'Use at least 12 characters. A phrase is easier to remember and harder to guess.');
    const confirmFeedback = document.getElementById('signup-confirm-feedback')
      || addFieldFeedback(confirm, 'signup-confirm-feedback', 'Passwords must match exactly.');

    const verificationNote = create('div', 'auth-notice');
    verificationNote.innerHTML = '<strong>Email verification required</strong><span>After creating the account, open the verification link sent to your inbox before signing in.</span>';
    form.querySelector('.actions')?.insertAdjacentElement('beforebegin', verificationNote);

    const terms = create('p', 'terms-copy');
    terms.innerHTML = 'By creating an account, you agree to the <a href="terms.html">Terms</a> and acknowledge the <a href="privacy.html">Privacy Policy</a>.';
    form.querySelector('.actions')?.insertAdjacentElement('afterend', terms);

    const submit = form.querySelector('[type="submit"]');
    if (submit) {
      submit.classList.add('auth-submit');
      submit.textContent = 'Create account';
    }

    const validateEmail = () => {
      const hasValue = Boolean(email.value.trim());
      const valid = !hasValue || email.validity.valid;
      email.classList.toggle('is-valid', hasValue && valid);
      emailFeedback?.classList.toggle('is-success', hasValue && valid);
      emailFeedback?.classList.toggle('is-error', hasValue && !valid);
      if (emailFeedback) {
        emailFeedback.textContent = !hasValue
          ? 'Use the email where you want league invites and verification links.'
          : valid ? 'Email format looks good.' : 'Enter a complete email address.';
      }
    };

    const validatePasswords = () => {
      const value = password.value;
      const min = Number(password.minLength || 12);
      confirm.minLength = password.minLength;
      confirm.maxLength = password.maxLength;
      const lengthOkay = value.length >= min;
      password.classList.toggle('is-valid', lengthOkay);
      if (passwordFeedback) {
        passwordFeedback.classList.toggle('is-success', lengthOkay);
        passwordFeedback.classList.toggle('is-error', Boolean(value) && !lengthOkay);
        passwordFeedback.textContent = !value
          ? `Use at least ${min} characters. A phrase is easier to remember and harder to guess.`
          : lengthOkay ? 'Password length meets the requirement.' : `${Math.max(0, min - value.length)} more character${min - value.length === 1 ? '' : 's'} required.`;
      }

      const mismatch = Boolean(confirm.value) && confirm.value !== value;
      confirm.setCustomValidity(mismatch ? 'Passwords do not match.' : '');
      confirm.classList.toggle('is-valid', Boolean(confirm.value) && !mismatch);
      confirmFeedback?.classList.toggle('is-success', Boolean(confirm.value) && !mismatch);
      confirmFeedback?.classList.toggle('is-error', mismatch);
      if (confirmFeedback) {
        confirmFeedback.textContent = !confirm.value
          ? 'Passwords must match exactly.'
          : mismatch ? 'Passwords do not match yet.' : 'Passwords match.';
      }
    };

    email.addEventListener('input', validateEmail);
    email.addEventListener('blur', validateEmail);
    password.addEventListener('input', validatePasswords);
    confirm.addEventListener('input', validatePasswords);
    validateEmail();
    validatePasswords();

    document.addEventListener('DOMContentLoaded', () => {
      const wrapper = confirm.closest('.password-field');
      const meter = wrapper?.nextElementSibling;
      if (meter?.classList.contains('password-meter')) meter.remove();
    }, { once: true });

    const sidecar = document.querySelector('.auth-sidecar');
    if (sidecar && !sidecar.querySelector('.auth-benefits')) {
      const sideHeading = sidecar.querySelector('h2');
      const sideCopy = sidecar.querySelector('p');
      if (sideHeading) sideHeading.textContent = 'Ready for kickoff';
      if (sideCopy) sideCopy.textContent = 'Your account keeps every private league, roster move, and draft pick tied to one manager profile.';
      const benefits = create('div', 'auth-benefits');
      benefits.innerHTML = [
        ['01', 'Join private leagues', 'Open invite links and request commissioner approval.'],
        ['02', 'Draft from one board', 'Queue FBS players and follow the live snake draft.'],
        ['03', 'Manage every week', 'Set lineups, submit waivers, trade, and track scoring.']
      ].map(([number, title, copy]) => `<div class="auth-benefit"><span>${number}</span><div><strong>${title}</strong><p>${copy}</p></div></div>`).join('');
      sideCopy?.insertAdjacentElement('afterend', benefits);
    }

    watchServiceStatus(document.getElementById('auth-api-status'));
    window.setTimeout(() => email.focus({ preventScroll: true }), 0);
  }

  function enhanceSignin() {
    const form = document.getElementById('login-form');
    if (!form || form.dataset.betaEnhanced === 'true') return;
    form.dataset.betaEnhanced = 'true';
    const card = form.closest('.auth-card');
    const heading = card?.querySelector('h2');
    if (heading) heading.textContent = 'Welcome back';
    if (card && !card.querySelector('.auth-intro')) {
      const intro = create('div', 'auth-intro');
      intro.innerHTML = '<span class="auth-kicker">Saturday starts here</span><p>Sign in to continue to your leagues, draft rooms, rosters, and matchups.</p>';
      card.querySelector('.card__header')?.insertAdjacentElement('afterend', intro);
    }
    form.querySelector('[type="submit"]')?.classList.add('auth-submit');
    watchServiceStatus(document.getElementById('auth-api-status'));
  }

  function groupLeagueTabs() {
    const tabs = [...document.querySelectorAll('.league-tab')];
    if (!tabs.length) return;
    const groups = {
      overview: 'League', scoreboard: 'League', standings: 'League', activity: 'League',
      team: 'Team', freeagency: 'Team', waivers: 'Team', trades: 'Team',
      managers: 'Manage', settings: 'Manage'
    };
    let previous = '';
    tabs.forEach((tab) => {
      const key = tab.dataset.leagueTab || (tab.tagName === 'A' ? 'draft' : '');
      const group = groups[key] || (key === 'draft' ? 'Draft' : 'League');
      tab.dataset.betaGroup = group.toLowerCase();
      tab.title = `${group}: ${tab.textContent.trim()}`;
      tab.setAttribute('aria-label', `${group}: ${tab.textContent.trim()}`);
      if (group !== previous) tab.classList.add('is-group-start');
      previous = group;
    });
  }

  function boot() {
    ensureMetaDescription();
    improveSharedBranding();
    document.documentElement.classList.add('beta-ui-ready');
    if (pageName === 'signup.html') enhanceSignup();
    if (pageName === 'signin.html') enhanceSignin();
    if (pageName === 'league.html') groupLeagueTabs();
  }

  if (document.body) boot();
  else document.addEventListener('DOMContentLoaded', boot, { once: true });
})();

