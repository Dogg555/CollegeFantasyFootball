(() => {
  'use strict';

  const BUSY_TIMEOUT_MS = 22000;
  const ui = window.CFF_UI;

  function node(tag, className = '', text = '') {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text) element.textContent = text;
    return element;
  }

  function passwordScore(value) {
    let score = 0;
    if (value.length >= 10) score += 1;
    if (/[a-z]/.test(value) && /[A-Z]/.test(value)) score += 1;
    if (/\d/.test(value)) score += 1;
    if (/[^A-Za-z0-9]/.test(value)) score += 1;
    return score;
  }

  function enhancePasswords() {
    document.querySelectorAll('input[type="password"]').forEach((input) => {
      if (input.dataset.cffEnhanced) return;
      input.dataset.cffEnhanced = 'true';
      const wrapper = node('div', 'password-field');
      input.parentNode.insertBefore(wrapper, input);
      wrapper.appendChild(input);
      const toggle = node('button', 'password-toggle', 'Show');
      toggle.type = 'button';
      toggle.setAttribute('aria-label', 'Show password');
      toggle.addEventListener('click', () => {
        const reveal = input.type === 'password';
        input.type = reveal ? 'text' : 'password';
        toggle.textContent = reveal ? 'Hide' : 'Show';
        toggle.setAttribute('aria-label', reveal ? 'Hide password' : 'Show password');
      });
      wrapper.appendChild(toggle);

      if (input.autocomplete === 'new-password') {
        input.minLength = Math.max(Number(input.minLength || 0), 10);
        const meter = node('div', 'password-meter');
        meter.dataset.score = '0';
        const bar = node('span', 'password-meter__bar');
        const text = node('span', 'password-meter__text', 'Use 10+ characters with mixed types.');
        meter.append(bar, text);
        wrapper.insertAdjacentElement('afterend', meter);
        input.addEventListener('input', () => {
          const score = passwordScore(input.value);
          meter.dataset.score = String(score);
          text.textContent = ['Too weak', 'Weak', 'Fair', 'Good', 'Strong'][score];
        });
      }
    });
  }

  function enhanceInputs() {
    document.querySelectorAll('input[type="email"]').forEach((input) => {
      input.autocapitalize = 'none';
      input.spellcheck = false;
      input.addEventListener('change', () => { input.value = input.value.trim().toLowerCase(); });
    });
    const future = new Date(Date.now() + 60000);
    future.setSeconds(0, 0);
    const minimum = new Date(future.getTime() - future.getTimezoneOffset() * 60000).toISOString().slice(0, 16);
    document.querySelectorAll('input[type="datetime-local"]').forEach((input) => {
      if (input.min) return;

      if (input.id === 'settings-draft-date') {
        let originalValue = null;
        const rememberOriginalValue = () => {
          if (originalValue === null) originalValue = input.value;
        };
        const enforceMinimumAfterEdit = () => {
          rememberOriginalValue();
          if (!input.value || input.value === originalValue) {
            input.removeAttribute('min');
            return;
          }
          input.min = minimum;
        };

        input.addEventListener('focus', rememberOriginalValue);
        input.addEventListener('pointerdown', rememberOriginalValue);
        input.addEventListener('keydown', rememberOriginalValue);
        input.addEventListener('input', enforceMinimumAfterEdit);
        input.addEventListener('change', enforceMinimumAfterEdit);
        return;
      }

      input.min = minimum;
    });
    document.querySelectorAll('input, select, textarea').forEach((field) => {
      field.addEventListener('input', () => field.classList.remove('is-invalid'));
      field.addEventListener('change', () => field.classList.remove('is-invalid'));
    });
  }

  function validateInvites(field) {
    if (!field) return true;
    const entries = field.value.split(/[,\n]/).map((value) => value.trim()).filter(Boolean);
    const invalid = entries.find((value) => !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value));
    const message = entries.length > 24
      ? 'Invite no more than 24 managers at once.'
      : invalid ? `Invalid email address: ${invalid}` : '';
    field.setCustomValidity(message);
    field.classList.toggle('is-invalid', Boolean(message));
    return !message;
  }

  function formLabel(form) {
    if (/login/.test(form.id)) return 'Signing in...';
    if (/signup/.test(form.id)) return 'Creating account...';
    if (/waiver/.test(form.id)) return 'Submitting claim...';
    if (/trade/.test(form.id)) return 'Sending offer...';
    if (/league/.test(form.id)) return 'Saving league...';
    if (/reset/.test(form.id)) return 'Sending...';
    return 'Working...';
  }

  function enhanceForms() {
    document.addEventListener('submit', (event) => {
      const form = event.target;
      if (!(form instanceof HTMLFormElement)) return;
      form.querySelectorAll('#settings-invites, #invite-emails, textarea[name="invitedEmails"]')
        .forEach(validateInvites);
      if (!form.checkValidity()) {
        event.preventDefault();
        event.stopImmediatePropagation();
        form.reportValidity();
        const first = form.querySelector(':invalid');
        const status = form.querySelector('[role="status"]');
        first?.classList.add('is-invalid');
        first?.focus();
        if (status) {
          status.textContent = first?.validationMessage || 'Review the highlighted fields before continuing.';
          status.style.color = 'var(--danger)';
        }
        ui?.notify(first?.validationMessage || 'Review the highlighted fields before continuing.', 'error');
        return;
      }
      if (form.dataset.cffSubmitting === 'true') {
        event.preventDefault();
        event.stopImmediatePropagation();
        return;
      }
      form.dataset.cffSubmitting = 'true';
      form.setAttribute('aria-busy', 'true');
      const submit = event.submitter || form.querySelector('[type="submit"]');
      if (submit) ui?.setBusy(submit, true, formLabel(form));
      window.setTimeout(() => {
        if (!form.isConnected) return;
        delete form.dataset.cffSubmitting;
        form.removeAttribute('aria-busy');
        if (submit?.isConnected) ui?.setBusy(submit, false);
      }, BUSY_TIMEOUT_MS);
    }, true);
  }

  const confirmations = [
    ['#clear-league', 'Clear this saved league? This cannot be undone.'],
    ['#clear-draft', 'Reset this draft? All picks and roster assignments will be removed.'],
    ['#undo-last-pick', 'Undo the most recent draft pick?'],
    ['#finalize-week', 'Finalize this scoring week? Final results may lock lineup changes.'],
    ['#reset-waiver-priority', 'Reset the waiver priority order?'],
    ['#reset-draft-order', 'Reset the draft order before the first pick?'],
    ['[data-drop], [data-drop-player]', 'Drop this player from the roster?'],
    ['[data-release]', 'Release this player and return them to the draft queue?'],
    ['[data-cancel-waiver], [data-trade-cancel]', 'Cancel this pending request?'],
    ['[data-remove-league]', 'Remove this league? This cannot be undone.'],
    ['[data-trade-accept]', 'Accept this trade offer?'],
    ['[data-trade-decline]', 'Decline this trade offer?'],
    ['[data-trade-veto]', 'Veto this accepted trade?']
  ];

  function confirmation(control) {
    return confirmations.find(([selector]) => control.matches(selector))?.[1] || '';
  }

  function actionLabel(control) {
    if (control.matches('[data-draft]')) return 'Drafting...';
    if (control.matches('[data-player], [data-player-index], [data-queue]')) return 'Queuing...';
    if (control.matches('[data-add], [data-add-free-agent]')) return 'Adding...';
    if (control.matches('[data-drop], [data-drop-player], [data-remove-league], [data-release]')) return 'Removing...';
    if (control.matches('[data-process-waiver], [data-process-all-waivers]')) return 'Processing...';
    if (control.matches('[data-trade-accept], [data-trade-decline], [data-trade-cancel], [data-trade-approve], [data-trade-veto]')) return 'Updating...';
    return 'Working...';
  }

  function enhanceActions() {
    const busySelector = '[data-draft], [data-player], [data-player-index], [data-queue], [data-add], [data-add-free-agent], [data-drop], [data-drop-player], [data-remove-league], [data-release], [data-process-waiver], [data-process-all-waivers], [data-trade-accept], [data-trade-decline], [data-trade-cancel], [data-trade-approve], [data-trade-veto]';
    document.addEventListener('click', (event) => {
      const control = event.target.closest('button, a.button');
      if (!control) return;
      if (control.disabled || control.getAttribute('aria-disabled') === 'true' || control.dataset.cffBusy === 'true') {
        event.preventDefault();
        event.stopImmediatePropagation();
        return;
      }
      const message = confirmation(control);
      if (message && !window.confirm(message)) {
        event.preventDefault();
        event.stopImmediatePropagation();
        return;
      }
      if (control.matches(busySelector)) ui?.setBusy(control, true, actionLabel(control));
    }, true);
  }

  function boot() {
    enhancePasswords();
    enhanceInputs();
    enhanceForms();
    enhanceActions();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
