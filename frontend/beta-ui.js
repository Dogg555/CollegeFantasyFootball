(() => {
  'use strict';

  const pageName = window.location.pathname.split('/').pop() || 'index.html';
  const pageClass = `page-${pageName.replace(/\.html$/i, '').replace(/[^a-z0-9-]/gi, '-') || 'home'}`;

  function create(tag, className = '', text = '') {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text) element.textContent = text;
    return element;
  }

  function ensureMetaDescription() {
    if (document.querySelector('meta[name="description"]')) return;
    const meta = document.createElement('meta');
    meta.name = 'description';
    meta.content = pageName === 'signup.html'
      ? 'Create an account for the College Fantasy Football closed beta.'
      : 'Create private college fantasy football leagues, draft FBS players, and manage every Saturday.';
    document.head.appendChild(meta);
  }

  function improveSharedBranding() {
    document.body.classList.add('beta-ui', pageClass);
    const subtitle = document.querySelector('.brand__subtitle');
    if (subtitle) subtitle.textContent = 'Closed beta · 2026 season';

    const footerBrand = document.querySelector('.footer__brand span');
    if (footerBrand) footerBrand.textContent = 'Built for college football Saturdays.';
  }

  function addFieldFeedback(input, id, initialText) {
    if (!input || document.getElementById(id)) return null;
    const feedback = create('span', 'field-feedback', initialText);
    feedback.id = id;
    feedback.setAttribute('aria-live', 'polite');
    const describedBy = new Set((input.getAttribute('aria-describedby') || '').split(/\s+/).filter(Boolean));
    describedBy.add(id);
    input.setAttribute('aria-describedby', [...describedBy].join(' '));
    const field = input.closest('.field');
    field?.appendChild(feedback);
    return feedback;
  }

  function normalizeServiceStatus(status) {
    const original = String(status.textContent || '').trim();
    if (!original) return;
    status.title = status.title || original;
    status.classList.add('system-status');
    const hasError = /unreachable|unavailable|degraded|not configured|failed/i.test(original);
    status.classList.toggle('system-status--error', hasError);
    let next = original;
    if (/checking/i.test(original)) {
      next = 'Checking account services…';
    } else if (/auth ready|authentication is ready|account services are ready/i.test(original)) {
      next = 'Account services are ready.';
      status.classList.remove('system-status--error');
    } else if (/email.*not configured|verification.*not configured/i.test(original)) {
      next = 'Email verification is temporarily unavailable. Account services are being configured.';
    } else if (/unreachable|unavailable|degraded|failed|need attention/i.test(original)) {
      next = 'Account services need attention. Please try again shortly.';
    }
    if (status.textContent !== next) status.textContent = next;
  }

  function watchServiceStatus(status) {
    if (!status || status.dataset.betaObserved === 'true') return;
    status.dataset.betaObserved = 'true';
    normalizeServiceStatus(status);
    new MutationObserver(() => normalizeServiceStatus(status))
      .observe(status, { childList: true, characterData: true, subtree: true });
  }

  function enhanceSignup() {
    const form = document.getElementById('signup-form');
    const email = document.getElementById('signup-email');
    const password = document.getElementById('signup-password');
    if (!form || !email || !password || form.dataset.betaEnhanced === 'true') return;
    form.dataset.betaEnhanced = 'true';

    const card = form.closest('.auth-card');
    const header = card?.querySelector('.card__header');
    const heading = header?.querySelector('h2');
    const pill = header?.querySelector('.pill');
    if (heading) heading.textContent = 'Create your account';
    if (pill) pill.textContent = 'Closed beta';

    if (header && !card.querySelector('.auth-intro')) {
      const intro = create('div', 'auth-intro');
      intro.innerHTML = '<span class="auth-kicker">Your league starts here</span><p>Use one secure account for league invites, drafts, rosters, waivers, trades, and weekly scoring.</p>';
      header.insertAdjacentElement('afterend', intro);
    }

    let confirm = document.getElementById('signup-password-confirm');
    if (!confirm) {
      const confirmField = create('label', 'field');
      confirmField.innerHTML = '<span>Confirm password</span><input id="signup-password-confirm" type="password" name="passwordConfirm" placeholder="Re-enter your password" required autocomplete="new-password" aria-describedby="signup-confirm-feedback" />';
      password.closest('.field')?.insertAdjacentElement('afterend', confirmField);
      confirm = confirmField.querySelector('input');
    }

    confirm.minLength = password.minLength;
    confirm.maxLength = password.maxLength;

    const emailFeedback = addFieldFeedback(email, 'signup-email-feedback', 'Use the email where you want league invites and verification links.');
    const passwordFeedback = addFieldFeedback(password, 'signup-password-feedback', 'Use at least 12 characters. A phrase is easier to remember and harder to guess.');
    const confirmFeedback = document.getElementById('signup-confirm-feedback')
      || addFieldFeedback(confirm, 'signup-confirm-feedback', 'Passwords must match exactly.');

    const verificationNote = create('div', 'auth-notice');
    verificationNote.innerHTML = '<strong>Email verification required</strong><span>After creating the account, open the verification link sent to your inbox before signing in.</span>';
    form.querySelector('.actions')?.insertAdjacentElement('beforebegin', verificationNote);

    const terms = create('p', 'terms-copy');
    terms.innerHTML = 'By creating an account, you agree to the <a href="terms.html">Terms</a> and acknowledge the <a href="privacy.html">Privacy Policy</a>.';
    form.querySelector('.actions')?.insertAdjacentElement('afterend', terms);

    const submit = form.querySelector('[type="submit"]');
    if (submit) {
      submit.classList.add('auth-submit');
      submit.textContent = 'Create account';
    }

    const validateEmail = () => {
      const hasValue = Boolean(email.value.trim());
      const valid = !hasValue || email.validity.valid;
      email.classList.toggle('is-valid', hasValue && valid);
      emailFeedback?.classList.toggle('is-success', hasValue && valid);
      emailFeedback?.classList.toggle('is-error', hasValue && !valid);
      if (emailFeedback) {
        emailFeedback.textContent = !hasValue
          ? 'Use the email where you want league invites and verification links.'
          : valid ? 'Email format looks good.' : 'Enter a complete email address.';
      }
    };

    const validatePasswords = () => {
      const value = password.value;
      const min = Number(password.minLength || 12);
      confirm.minLength = password.minLength;
      confirm.maxLength = password.maxLength;
      const lengthOkay = value.length >= min;
      password.classList.toggle('is-valid', lengthOkay);
      if (passwordFeedback) {
        passwordFeedback.classList.toggle('is-success', lengthOkay);
        passwordFeedback.classList.toggle('is-error', Boolean(value) && !lengthOkay);
        passwordFeedback.textContent = !value
          ? `Use at least ${min} characters. A phrase is easier to remember and harder to guess.`
          : lengthOkay ? 'Password length meets the requirement.' : `${Math.max(0, min - value.length)} more character${min - value.length === 1 ? '' : 's'} required.`;
      }

      const mismatch = Boolean(confirm.value) && confirm.value !== value;
      confirm.setCustomValidity(mismatch ? 'Passwords do not match.' : '');
      confirm.classList.toggle('is-valid', Boolean(confirm.value) && !mismatch);
      confirmFeedback?.classList.toggle('is-success', Boolean(confirm.value) && !mismatch);
      confirmFeedback?.classList.toggle('is-error', mismatch);
      if (confirmFeedback) {
        confirmFeedback.textContent = !confirm.value
          ? 'Passwords must match exactly.'
          : mismatch ? 'Passwords do not match yet.' : 'Passwords match.';
      }
    };

    email.addEventListener('input', validateEmail);
    email.addEventListener('blur', validateEmail);
    password.addEventListener('input', validatePasswords);
    confirm.addEventListener('input', validatePasswords);
    validateEmail();
    validatePasswords();

    document.addEventListener('DOMContentLoaded', () => {
      const wrapper = confirm.closest('.password-field');
      const meter = wrapper?.nextElementSibling;
      if (meter?.classList.contains('password-meter')) meter.remove();
    }, { once: true });

    const sidecar = document.querySelector('.auth-sidecar');
    if (sidecar && !sidecar.querySelector('.auth-benefits')) {
      const sideHeading = sidecar.querySelector('h2');
      const sideCopy = sidecar.querySelector('p');
      if (sideHeading) sideHeading.textContent = 'Ready for kickoff';
      if (sideCopy) sideCopy.textContent = 'Your account keeps every private league, roster move, and draft pick tied to one manager profile.';
      const benefits = create('div', 'auth-benefits');
      benefits.innerHTML = [
        ['01', 'Join private leagues', 'Open invite links and request commissioner approval.'],
        ['02', 'Draft from one board', 'Queue FBS players and follow the live snake draft.'],
        ['03', 'Manage every week', 'Set lineups, submit waivers, trade, and track scoring.']
      ].map(([number, title, copy]) => `<div class="auth-benefit"><span>${number}</span><div><strong>${title}</strong><p>${copy}</p></div></div>`).join('');
      sideCopy?.insertAdjacentElement('afterend', benefits);
    }

    watchServiceStatus(document.getElementById('auth-api-status'));
    window.setTimeout(() => email.focus({ preventScroll: true }), 0);
  }

  function enhanceSignin() {
    const form = document.getElementById('login-form');
    if (!form || form.dataset.betaEnhanced === 'true') return;
    form.dataset.betaEnhanced = 'true';
    const card = form.closest('.auth-card');
    const heading = card?.querySelector('h2');
    if (heading) heading.textContent = 'Welcome back';
    if (card && !card.querySelector('.auth-intro')) {
      const intro = create('div', 'auth-intro');
      intro.innerHTML = '<span class="auth-kicker">Saturday starts here</span><p>Sign in to continue to your leagues, draft rooms, rosters, and matchups.</p>';
      card.querySelector('.card__header')?.insertAdjacentElement('afterend', intro);
    }
    form.querySelector('[type="submit"]')?.classList.add('auth-submit');
    watchServiceStatus(document.getElementById('auth-api-status'));
  }

  function groupLeagueTabs() {
    const tabs = [...document.querySelectorAll('.league-tab')];
    if (!tabs.length) return;
    const groups = {
      overview: 'League', scoreboard: 'League', standings: 'League', activity: 'League',
      team: 'Team', freeagency: 'Team', waivers: 'Team', trades: 'Team',
      managers: 'Manage', settings: 'Manage'
    };
    let previous = '';
    tabs.forEach((tab) => {
      const key = tab.dataset.leagueTab || (tab.tagName === 'A' ? 'draft' : '');
      const group = groups[key] || (key === 'draft' ? 'Draft' : 'League');
      tab.dataset.betaGroup = group.toLowerCase();
      tab.title = `${group}: ${tab.textContent.trim()}`;
      tab.setAttribute('aria-label', `${group}: ${tab.textContent.trim()}`);
      if (group !== previous) tab.classList.add('is-group-start');
      previous = group;
    });
  }

  function boot() {
    ensureMetaDescription();
    improveSharedBranding();
    if (pageName === 'signup.html') enhanceSignup();
    if (pageName === 'signin.html') enhanceSignin();
    if (pageName === 'league.html') groupLeagueTabs();
  }

  if (document.body) boot();
  else document.addEventListener('DOMContentLoaded', boot, { once: true });
})();
