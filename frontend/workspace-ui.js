(() => {
  'use strict';

  const pageName = window.location.pathname.split('/').pop() || 'index.html';
  const emailPattern = /^[^\s@]+@[^\s@]+\.[^\s@]+$/i;
  let refreshScheduled = false;

  function escapeText(value) {
    return String(value ?? '')
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#039;');
  }

  function waitForApp(callback, attempts = 80) {
    if (typeof window.getLeagueState === 'function'
        && typeof window.isCurrentCommissioner === 'function') {
      callback();
      return;
    }
    if (attempts <= 0) return;
    window.setTimeout(() => waitForApp(callback, attempts - 1), 50);
  }

  function setupDraftRoomMenu() {
    if (pageName !== 'draft.html') return;

    const legacyMenu = document.querySelector('main.layout > .league-tabs');
    if (!legacyMenu || legacyMenu.dataset.draftMenuReady === 'true') return;

    const menuShell = legacyMenu.closest('main.layout');
    const roomContent = document.getElementById('draft-room-content');
    const lockedPanel = document.getElementById('draft-locked');
    const hero = roomContent?.querySelector('.hero');
    const queueCard = document.querySelector('.draft-queue-card');
    const rosterCard = document.querySelector('.draft-roster-card');
    const orderCard = Array.from(document.querySelectorAll('.draft-dashboard .card'))
      .find((card) => /draft order/i.test(card.querySelector('h2')?.textContent || ''));
    const picksCard = Array.from(document.querySelectorAll('.draft-dashboard .card'))
      .find((card) => /draft picks/i.test(card.querySelector('h2')?.textContent || ''));

    if (menuShell) {
      menuShell.classList.add('draft-room-menu-shell');
      menuShell.removeAttribute('style');
    }

    if (hero) hero.id = hero.id || 'draft-overview';
    if (queueCard) queueCard.id = queueCard.id || 'draft-queue-section';
    if (rosterCard) rosterCard.id = rosterCard.id || 'draft-roster-section';
    if (orderCard) orderCard.id = orderCard.id || 'draft-order-section';
    if (picksCard) picksCard.id = picksCard.id || 'draft-picks-section';

    legacyMenu.dataset.draftMenuReady = 'true';
    legacyMenu.className = 'draft-room-menu';
    legacyMenu.setAttribute('role', 'navigation');
    legacyMenu.setAttribute('aria-label', 'Draft room navigation');
    legacyMenu.innerHTML = `
      <div class="draft-room-menu__identity">
        <span class="draft-room-menu__eyebrow">Draft room</span>
        <strong class="draft-room-menu__status" id="draft-menu-status">Lobby</strong>
      </div>
      <div class="draft-room-menu__links">
        <a class="draft-room-menu__link is-active" data-draft-nav="overview" href="#draft-overview">Overview</a>
        <a class="draft-room-menu__link" data-draft-nav="queue" data-draft-section href="#draft-queue-section">Queue</a>
        <a class="draft-room-menu__link" data-draft-nav="roster" data-draft-section href="#draft-roster-section">My Roster</a>
        <a class="draft-room-menu__link" data-draft-nav="order" data-draft-section href="#draft-order-section">Draft Order</a>
        <a class="draft-room-menu__link" data-draft-nav="picks" data-draft-section href="#draft-picks-section">Pick Log</a>
      </div>
      <div class="draft-room-menu__actions">
        <a class="draft-room-menu__link draft-room-menu__link--players" href="players.html">Player Pool</a>
        <a class="draft-room-menu__link draft-room-menu__link--league" href="league.html">League Home</a>
      </div>
    `;

    const sectionLinks = [...legacyMenu.querySelectorAll('[data-draft-section]')];
    const overviewLink = legacyMenu.querySelector('[data-draft-nav="overview"]');
    const status = document.getElementById('draft-menu-status');

    function setActive(key) {
      legacyMenu.querySelectorAll('[data-draft-nav]').forEach((link) => {
        const active = link.dataset.draftNav === key;
        link.classList.toggle('is-active', active);
        if (active) link.setAttribute('aria-current', 'page');
        else link.removeAttribute('aria-current');
      });
    }

    function roomIsOpen() {
      return Boolean(roomContent && !roomContent.hidden);
    }

    function updateLockState() {
      const open = roomIsOpen();
      legacyMenu.classList.toggle('is-locked', !open);
      if (status) status.textContent = open ? 'Live workspace' : 'Lobby closed';

      if (overviewLink) {
        overviewLink.textContent = open ? 'Overview' : 'Lobby';
        overviewLink.href = open ? '#draft-overview' : '#draft-locked';
        overviewLink.dataset.draftNav = 'overview';
      }

      sectionLinks.forEach((link) => {
        link.classList.toggle('is-disabled', !open);
        link.setAttribute('aria-disabled', String(!open));
        if (!open) link.setAttribute('tabindex', '-1');
        else link.removeAttribute('tabindex');
      });

      if (!open) setActive('overview');
    }

    legacyMenu.addEventListener('click', (event) => {
      const link = event.target.closest('[data-draft-nav]');
      if (!link) return;
      if (link.getAttribute('aria-disabled') === 'true') {
        event.preventDefault();
        return;
      }
      setActive(link.dataset.draftNav);
    });

    sectionLinks.forEach((link) => {
      link.addEventListener('click', () => setActive(link.dataset.draftNav));
    });

    if ('IntersectionObserver' in window) {
      const observed = [
        ['overview', hero],
        ['queue', queueCard],
        ['roster', rosterCard],
        ['order', orderCard],
        ['picks', picksCard]
      ].filter(([, node]) => node);

      const observer = new IntersectionObserver((entries) => {
        if (!roomIsOpen()) return;
        const visible = entries
          .filter((entry) => entry.isIntersecting)
          .sort((left, right) => right.intersectionRatio - left.intersectionRatio)[0];
        if (!visible) return;
        const match = observed.find(([, node]) => node === visible.target);
        if (match) setActive(match[0]);
      }, { rootMargin: '-25% 0px -60% 0px', threshold: [0.15, 0.35, 0.6] });

      observed.forEach(([, node]) => observer.observe(node));
    }

    if (roomContent) {
      new MutationObserver(updateLockState)
        .observe(roomContent, { attributes: true, attributeFilter: ['hidden'] });
    }
    if (lockedPanel) {
      new MutationObserver(updateLockState)
        .observe(lockedPanel, { attributes: true, attributeFilter: ['hidden'] });
    }

    updateLockState();
  }

  function safeMemberName(member, index) {
    const teamName = String(member?.teamName || '').trim();
    if (teamName) return teamName;
    return member?.role === 'commissioner' ? 'League Commissioner' : `Manager ${index + 1}`;
  }

  function memberStatusCopy(status) {
    const normalized = String(status || 'Invited').toLowerCase();
    if (normalized === 'active') return 'Active';
    if (normalized === 'pending') return 'Pending approval';
    if (normalized === 'removed') return 'Removed';
    return 'Invited';
  }

  function setupLeagueMemberPrivacy() {
    if (pageName !== 'league.html') return;

    const dashboard = document.querySelector('.league-dashboard');
    if (!dashboard) return;

    function leagueState() {
      return window.getLeagueState?.() || null;
    }

    function canManage() {
      const league = leagueState();
      return Boolean(league && window.isCurrentCommissioner?.(league));
    }

    function currentTab() {
      return document.querySelector('[data-league-tab].is-active')?.dataset.leagueTab || 'overview';
    }

    function ensureActivityMemberCard() {
      let card = document.getElementById('activity-member-card');
      if (card) return card;

      card = document.createElement('section');
      card.id = 'activity-member-card';
      card.className = 'card activity-member-card';
      card.dataset.leaguePanel = 'activity';
      card.innerHTML = `
        <div class="card__header">
          <div>
            <h2>League Members</h2>
            <div class="muted small">Team names, roles, and league status.</div>
          </div>
          <span class="pill pill--muted" id="activity-member-count">0</span>
        </div>
        <div id="activity-member-list" class="list">No league members yet.</div>
      `;

      const recentActivity = document.getElementById('transaction-list')?.closest('[data-league-panel="activity"]');
      const feedCard = document.getElementById('league-feed-list')?.closest('[data-league-panel="activity"]');
      if (recentActivity) recentActivity.insertAdjacentElement('beforebegin', card);
      else if (feedCard) feedCard.insertAdjacentElement('afterend', card);
      else dashboard.appendChild(card);
      return card;
    }

    function renderActivityMembers() {
      const card = ensureActivityMemberCard();
      const list = card.querySelector('#activity-member-list');
      const count = card.querySelector('#activity-member-count');
      const league = leagueState();
      const members = Array.isArray(league?.members) ? league.members : [];
      const commissioner = canManage();
      const signature = JSON.stringify({
        commissioner,
        active: currentTab(),
        members: members.map((member) => [
          member.email || '',
          member.teamName || '',
          member.role || '',
          member.status || ''
        ])
      });

      card.hidden = currentTab() !== 'activity';
      if (count) count.textContent = String(members.length);
      if (!list || list.dataset.memberSignature === signature) return;
      list.dataset.memberSignature = signature;

      if (!members.length) {
        list.textContent = 'No league members yet.';
        return;
      }

      list.innerHTML = members.map((member, index) => {
        const statusCopy = memberStatusCopy(member.status);
        const isCurrent = String(member.email || '').toLowerCase()
          === String(window.getAuthState?.()?.email || '').toLowerCase();
        const email = commissioner && member.email
          ? `<div class="muted activity-member-card__email">${escapeText(member.email)}</div>`
          : '';
        return `
          <div class="row activity-member-row">
            <div>
              <strong>${escapeText(safeMemberName(member, index))}${isCurrent ? ' <span class="activity-member-you">(You)</span>' : ''}</strong>
              ${email}
              <div class="muted small">${member.role === 'commissioner' ? 'Commissioner' : 'Member'} · ${escapeText(statusCopy)}</div>
            </div>
            <span class="pill ${String(member.status || '').toLowerCase() === 'pending' ? '' : 'pill--muted'}">${escapeText(statusCopy)}</span>
          </div>
        `;
      }).join('');
    }

    function updateMemberManagementAccess() {
      const commissioner = canManage();
      const managersTab = document.querySelector('[data-league-tab="managers"]');
      const managersPanel = document.querySelector('[data-league-panel="managers"]');
      if (managersTab) {
        managersTab.hidden = !commissioner;
        managersTab.setAttribute('aria-hidden', String(!commissioner));
      }
      if (managersPanel && !commissioner) managersPanel.hidden = true;

      const select = document.querySelector('.league-tab-select');
      if (select) {
        Array.from(select.options).forEach((option) => {
          const isMembers = /members/i.test(option.textContent || '');
          if (!isMembers) return;
          option.hidden = !commissioner;
          option.disabled = !commissioner;
        });
      }

      if (!commissioner && (currentTab() === 'managers' || window.location.hash === '#managers')) {
        const activityTab = document.querySelector('[data-league-tab="activity"]');
        activityTab?.click();
      }
    }

    function scrubVisibleMemberEmails() {
      if (canManage()) return;
      const league = leagueState();
      const members = Array.isArray(league?.members) ? league.members : [];
      const replacements = members
        .map((member, index) => ({
          email: String(member.email || '').trim(),
          name: safeMemberName(member, index)
        }))
        .filter((item) => emailPattern.test(item.email));

      if (!replacements.length) return;

      const walker = document.createTreeWalker(
        dashboard,
        NodeFilter.SHOW_TEXT,
        {
          acceptNode(node) {
            const parent = node.parentElement;
            if (!parent) return NodeFilter.FILTER_REJECT;
            if (parent.closest('script, style, input, textarea')) return NodeFilter.FILTER_REJECT;
            return replacements.some((item) => node.nodeValue.includes(item.email))
              ? NodeFilter.FILTER_ACCEPT
              : NodeFilter.FILTER_REJECT;
          }
        }
      );

      const nodes = [];
      while (walker.nextNode()) nodes.push(walker.currentNode);
      nodes.forEach((node) => {
        let next = node.nodeValue;
        replacements.forEach((item) => {
          next = next.split(item.email).join(item.name);
        });
        if (next !== node.nodeValue) node.nodeValue = next;
      });
    }

    function refresh() {
      refreshScheduled = false;
      updateMemberManagementAccess();
      renderActivityMembers();
      scrubVisibleMemberEmails();
    }

    function scheduleRefresh() {
      if (refreshScheduled) return;
      refreshScheduled = true;
      window.requestAnimationFrame(refresh);
    }

    const observer = new MutationObserver(scheduleRefresh);
    observer.observe(dashboard, {
      childList: true,
      subtree: true,
      characterData: true,
      attributes: true,
      attributeFilter: ['hidden', 'class']
    });

    document.addEventListener('click', (event) => {
      if (event.target.closest('[data-league-tab]')) window.setTimeout(scheduleRefresh, 0);
    });
    window.addEventListener('hashchange', scheduleRefresh);

    refresh();
  }

  function boot() {
    setupDraftRoomMenu();
    setupLeagueMemberPrivacy();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => waitForApp(boot), { once: true });
  } else {
    waitForApp(boot);
  }
})();
