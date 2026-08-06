(function initSnakeDraftOnly(root) {
  'use strict';

  const SNAKE_DRAFT_TYPE = 'snake';
  const FUTURE_RELEASE_LABEL = 'Auction (coming in future release)';
  const FUTURE_RELEASE_MESSAGE = 'Snake drafts are available for beta. Auction drafts are coming in a future release.';

  function normalizeDraftType(value = '') {
    return String(value || '').trim().toLowerCase();
  }

  function supportedDraftType(value = '') {
    const normalized = normalizeDraftType(value);
    return !normalized || normalized === SNAKE_DRAFT_TYPE;
  }

  function setAvailabilityMessage() {
    const status = root.document?.getElementById?.('form-status');
    if (!status) return;
    status.textContent = FUTURE_RELEASE_MESSAGE;
    status.classList.remove('is-error');
    status.style.color = 'var(--muted)';
  }

  function configureDraftTypeControls() {
    const input = root.document?.getElementById?.('draft-type');
    if (input) input.value = SNAKE_DRAFT_TYPE;

    root.document?.querySelectorAll?.('.segment[data-value="snake"]').forEach((button) => {
      button.disabled = false;
      button.classList.add('is-active');
      button.setAttribute('aria-pressed', 'true');
    });

    root.document?.querySelectorAll?.('.segment[data-value="auction"]').forEach((button) => {
      button.disabled = true;
      button.classList.remove('is-active');
      button.classList.add('is-disabled');
      button.setAttribute('aria-disabled', 'true');
      button.setAttribute('aria-pressed', 'false');
      button.setAttribute('title', FUTURE_RELEASE_MESSAGE);
      button.textContent = FUTURE_RELEASE_LABEL;
    });
  }

  function enforceSnakeDraft(event) {
    if (event?.target?.id !== 'create-league-form') return;
    const input = root.document?.getElementById?.('draft-type');
    if (input) input.value = SNAKE_DRAFT_TYPE;
  }

  function install() {
    configureDraftTypeControls();
    root.document?.addEventListener?.('submit', enforceSnakeDraft, true);
    root.document?.addEventListener?.('reset', (event) => {
      if (event?.target?.id !== 'create-league-form') return;
      root.setTimeout?.(configureDraftTypeControls, 0);
    }, true);
    root.document?.addEventListener?.('click', (event) => {
      if (!event?.target?.closest?.('.js-open-league')) return;
      root.setTimeout?.(() => {
        configureDraftTypeControls();
        setAvailabilityMessage();
      }, 0);
    }, true);
  }

  const helpers = Object.freeze({
    SNAKE_DRAFT_TYPE,
    FUTURE_RELEASE_LABEL,
    FUTURE_RELEASE_MESSAGE,
    normalizeDraftType,
    supportedDraftType,
    configureDraftTypeControls
  });

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  install();
  root.CFFSnakeDraftOnly = helpers;
  document.documentElement.dataset.cffSnakeDraftOnly = 'true';
})(typeof window !== 'undefined' ? window : globalThis);
