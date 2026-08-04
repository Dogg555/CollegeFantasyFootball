(() => {
  'use strict';

  const page = window.location.pathname.split('/').pop() || 'index.html';
  if (page !== 'index.html') return;

  function enhanceHero() {
    document.body.classList.add('page-home');
    const hero = document.querySelector('.home-hero');
    const copy = hero?.querySelector('.hero__copy');
    const cta = copy?.querySelector('.cta-row');
    if (!hero || !copy || !cta) return;

    cta.classList.add('hero-cta-group');
    const primary = cta.querySelector('.js-open-league');
    if (primary) primary.textContent = 'Create your league';

    const login = cta.querySelector('a[href="signin.html"]');
    if (login) login.textContent = 'Sign in';

    if (!cta.querySelector('[data-join-league]')) {
      const join = document.createElement('a');
      join.className = 'button button--ghost';
      join.href = 'league.html';
      join.dataset.joinLeague = 'true';
      join.textContent = 'Join a league';
      cta.insertBefore(join, login || null);
    }

    if (!cta.querySelector('a[href="players.html"]')) {
      const players = document.createElement('a');
      players.className = 'button button--ghost';
      players.href = 'players.html';
      players.textContent = 'Browse players';
      cta.appendChild(players);
    }

    const existingHint = cta.nextElementSibling;
    if (existingHint?.classList.contains('muted')) existingHint.remove();

    if (!copy.querySelector('.hero-proof')) {
      const proof = document.createElement('div');
      proof.className = 'hero-proof';
      proof.innerHTML = '<span>Private leagues</span><span>Live draft room</span><span>Full-season management</span>';
      cta.insertAdjacentElement('afterend', proof);
    }

    const preview = hero.querySelector('.product-shot--hero');
    if (preview && !preview.querySelector('.hero-preview-status')) {
      const status = document.createElement('div');
      status.className = 'hero-preview-status';
      status.innerHTML = '<span>8 managers</span><span>18 roster spots</span><span>Week 1 ready</span>';
      preview.querySelector('.product-shot__top')?.insertAdjacentElement('afterend', status);
    }
  }

  function simplifyFeatures() {
    const tiles = [...document.querySelectorAll('.feature-grid .feature-tile')];
    const keep = new Set(['Live Scoring', 'Draft Rooms', 'Commissioner Tools', 'Mobile Friendly']);
    tiles.forEach((tile) => {
      const title = tile.querySelector('h3')?.textContent.trim();
      if (title && !keep.has(title)) tile.hidden = true;
    });
  }

  function rebuildJourney() {
    const list = document.querySelector('.steps-list');
    if (!list) return;
    const steps = [
      ['1', 'Create a league', 'Choose size, scoring, roster rules, and draft settings.'],
      ['2', 'Invite managers', 'Share the league and approve the people joining your season.'],
      ['3', 'Draft players', 'Build a queue, follow the clock, and fill every roster slot.'],
      ['4', 'Set lineups', 'Manage starters, waivers, trades, and weekly roster decisions.'],
      ['5', 'Compete weekly', 'Follow Saturday scoring, matchups, standings, and playoffs.']
    ];
    list.innerHTML = steps.map(([number, title, copy]) =>
      `<div class="step-item"><span>${number}</span><strong>${title}</strong><p>${copy}</p></div>`
    ).join('');
  }

  function addBetaBadge() {
    const brand = document.querySelector('.footer--rich .footer__brand');
    if (!brand || brand.querySelector('.footer-beta-badge')) return;
    const badge = document.createElement('span');
    badge.className = 'footer-beta-badge';
    badge.textContent = 'Closed Beta 2026';
    brand.appendChild(badge);
  }

  function boot() {
    enhanceHero();
    simplifyFeatures();
    rebuildJourney();
    addBetaBadge();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
