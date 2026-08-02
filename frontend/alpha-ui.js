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

  function setupLeagueMobileNav() {
    const tabs = document.querySelector('.league-tabs');
    if (!tabs || document.querySelector('.league-tab-select')) return;
    const targets = [...tabs.querySelectorAll('.league-tab')];
    if (!targets.length) return;

    const wrap = document.createElement('div');
    wrap.className = 'league-tab-select-wrap layout';
    wrap.style.maxWidth = '1080px';
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
    tabs.closest('main')?.insertAdjacentElement('afterend', wrap);

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

  function init() {
    document.documentElement.classList.add('alpha-ui-ready');
    setupLeagueMobileNav();
    setupCollapsibleCards();
    setupDraftStatusDock();
  }

  root.CFF_ALPHA_UI = helpers;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})(typeof window !== 'undefined' ? window : globalThis);
