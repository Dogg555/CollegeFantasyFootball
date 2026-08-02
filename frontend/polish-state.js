(() => {
  'use strict';

  const ui = window.CFF_UI;
  let lastStatus = '';
  let lastStatusAt = 0;

  function stateKind(node) {
    const text = String(node.textContent || '').trim().toLowerCase();
    if (!text) return 'empty';
    if (/loading|searching|saving|sending|working|verifying|resetting|processing/.test(text)) return 'loading';
    if (/no |not found|sign in to|create a league|add players|empty|yet\.?$/.test(text)) return 'empty';
    if (/failed|unable|could not|unavailable|invalid|error/.test(text)) return 'error';
    return 'ready';
  }

  function decorateState(node) {
    if (!(node instanceof HTMLElement)) return;
    const kind = stateKind(node);
    node.classList.toggle('is-loading', kind === 'loading');
    node.classList.toggle('is-empty', kind === 'empty');
    node.classList.toggle('is-error', kind === 'error');
    if (kind === 'loading') node.setAttribute('aria-busy', 'true');
    else node.removeAttribute('aria-busy');
  }

  function enhanceStates() {
    const selector = '.list, [role="status"]';
    document.querySelectorAll(selector).forEach(decorateState);
    const observer = new MutationObserver((mutations) => {
      const targets = new Set();
      mutations.forEach((mutation) => {
        const base = mutation.target instanceof HTMLElement ? mutation.target : mutation.target.parentElement;
        const target = base?.closest(selector);
        if (target) targets.add(target);
      });
      targets.forEach((target) => {
        decorateState(target);
        if (!target.matches('[role="status"]')) return;
        const text = target.textContent.trim();
        const now = Date.now();
        if (!text || /working|loading|searching|saving|sending|verifying|resetting/i.test(text)) return;
        if (text === lastStatus && now - lastStatusAt < 1500) return;
        lastStatus = text;
        lastStatusAt = now;
        ui?.notify(text, stateKind(target) === 'error' ? 'error' : 'success');
      });
    });
    observer.observe(document.body, { childList: true, subtree: true, characterData: true });
  }

  function enhanceLeagueTabs() {
    const tabs = [...document.querySelectorAll('[data-league-tab]')];
    if (!tabs.length) return;
    const key = 'cff:last-league-tab';
    tabs.forEach((tab) => {
      tab.setAttribute('role', 'tab');
      tab.addEventListener('click', () => sessionStorage.setItem(key, tab.dataset.leagueTab));
    });
    const requested = window.location.hash.slice(1) || sessionStorage.getItem(key);
    const target = tabs.find((tab) => tab.dataset.leagueTab === requested);
    if (target && !target.classList.contains('is-active')) window.setTimeout(() => target.click(), 0);
  }

  function enhanceDraftAnnouncements() {
    const pick = document.getElementById('draft-current-pick');
    const manager = document.getElementById('draft-current-manager');
    if (!pick || !manager) return;
    let previous = '';
    const announce = () => {
      const value = `${pick.textContent.trim()} - ${manager.textContent.trim()}`;
      if (!value || value === previous || /tbd|no league|complete/i.test(value)) return;
      previous = value;
      ui?.notify(value, 'info', 2600);
    };
    const observer = new MutationObserver(announce);
    observer.observe(pick, { childList: true, subtree: true, characterData: true });
    observer.observe(manager, { childList: true, subtree: true, characterData: true });
  }

  function enhanceAccessibility() {
    document.documentElement.classList.add('cff-polished');
    document.querySelectorAll('button:not([aria-label]), a.button:not([aria-label])').forEach((control) => {
      const text = control.textContent.trim();
      if (text) control.setAttribute('aria-label', text);
    });
    document.querySelectorAll('.modal').forEach((modal) => {
      modal.addEventListener('keydown', (event) => {
        if (event.key === 'Escape') modal.querySelector('#close-modal, [aria-label*="Close"]')?.click();
      });
    });
  }

  function boot() {
    enhanceStates();
    enhanceLeagueTabs();
    enhanceDraftAnnouncements();
    enhanceAccessibility();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
