(() => {
  'use strict';

  const form = document.getElementById('resend-form');
  const emailInput = document.getElementById('resend-email');
  const cooldownStatus = document.getElementById('resend-cooldown');
  const submitButton = form?.querySelector('button[type="submit"]');
  if (!form || !emailInput || !submitButton || !cooldownStatus) return;

  const STORAGE_PREFIX = 'cff_verification_resend_cooldown_v1:';
  const DEFAULT_COOLDOWN_SECONDS = Math.max(
    1,
    Number.parseInt(form.dataset.cooldownSeconds || '60', 10) || 60
  );
  const originalButtonLabel = submitButton.textContent || 'Send verification';
  const originalFetch = window.fetch.bind(window);
  let timer = null;

  function canonicalEmail(value) {
    return String(value || '').trim().toLowerCase();
  }

  function storageKey(email) {
    return `${STORAGE_PREFIX}${encodeURIComponent(canonicalEmail(email))}`;
  }

  function readExpiry(email) {
    if (!canonicalEmail(email)) return 0;
    try {
      const value = Number.parseInt(window.localStorage.getItem(storageKey(email)) || '0', 10);
      return Number.isFinite(value) && value > 0 ? value : 0;
    } catch {
      return 0;
    }
  }

  function writeExpiry(email, seconds) {
    const normalized = canonicalEmail(email);
    if (!normalized) return;
    const duration = Math.max(1, Number.parseInt(seconds, 10) || DEFAULT_COOLDOWN_SECONDS);
    try {
      window.localStorage.setItem(storageKey(normalized), String(Date.now() + duration * 1000));
    } catch {
      // The in-page countdown still works when storage is unavailable.
      form.dataset.cooldownFallbackExpiry = String(Date.now() + duration * 1000);
      form.dataset.cooldownFallbackEmail = normalized;
    }
  }

  function fallbackExpiry(email) {
    return form.dataset.cooldownFallbackEmail === canonicalEmail(email)
      ? Number.parseInt(form.dataset.cooldownFallbackExpiry || '0', 10) || 0
      : 0;
  }

  function remainingSeconds(email) {
    const expiry = Math.max(readExpiry(email), fallbackExpiry(email));
    return Math.max(0, Math.ceil((expiry - Date.now()) / 1000));
  }

  function clearExpired(email) {
    const normalized = canonicalEmail(email);
    if (!normalized) return;
    try {
      window.localStorage.removeItem(storageKey(normalized));
    } catch {
      // Storage cleanup is optional.
    }
    if (form.dataset.cooldownFallbackEmail === normalized) {
      delete form.dataset.cooldownFallbackEmail;
      delete form.dataset.cooldownFallbackExpiry;
    }
  }

  function renderCooldown() {
    const email = canonicalEmail(emailInput.value);
    const remaining = remainingSeconds(email);
    const submitting = form.dataset.submitting === 'true';

    if (remaining > 0) {
      submitButton.disabled = true;
      submitButton.setAttribute('aria-disabled', 'true');
      submitButton.textContent = `Resend in ${remaining}s`;
      cooldownStatus.textContent = `You can request another verification email in ${remaining} second${remaining === 1 ? '' : 's'}.`;
      return remaining;
    }

    clearExpired(email);
    cooldownStatus.textContent = '';
    if (!submitting) {
      submitButton.disabled = false;
      submitButton.removeAttribute('aria-disabled');
      submitButton.textContent = originalButtonLabel;
    }
    return 0;
  }

  function ensureTimer() {
    if (timer) return;
    timer = window.setInterval(() => {
      if (renderCooldown() === 0) {
        window.clearInterval(timer);
        timer = null;
      }
    }, 1000);
  }

  function beginCooldown(email, seconds = DEFAULT_COOLDOWN_SECONDS) {
    writeExpiry(email, seconds);
    renderCooldown();
    ensureTimer();
  }

  function requestEmail(init = {}) {
    if (typeof init.body !== 'string') return canonicalEmail(emailInput.value);
    try {
      return canonicalEmail(JSON.parse(init.body)?.email || emailInput.value);
    } catch {
      return canonicalEmail(emailInput.value);
    }
  }

  function requestUrl(input) {
    return typeof input === 'string' ? input : String(input?.url || '');
  }

  function retryAfterSeconds(response) {
    const value = Number.parseInt(response.headers?.get?.('Retry-After') || '', 10);
    return Number.isFinite(value) && value > 0 ? value : DEFAULT_COOLDOWN_SECONDS;
  }

  window.fetch = async function fetchWithVerificationCooldown(input, init = {}) {
    const isResendRequest = /\/auth\/resend-verification(?:\?|$)/.test(requestUrl(input));
    const email = isResendRequest ? requestEmail(init) : '';
    const response = await originalFetch(input, init);

    if (isResendRequest && (response.ok || response.status === 429)) {
      let seconds = response.status === 429
        ? retryAfterSeconds(response)
        : DEFAULT_COOLDOWN_SECONDS;
      if (response.ok) {
        try {
          const payload = await response.clone().json();
          seconds = Math.max(1, Number.parseInt(payload?.cooldownSeconds || seconds, 10) || seconds);
        } catch {
          // The default cooldown applies to non-JSON success responses.
        }
      }
      beginCooldown(email, seconds);
    }

    return response;
  };

  form.addEventListener('submit', (event) => {
    if (remainingSeconds(emailInput.value) <= 0) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    renderCooldown();
    ensureTimer();
  }, true);

  emailInput.addEventListener('input', renderCooldown);
  window.addEventListener('storage', (event) => {
    if (event.key?.startsWith(STORAGE_PREFIX)) renderCooldown();
  });
  window.addEventListener('pagehide', () => {
    if (timer) window.clearInterval(timer);
  }, { once: true });

  if (renderCooldown() > 0) ensureTimer();
})();
