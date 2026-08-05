(() => {
  'use strict';

  const READ_REQUEST_TIMEOUT_MS = 20000;
  const MUTATION_REQUEST_TIMEOUT_MS = 60000;
  const BUSY_TIMEOUT_MS = 62000;
  const NOTIFICATION_DEDUP_MS = 8000;
  const MUTATION_CONTROL_SELECTOR = [
    'form:not([method="get"]):not([data-cff-allow-outage="true"]) button[type="submit"]',
    'form:not([method="get"]):not([data-cff-allow-outage="true"]) input[type="submit"]',
    '[data-cff-mutation="true"]'
  ].join(', ');
  const nativeFetch = window.fetch.bind(window);
  const busyControls = new Set();
  const notificationHistory = new Map();
  let activeRequests = 0;
  let serviceUnavailable = false;
  let outageObserver = null;

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

  function requestMethod(input, options = {}) {
    if (options.method) return String(options.method).toUpperCase();
    if (typeof Request !== 'undefined' && input instanceof Request) {
      return String(input.method || 'GET').toUpperCase();
    }
    return 'GET';
  }

  function timeoutForMethod(method) {
    return ['GET', 'HEAD', 'OPTIONS'].includes(method)
      ? READ_REQUEST_TIMEOUT_MS
      : MUTATION_REQUEST_TIMEOUT_MS;
  }

  function isRetryablePut(input, method, error) {
    if (method !== 'PUT' || typeof input !== 'string') return false;
    if (error?.name === 'AbortError' || error?.name === 'TimeoutError') return false;
    return /failed to fetch|networkerror|load failed/i.test(String(error?.message || error || ''));
  }

  window.fetch = async (input, options = {}) => {
    const method = requestMethod(input, options);
    const timeoutMs = timeoutForMethod(method);
    const url = typeof input === 'string' ? input : input?.url || '';
    const maxAttempts = method === 'PUT' && typeof input === 'string' ? 2 : 1;
    activeRequests += 1;
    emit('cff:request-start', { url, method, activeRequests });

    try {
      let lastError = null;
      for (let attempt = 1; attempt <= maxAttempts; attempt += 1) {
        const controller = options.signal ? null : new AbortController();
        const timeout = controller
          ? window.setTimeout(() => controller.abort(new DOMException('Request timed out', 'TimeoutError')), timeoutMs)
          : null;

        try {
          const response = await nativeFetch(input, controller ? { ...options, signal: controller.signal } : options);
          emit('cff:request-response', { url, method, status: response.status, ok: response.ok, attempt });
          return response;
        } catch (error) {
          lastError = error;
          const retry = attempt < maxAttempts && navigator.onLine && isRetryablePut(input, method, error);
          if (!retry) {
            emit('cff:request-error', { url, method, error, attempt, message: friendlyError(error) });
            throw error;
          }
          emit('cff:request-retry', { url, method, attempt, message: friendlyError(error) });
          await new Promise((resolve) => window.setTimeout(resolve, 500));
        } finally {
          if (timeout) window.clearTimeout(timeout);
        }
      }
      throw lastError || new Error('Request failed');
    } finally {
      activeRequests = Math.max(0, activeRequests - 1);
      emit('cff:request-end', { url, method, activeRequests });
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
      const retry = create('button', 'button button--ghost cff-network-status__retry', 'Retry page');
      retry.type = 'button';
      retry.dataset.cffAllowOutage = 'true';
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

  function notificationKey(message, type) {
    return `${String(type || 'info').trim().toLowerCase()}::${String(message || '').trim()}`;
  }

  function notify(message, type = 'info', timeout = 4200) {
    if (!message) return;
    const key = notificationKey(message, type);
    const now = Date.now();
    const lastShownAt = notificationHistory.get(key) || 0;
    if (now - lastShownAt < NOTIFICATION_DEDUP_MS) return;
    notificationHistory.set(key, now);
    if (notificationHistory.size > 50) {
      for (const [historyKey, shownAt] of notificationHistory) {
        if (now - shownAt > NOTIFICATION_DEDUP_MS) notificationHistory.delete(historyKey);
      }
    }

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
    positionToastRegion();
    requestAnimationFrame(() => toast.classList.add('is-visible'));
    window.setTimeout(() => {
      toast.classList.remove('is-visible');
      window.setTimeout(() => toast.remove(), 180);
    }, timeout);
  }

  function positionToastRegion() {
    ensureFeedbackUi();
    const region = document.getElementById('cff-toast-region');
    if (!region) return;
    if (!window.matchMedia('(max-width: 760px)').matches) {
      region.style.removeProperty('top');
      return;
    }
    const banner = document.getElementById('cff-network-status');
    const topbar = document.querySelector('.topbar');
    const anchor = banner && !banner.hidden ? banner : topbar;
    const bottom = anchor?.getBoundingClientRect().bottom || 64;
    region.style.top = `${Math.max(12, Math.ceil(bottom + 12))}px`;
  }

  function markControlUnavailable(control) {
    if (!control || control.dataset.cffNetworkDisabled === 'true' || control.disabled) return;
    control.dataset.cffNetworkDisabled = 'true';
    control.dataset.cffNetworkTitle = control.getAttribute('title') || '';
    control.disabled = true;
    control.setAttribute('aria-disabled', 'true');
    control.setAttribute('title', 'Unavailable while the service is offline.');
  }

  function restoreControl(control) {
    if (!control || control.dataset.cffNetworkDisabled !== 'true') return;
    control.disabled = false;
    control.removeAttribute('aria-disabled');
    const originalTitle = control.dataset.cffNetworkTitle || '';
    if (originalTitle) control.setAttribute('title', originalTitle);
    else control.removeAttribute('title');
    delete control.dataset.cffNetworkDisabled;
    delete control.dataset.cffNetworkTitle;
  }

  function setMutationControlsUnavailable(unavailable) {
    serviceUnavailable = Boolean(unavailable);
    document.documentElement.classList.toggle('cff-api-unavailable', serviceUnavailable);
    document.documentElement.dataset.cffApiUnavailable = String(serviceUnavailable);
    if (serviceUnavailable) {
      document.querySelectorAll(MUTATION_CONTROL_SELECTOR).forEach(markControlUnavailable);
      return;
    }
    document.querySelectorAll('[data-cff-network-disabled="true"]').forEach(restoreControl);
  }

  function setNetworkStatus(online, message = '') {
    ensureFeedbackUi();
    const banner = document.getElementById('cff-network-status');
    const copy = banner?.querySelector('.cff-network-status__message');
    if (!banner || !copy) return;
    const unavailable = !online;
    banner.hidden = online && !message;
    banner.classList.toggle('is-error', unavailable);
    banner.classList.toggle('is-working', online && Boolean(message));
    copy.textContent = message || (online
      ? 'Connection restored.'
      : 'You are offline. Cached league and draft data remain available on this device.');
    setMutationControlsUnavailable(unavailable);
    requestAnimationFrame(positionToastRegion);
  }

  function setBusy(control, busy, label = 'Working...') {
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
    if (serviceUnavailable) markControlUnavailable(control);
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

  function isProtectedMutationForm(form) {
    if (!form || form.dataset.cffAllowOutage === 'true') return false;
    return String(form.getAttribute('method') || 'post').toLowerCase() !== 'get';
  }

  function enhanceConnectivity() {
    setNetworkStatus(navigator.onLine);
    window.addEventListener('offline', () => {
      setNetworkStatus(false, 'You are offline. Cached data remains available.');
    });
    window.addEventListener('online', () => {
      setNetworkStatus(true, 'Connection restored. Refreshing current data...');
      notify('Connection restored.', 'success');
      window.setTimeout(() => setNetworkStatus(true), 2200);
      window.dispatchEvent(new Event('focus'));
    });
    window.addEventListener('resize', positionToastRegion, { passive: true });
    document.addEventListener('submit', (event) => {
      if (!serviceUnavailable || !isProtectedMutationForm(event.target)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      setNetworkStatus(false, 'The service could not be reached. Cached data remains available.');
    }, true);
    document.addEventListener('cff:request-start', () => document.documentElement.classList.add('cff-request-active'));
    document.addEventListener('cff:request-end', (event) => {
      if (event.detail.activeRequests === 0) {
        document.documentElement.classList.remove('cff-request-active');
        releaseBusy();
      }
    });
    document.addEventListener('cff:request-response', (event) => {
      const status = Number(event.detail.status || 0);
      if (status >= 500) {
        setNetworkStatus(false, 'The service is temporarily unavailable. Cached data remains available.');
      } else if (status > 0 && navigator.onLine && serviceUnavailable) {
        setNetworkStatus(true);
      }
    });
    document.addEventListener('cff:request-error', (event) => {
      const message = event.detail.message || 'The service could not be reached.';
      setNetworkStatus(false, message);
    });

    outageObserver = new MutationObserver(() => {
      if (serviceUnavailable) setMutationControlsUnavailable(true);
      positionToastRegion();
    });
    outageObserver.observe(document.body, { childList: true, subtree: true });
  }

  function boot() {
    ensureFeedbackUi();
    enhanceNavigation();
    enhanceConnectivity();
    positionToastRegion();
  }

  window.CFF_UI = Object.freeze({
    notify,
    setBusy,
    friendlyError,
    setNetworkStatus,
    setMutationControlsUnavailable
  });
  window.CFF_POLISH = Object.freeze({ create, emit, releaseBusy, positionToastRegion });
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
