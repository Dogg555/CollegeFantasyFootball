(() => {
  'use strict';

  const REQUEST_TIMEOUT_MS = 20000;
  const BUSY_TIMEOUT_MS = 22000;
  const nativeFetch = window.fetch.bind(window);
  const busyControls = new Set();
  let activeRequests = 0;

  function emit(name, detail = {}) {
    document.dispatchEvent(new CustomEvent(name, { detail }));
  }

  function friendlyError(error) {
    if (!error) return 'Something went wrong. Please try again.';
    if (error.name === 'AbortError' || error.name === 'TimeoutError') {
      return 'The request took too long. Check your connection and try again.';
    }
    const message = String(error.message || error);
    return /failed to fetch|networkerror|load failed/i.test(message)
      ? 'The service could not be reached. Cached data remains available.'
      : message;
  }

  window.fetch = async (input, options = {}) => {
    const controller = options.signal ? null : new AbortController();
    const timeout = controller
      ? window.setTimeout(() => controller.abort(new DOMException('Request timed out', 'TimeoutError')), REQUEST_TIMEOUT_MS)
      : null;
    const url = typeof input === 'string' ? input : input?.url || '';
    activeRequests += 1;
    emit('cff:request-start', { url, activeRequests });
    try {
      const response = await nativeFetch(input, controller ? { ...options, signal: controller.signal } : options);
      emit('cff:request-response', { url, status: response.status, ok: response.ok });
      return response;
    } catch (error) {
      emit('cff:request-error', { url, error, message: friendlyError(error) });
      throw error;
    } finally {
      if (timeout) window.clearTimeout(timeout);
      activeRequests = Math.max(0, activeRequests - 1);
      emit('cff:request-end', { url, activeRequests });
    }
  };

  function create(tag, className = '', text = '') {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text) node.textContent = text;
    return node;
  }

  function ensureFeedbackUi() {
    if (!document.getElementById('cff-network-status')) {
      const banner = create('div', 'cff-network-status');
      banner.id = 'cff-network-status';
      banner.hidden = true;
      banner.setAttribute('role', 'status');
      banner.setAttribute('aria-live', 'polite');
      const message = create('span', 'cff-network-status__message');
      const retry = create('button', 'button button--ghost cff-network-status__retry', 'Retry');
      retry.type = 'button';
      retry.addEventListener('click', () => window.location.reload());
      banner.append(message, retry);
      const topbar = document.querySelector('.topbar');
      if (topbar) topbar.insertAdjacentElement('afterend', banner);
      else document.body.prepend(banner);
    }
    if (!document.getElementById('cff-toast-region')) {
      const region = create('div', 'cff-toast-region');
      region.id = 'cff-toast-region';
      region.setAttribute('aria-live', 'polite');
      region.setAttribute('aria-atomic', 'true');
      document.body.appendChild(region);
    }
  }

  function notify(message, type = 'info', timeout = 4200) {
    if (!message) return;
    ensureFeedbackUi();
    const region = document.getElementById('cff-toast-region');
    const toast = create('div', `cff-toast cff-toast--${type}`);
    toast.setAttribute('role', type === 'error' ? 'alert' : 'status');
    const copy = create('span', 'cff-toast__copy', message);
    const close = create('button', 'cff-toast__close', '×');
    close.type = 'button';
    close.setAttribute('aria-label', 'Dismiss message');
    close.addEventListener('click', () => toast.remove());
    toast.append(copy, close);
    region.appendChild(toast);
    requestAnimationFrame(() => toast.classList.add('is-visible'));
    window.setTimeout(() => {
      toast.classList.remove('is-visible');
      window.setTimeout(() => toast.remove(), 180);
    }, timeout);
  }

  function setNetworkStatus(online, message = '') {
    ensureFeedbackUi();
    const banner = document.getElementById('cff-network-status');
    const copy = banner?.querySelector('.cff-network-status__message');
    if (!banner || !copy) return;
    banner.hidden = online && !message;
    banner.classList.toggle('is-error', !online);
    banner.classList.toggle('is-working', online && Boolean(message));
    copy.textContent = message || (online
      ? 'Connection restored.'
      : 'You are offline. Cached league and draft data remain available on this device.');
  }

  function setBusy(control, busy, label = 'Working…') {
    if (!control) return;
    if (busy) {
      if (control.dataset.cffBusy === 'true') return;
      control.dataset.cffBusy = 'true';
      control.dataset.cffOriginalText = control.textContent;
      control.disabled = true;
      control.setAttribute('aria-busy', 'true');
      control.textContent = label;
      busyControls.add(control);
      window.setTimeout(() => setBusy(control, false), BUSY_TIMEOUT_MS);
      return;
    }
    control.disabled = false;
    control.removeAttribute('aria-busy');
    control.textContent = control.dataset.cffOriginalText || control.textContent;
    delete control.dataset.cffBusy;
    delete control.dataset.cffOriginalText;
    busyControls.delete(control);
  }

  function releaseBusy() {
    busyControls.forEach((control) => setBusy(control, false));
    document.querySelectorAll('form[data-cff-submitting="true"]').forEach((form) => {
      delete form.dataset.cffSubmitting;
      form.removeAttribute('aria-busy');
    });
  }

  function enhanceNavigation() {
    const topbar = document.querySelector('.topbar');
    const nav = topbar?.querySelector('.nav');
    const actions = topbar?.querySelector('.nav__actions');
    if (!topbar || !nav || !actions) return;
    const main = document.querySelector('main');
    if (main && !main.id) main.id = 'main-content';
    if (main && !document.querySelector('.skip-link')) {
      const skip = create('a', 'skip-link', 'Skip to main content');
      skip.href = '#main-content';
      document.body.prepend(skip);
    }
    if (!topbar.querySelector('.nav-toggle')) {
      const toggle = create('button', 'nav-toggle', 'Menu');
      toggle.type = 'button';
      toggle.setAttribute('aria-expanded', 'false');
      toggle.addEventListener('click', () => {
        const open = topbar.classList.toggle('is-nav-open');
        toggle.textContent = open ? 'Close' : 'Menu';
        toggle.setAttribute('aria-expanded', String(open));
        toggle.setAttribute('aria-label', open ? 'Close navigation' : 'Open navigation');
      });
      topbar.querySelector('.brand')?.insertAdjacentElement('afterend', toggle);
    }
    const file = window.location.pathname.split('/').pop() || 'index.html';
    const page = { 'index.html': 'home', 'league.html': 'league', 'draft.html': 'league', 'players.html': 'players' }[file];
    document.querySelectorAll('.nav__link').forEach((link) => {
      const active = Boolean(page && link.dataset.page === page);
      link.classList.toggle('is-active', active);
      if (active) link.setAttribute('aria-current', 'page');
      else link.removeAttribute('aria-current');
    });
    document.addEventListener('click', (event) => {
      if (!event.target.closest('.nav__link, .nav__actions a')) return;
      topbar.classList.remove('is-nav-open');
      const toggle = topbar.querySelector('.nav-toggle');
      toggle?.setAttribute('aria-expanded', 'false');
      if (toggle) toggle.textContent = 'Menu';
    });
  }

  function enhanceConnectivity() {
    setNetworkStatus(navigator.onLine);
    window.addEventListener('offline', () => {
      setNetworkStatus(false);
      notify('You are offline. Cached data remains available.', 'error', 6000);
    });
    window.addEventListener('online', () => {
      setNetworkStatus(true, 'Connection restored. Refreshing current data…');
      notify('Connection restored.', 'success');
      window.setTimeout(() => setNetworkStatus(true), 2200);
      window.dispatchEvent(new Event('focus'));
    });
    document.addEventListener('cff:request-start', () => document.documentElement.classList.add('cff-request-active'));
    document.addEventListener('cff:request-end', (event) => {
      if (event.detail.activeRequests === 0) {
        document.documentElement.classList.remove('cff-request-active');
        releaseBusy();
      }
    });
    document.addEventListener('cff:request-error', (event) => {
      const message = event.detail.message || 'The service could not be reached.';
      setNetworkStatus(false, message);
      notify(message, 'error', 6500);
    });
  }

  function boot() {
    ensureFeedbackUi();
    enhanceNavigation();
    enhanceConnectivity();
  }

  window.CFF_UI = Object.freeze({ notify, setBusy, friendlyError, setNetworkStatus });
  window.CFF_POLISH = Object.freeze({ create, emit, releaseBusy });
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
