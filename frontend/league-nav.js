(() => {
  'use strict';

  const pageName = window.location.pathname.split('/').pop() || 'index.html';
  if (pageName !== 'league.html') return;

  const nav = document.querySelector('.league-tabs');
  if (!nav || nav.dataset.cffGrouped === 'true') return;

  const tabs = [...nav.querySelectorAll(':scope > .league-tab')];
  if (!tabs.length) return;

  const groupDefinitions = [
    { key: 'league', label: 'League', items: ['overview', 'activity'] },
    { key: 'team', label: 'Team', items: ['team', 'freeagency', 'waivers', 'trades'] },
    { key: 'competition', label: 'Competition', items: ['scoreboard', 'standings', 'draft'] },
    { key: 'manage', label: 'Manage', items: ['managers', 'settings'] }
  ];

  function tabKey(tab) {
    if (tab.dataset.leagueTab) return tab.dataset.leagueTab;
    if (tab.tagName === 'A' && /draft\.html(?:$|[?#])/i.test(tab.getAttribute('href') || '')) return 'draft';
    return '';
  }

  const tabByKey = new Map(tabs.map((tab) => [tabKey(tab), tab]));
  const fragment = document.createDocumentFragment();
  const groupedTabs = new Set();

  groupDefinitions.forEach((definition) => {
    const groupTabs = definition.items.map((key) => tabByKey.get(key)).filter(Boolean);
    if (!groupTabs.length) return;

    const group = document.createElement('section');
    group.className = 'league-nav-group';
    group.dataset.navGroup = definition.key;
    group.setAttribute('aria-label', `${definition.label} navigation`);

    const label = document.createElement('span');
    label.className = 'league-nav-group__label';
    label.textContent = definition.label;

    const tabRow = document.createElement('div');
    tabRow.className = 'league-nav-group__tabs';
    tabRow.setAttribute('role', 'group');
    tabRow.setAttribute('aria-label', definition.label);

    groupTabs.forEach((tab) => {
      groupedTabs.add(tab);
      tab.dataset.navGroup = definition.key;
      tab.dataset.betaGroup = definition.key;
      tab.classList.remove('is-group-start');
      tab.title = `${definition.label}: ${tab.textContent.trim()}`;
      tab.setAttribute('aria-label', `${definition.label}: ${tab.textContent.trim()}`);
      tabRow.appendChild(tab);
    });

    group.append(label, tabRow);
    fragment.appendChild(group);
  });

  const remainingTabs = tabs.filter((tab) => !groupedTabs.has(tab));
  if (remainingTabs.length) {
    const group = document.createElement('section');
    group.className = 'league-nav-group';
    group.dataset.navGroup = 'more';
    group.setAttribute('aria-label', 'More navigation');

    const label = document.createElement('span');
    label.className = 'league-nav-group__label';
    label.textContent = 'More';

    const tabRow = document.createElement('div');
    tabRow.className = 'league-nav-group__tabs';
    remainingTabs.forEach((tab) => tabRow.appendChild(tab));
    group.append(label, tabRow);
    fragment.appendChild(group);
  }

  nav.replaceChildren(fragment);
  nav.classList.add('league-tabs--grouped');
  nav.dataset.cffGrouped = 'true';
  nav.setAttribute('aria-label', 'League workspace navigation');

  function syncActiveGroup() {
    nav.querySelectorAll('.league-nav-group').forEach((group) => {
      group.classList.toggle('is-active-group', Boolean(group.querySelector('.league-tab.is-active')));
    });
  }

  const activeObserver = new MutationObserver(syncActiveGroup);
  nav.querySelectorAll('.league-tab').forEach((tab) => {
    activeObserver.observe(tab, { attributes: true, attributeFilter: ['class'] });
  });
  syncActiveGroup();

  function labelMobileOptions() {
    const select = document.querySelector('.league-tab-select');
    if (!select) return false;
    const orderedTabs = [...nav.querySelectorAll('.league-tab')];
    [...select.options].forEach((option, index) => {
      const tab = orderedTabs[index];
      if (!tab) return;
      const group = groupDefinitions.find((definition) => definition.key === tab.dataset.navGroup);
      const prefix = group?.label || 'League';
      option.textContent = `${prefix} - ${tab.textContent.trim()}`;
    });
    return true;
  }

  if (!labelMobileOptions()) {
    const mobileObserver = new MutationObserver(() => {
      if (!labelMobileOptions()) return;
      mobileObserver.disconnect();
    });
    mobileObserver.observe(document.body, { childList: true, subtree: true });
  }
})();
