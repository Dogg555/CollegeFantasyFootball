(function initScoreboardModule(root) {
  'use strict';

  const SLIDE_SIZE = 4;
  const ROTATION_MS = 8000;

  function toNumber(value, fallback = 0) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : fallback;
  }

  function formatAge(value) {
    const seconds = Number(value);
    if (!Number.isFinite(seconds) || seconds < 0) return 'unknown';
    if (seconds < 60) return `${Math.round(seconds)} sec ago`;
    if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
    if (seconds < 86400) return `${Math.round(seconds / 3600)} hr ago`;
    const days = Math.round(seconds / 86400);
    return `${days} day${days === 1 ? '' : 's'} ago`;
  }

  function parseStartDate(value) {
    if (!value) return null;
    const parsed = new Date(value);
    return Number.isNaN(parsed.getTime()) ? null : parsed;
  }

  function normalizeGame(game = {}, index = 0) {
    const startDate = parseStartDate(game.startDate || game.start_date);
    const status = String(game.status || 'scheduled');
    const normalizedStatus = status.toLowerCase();
    const quarter = toNumber(game.quarter ?? game.period, 0);
    const live = Boolean(game.live) || (
      quarter > 0 &&
      !normalizedStatus.includes('final') &&
      !normalizedStatus.includes('complete')
    );

    return {
      id: String(game.id || `game-${index}`),
      season: toNumber(game.season, 0),
      week: toNumber(game.week, 0),
      startDate,
      away: String(game.away || game.awayTeam || 'Away'),
      home: String(game.home || game.homeTeam || 'Home'),
      awayScore: toNumber(game.awayScore ?? game.awayPoints, 0),
      homeScore: toNumber(game.homeScore ?? game.homePoints, 0),
      quarter,
      clock: String(game.clock || ''),
      status,
      live,
    };
  }

  function availableWeeks(games) {
    return [...new Set(games.map((game) => game.week).filter((week) => week > 0))]
      .sort((a, b) => a - b);
  }

  function chooseDefaultWeek(games, now = new Date()) {
    const liveGame = games.find((game) => game.live && game.week > 0);
    if (liveGame) return liveGame.week;

    const dated = games
      .filter((game) => game.week > 0 && game.startDate)
      .sort((a, b) => Math.abs(a.startDate - now) - Math.abs(b.startDate - now));
    if (dated.length) return dated[0].week;

    return availableWeeks(games)[0] || 0;
  }

  function timeGroupKey(game) {
    if (!game.startDate) return 'tbd';
    const date = game.startDate;
    return [
      date.getFullYear(),
      String(date.getMonth() + 1).padStart(2, '0'),
      String(date.getDate()).padStart(2, '0'),
      String(date.getHours()).padStart(2, '0'),
      String(date.getMinutes()).padStart(2, '0'),
    ].join('-');
  }

  function timeGroupLabel(game) {
    if (!game.startDate) return 'Kickoff time TBD';
    return new Intl.DateTimeFormat(undefined, {
      weekday: 'short',
      month: 'short',
      day: 'numeric',
      hour: 'numeric',
      minute: '2-digit',
    }).format(game.startDate);
  }

  function buildSlides(games, week, slideSize = SLIDE_SIZE) {
    const selected = games
      .filter((game) => game.week === week)
      .sort((a, b) => {
        const aTime = a.startDate ? a.startDate.getTime() : Number.MAX_SAFE_INTEGER;
        const bTime = b.startDate ? b.startDate.getTime() : Number.MAX_SAFE_INTEGER;
        return aTime - bTime || a.away.localeCompare(b.away);
      });

    const groups = new Map();
    selected.forEach((game) => {
      const key = timeGroupKey(game);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(game);
    });

    const slides = [];
    groups.forEach((groupGames) => {
      for (let offset = 0; offset < groupGames.length; offset += slideSize) {
        slides.push({
          label: timeGroupLabel(groupGames[0]),
          games: groupGames.slice(offset, offset + slideSize),
          page: Math.floor(offset / slideSize) + 1,
          pages: Math.ceil(groupGames.length / slideSize),
        });
      }
    });
    return slides;
  }

  const helpers = {
    normalizeGame,
    availableWeeks,
    chooseDefaultWeek,
    buildSlides,
    timeGroupKey,
    formatAge,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = helpers;
  }

  if (typeof document === 'undefined') return;

  const apiBase = root.CFF_API_BASE || '/api';
  const allowLocalDemo = root.CFF_ALLOW_LOCAL_DEMO !== false;
  const scoreList = document.getElementById('live-scores');
  const weekSelect = document.getElementById('score-week-select');
  const weekPrev = document.getElementById('score-week-prev');
  const weekNext = document.getElementById('score-week-next');
  const slidePrev = document.getElementById('score-slide-prev');
  const slideNext = document.getElementById('score-slide-next');
  const rotationToggle = document.getElementById('score-rotation-toggle');
  const meta = document.getElementById('scoreboard-meta');
  const freshness = document.getElementById('scoreboard-freshness');
  const dots = document.getElementById('scoreboard-dots');

  if (!scoreList) return;

  let games = [];
  let selectedWeek = 0;
  let slides = [];
  let slideIndex = 0;
  let rotationTimer = null;
  let rotationEnabled = !root.matchMedia?.('(prefers-reduced-motion: reduce)').matches;

  function safeText(value) {
    if (typeof root.escapeHtml === 'function') return root.escapeHtml(value ?? '');
    const element = document.createElement('div');
    element.textContent = String(value ?? '');
    return element.innerHTML;
  }

  function gameStatus(game) {
    const normalized = game.status.toLowerCase();
    if (game.live) {
      if (normalized.includes('half')) return 'Halftime';
      const quarter = game.quarter > 0 ? `Q${game.quarter}` : 'Live';
      return game.clock ? `${quarter} / ${game.clock}` : quarter;
    }
    if (normalized.includes('final') || normalized.includes('complete')) return 'Final';
    return game.startDate
      ? new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' }).format(game.startDate)
      : 'Time TBD';
  }

  function scoreMarkup(game) {
    const normalized = game.status.toLowerCase();
    const hasScore = game.live || normalized.includes('final') || normalized.includes('complete');
    return hasScore
      ? `<div class="scoreboard-game__score">${game.awayScore}<span>-</span>${game.homeScore}</div>`
      : '<div class="scoreboard-game__score scoreboard-game__score--scheduled">vs</div>';
  }

  function renderWeekOptions() {
    if (!weekSelect) return;
    const weeks = availableWeeks(games);
    weekSelect.innerHTML = weeks.map((week) => (
      `<option value="${week}"${week === selectedWeek ? ' selected' : ''}>Week ${week}</option>`
    )).join('');
    weekSelect.disabled = weeks.length <= 1;
    if (weekPrev) weekPrev.disabled = weeks.indexOf(selectedWeek) <= 0;
    if (weekNext) weekNext.disabled = weeks.indexOf(selectedWeek) >= weeks.length - 1;
  }

  function renderDots() {
    if (!dots) return;
    dots.innerHTML = slides.map((_, index) => (
      `<button type="button" class="scoreboard-dot${index === slideIndex ? ' is-active' : ''}" data-slide-index="${index}" aria-label="Show kickoff group ${index + 1}" aria-current="${index === slideIndex ? 'true' : 'false'}"></button>`
    )).join('');
    dots.querySelectorAll('[data-slide-index]').forEach((button) => {
      button.addEventListener('click', () => {
        slideIndex = Number(button.dataset.slideIndex) || 0;
        renderSlide();
        restartRotation();
      });
    });
  }

  function renderSlide() {
    if (!slides.length) {
      scoreList.innerHTML = '<div class="scoreboard-empty">No games are available for this week yet.</div>';
      if (meta) meta.textContent = selectedWeek ? `Week ${selectedWeek}` : 'Schedule unavailable';
      if (dots) dots.innerHTML = '';
      return;
    }

    slideIndex = ((slideIndex % slides.length) + slides.length) % slides.length;
    const slide = slides[slideIndex];
    const pageLabel = slide.pages > 1 ? ` / group ${slide.page} of ${slide.pages}` : '';
    if (meta) {
      meta.textContent = `Week ${selectedWeek} / ${slide.label}${pageLabel} / ${slideIndex + 1} of ${slides.length}`;
    }

    scoreList.innerHTML = `<div class="scoreboard-games">${slide.games.map((game) => `
      <article class="scoreboard-game${game.live ? ' is-live' : ''}">
        <div class="scoreboard-game__status">
          <span class="${game.live ? 'live-indicator' : ''}">${safeText(gameStatus(game))}</span>
        </div>
        <div class="scoreboard-game__matchup">
          <div><span class="scoreboard-game__team">${safeText(game.away)}</span><span class="scoreboard-game__side">Away</span></div>
          ${scoreMarkup(game)}
          <div><span class="scoreboard-game__team">${safeText(game.home)}</span><span class="scoreboard-game__side">Home</span></div>
        </div>
      </article>
    `).join('')}</div>`;

    if (slidePrev) slidePrev.disabled = slides.length <= 1;
    if (slideNext) slideNext.disabled = slides.length <= 1;
    renderDots();
  }

  function selectWeek(week) {
    selectedWeek = toNumber(week, selectedWeek);
    slides = buildSlides(games, selectedWeek);
    slideIndex = 0;
    renderWeekOptions();
    renderSlide();
    restartRotation();
  }

  function stepWeek(direction) {
    const weeks = availableWeeks(games);
    const index = weeks.indexOf(selectedWeek);
    const target = weeks[index + direction];
    if (target) selectWeek(target);
  }

  function stepSlide(direction) {
    if (slides.length <= 1) return;
    slideIndex = (slideIndex + direction + slides.length) % slides.length;
    renderSlide();
    restartRotation();
  }

  function stopRotation() {
    if (rotationTimer) root.clearInterval(rotationTimer);
    rotationTimer = null;
  }

  function startRotation() {
    stopRotation();
    if (!rotationEnabled || slides.length <= 1 || document.hidden) return;
    rotationTimer = root.setInterval(() => {
      slideIndex = (slideIndex + 1) % slides.length;
      renderSlide();
    }, ROTATION_MS);
  }

  function restartRotation() {
    startRotation();
    if (rotationToggle) {
      rotationToggle.textContent = rotationEnabled ? 'Pause rotation' : 'Resume rotation';
      rotationToggle.setAttribute('aria-pressed', String(!rotationEnabled));
    }
  }

  function renderLiveScores(payload = [], fallback = false) {
    if (fallback && !allowLocalDemo) {
      games = [];
      scoreList.innerHTML = '<div class="scoreboard-empty">The schedule is temporarily unavailable.</div>';
      if (meta) meta.textContent = 'Cached schedule unavailable';
      return;
    }

    games = (Array.isArray(payload) ? payload : [])
      .map(normalizeGame)
      .filter((game) => game.week > 0 || game.startDate);

    if (!games.length) {
      scoreList.innerHTML = '<div class="scoreboard-empty">No games have been cached for the selected week.</div>';
      if (meta) meta.textContent = 'Waiting for the next schedule refresh';
      if (weekSelect) weekSelect.innerHTML = '';
      stopRotation();
      return;
    }

    selectedWeek = chooseDefaultWeek(games);
    selectWeek(selectedWeek);
  }

  function renderFreshness(payload = {}) {
    if (!freshness) return;
    const age = formatAge(payload.ageSeconds);
    const scheduleAge = formatAge(payload.scheduleAgeSeconds);
    const liveCount = Number(payload.liveGameCount || 0);
    const stale = payload.fresh === false;
    freshness.innerHTML = `<span class="data-freshness${stale ? ' is-stale' : ''}">${stale ? 'Score cache delayed' : 'Score cache current'} &middot; ${age}</span><span>${Number(payload.scheduleGameCount || 0)} scheduled games &middot; schedule ${scheduleAge}${liveCount ? ` &middot; ${liveCount} live` : ''}</span>`;
  }

  async function loadScoreboard() {
    scoreList.innerHTML = '<div class="scoreboard-empty">Loading the weekly schedule...</div>';
    try {
      const response = await root.fetch(`${apiBase}/scores/live`, {
        headers: { Accept: 'application/json' },
      });
      if (!response.ok) throw new Error(`Scoreboard request failed with ${response.status}`);
      renderLiveScores(await response.json(), false);
      try {
        const metaResponse = await root.fetch(`${apiBase}/scores/live/meta`, { headers: { Accept: 'application/json' } });
        if (metaResponse.ok) renderFreshness(await metaResponse.json());
        else if (freshness) freshness.textContent = 'Schedule freshness unavailable';
      } catch {
        if (freshness) freshness.textContent = 'Schedule freshness unavailable';
      }
    } catch (error) {
      if (allowLocalDemo && Array.isArray(root.sampleScores)) {
        renderLiveScores(root.sampleScores, true);
      } else {
        renderLiveScores([], true);
      }
    }
  }

  root.renderLiveScores = renderLiveScores;
  root.CFF_SCOREBOARD = helpers;

  weekSelect?.addEventListener('change', () => selectWeek(weekSelect.value));
  weekPrev?.addEventListener('click', () => stepWeek(-1));
  weekNext?.addEventListener('click', () => stepWeek(1));
  slidePrev?.addEventListener('click', () => stepSlide(-1));
  slideNext?.addEventListener('click', () => stepSlide(1));
  rotationToggle?.addEventListener('click', () => {
    rotationEnabled = !rotationEnabled;
    restartRotation();
  });

  document.addEventListener('visibilitychange', restartRotation);
  scoreList.addEventListener('mouseenter', stopRotation);
  scoreList.addEventListener('mouseleave', startRotation);
  scoreList.addEventListener('focusin', stopRotation);
  scoreList.addEventListener('focusout', startRotation);

  loadScoreboard();
})(typeof window !== 'undefined' ? window : globalThis);
