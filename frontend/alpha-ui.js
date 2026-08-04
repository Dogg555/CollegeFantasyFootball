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

  const helpers = { formatAge };
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

  // Page scripts call validateAuthSession during initialization. Route those
  // calls through the same guard so private content and API requests remain
  // blocked until the backend confirms the saved session.
  if (isPrivatePage && originalValidateAuthSession) {
    root.validateAuthSession = async function validateAuthAfterPrivatePageRecovery() {
      return privateGuard;
    };
  }

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
    document.documentElement.classList.add('alpha-ui-ready');
    setupBranding();
    simplifyPageCopy();
    setupLeagueMobileNav();
    setupCollapsibleCards();
    setupDraftStatusDock();
  }

  root.CFF_ALPHA_UI = helpers;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => { void init(); });
  else void init();
})(typeof window !== 'undefined' ? window : globalThis);
