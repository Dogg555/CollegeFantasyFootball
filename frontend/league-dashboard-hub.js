(function initLeagueDashboardHub(root) {
  'use strict';

  const CACHE_KEY = 'cff_league_dashboard_cache_v1';
  const VALIDATED_KEY = 'cff_league_dashboard_validated_v1';
  const MAX_CACHE_AGE_MS = 24 * 60 * 60 * 1000;

  function object(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
  }

  function array(value) {
    return Array.isArray(value) ? value : [];
  }

  function number(value, fallback = 0) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : fallback;
  }

  function normalizeDashboard(payload = {}) {
    const source = object(payload);
    const freshness = object(source.freshness);
    return {
      ...source,
      league: object(source.league),
      nextAction: object(source.nextAction),
      draft: object(source.draft),
      lineup: { ...object(source.lineup), warnings: array(source.lineup?.warnings) },
      currentMatchup: source.currentMatchup && typeof source.currentMatchup === 'object'
        ? source.currentMatchup
        : null,
      waivers: { ...object(source.waivers), items: array(source.waivers?.items) },
      trades: { ...object(source.trades), items: array(source.trades?.items) },
      standings: {
        ...object(source.standings),
        leaders: array(source.standings?.leaders),
        myTeam: source.standings?.myTeam && typeof source.standings.myTeam === 'object'
          ? source.standings.myTeam
          : null
      },
      activity: array(source.activity),
      commissionerNotices: array(source.commissionerNotices),
      deadlines: array(source.deadlines),
      freshness: {
        source: freshness.source || 'api',
        generatedAt: freshness.generatedAt || freshness.serverTime || '',
        serverTime: freshness.serverTime || freshness.generatedAt || '',
        stale: Boolean(freshness.stale),
        partial: Boolean(freshness.partial),
        unavailableSections: array(freshness.unavailableSections)
      },
      readOnly: Boolean(source.readOnly)
    };
  }

  function formatScore(value) {
    const parsed = number(value);
    return Number.isInteger(parsed) ? String(parsed) : parsed.toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
  }

  function formatDate(value, options = {}) {
    const parsed = new Date(value || '');
    if (Number.isNaN(parsed.getTime())) return '';
    return new Intl.DateTimeFormat(undefined, {
      month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit', ...options
    }).format(parsed);
  }

  function freshnessLabel(dashboard, now = Date.now()) {
    if (dashboard.readOnly || dashboard.freshness.stale) return 'Read-only cached dashboard';
    const generated = Date.parse(dashboard.freshness.generatedAt || '');
    if (!Number.isFinite(generated)) return dashboard.freshness.partial ? 'Partially available' : 'Server-authoritative';
    const minutes = Math.max(0, Math.floor((now - generated) / 60000));
    if (minutes < 1) return dashboard.freshness.partial ? 'Updated now · partial' : 'Updated now';
    return `Updated ${minutes}m ago${dashboard.freshness.partial ? ' · partial' : ''}`;
  }

  function matchupSummary(matchup) {
    if (!matchup) return { title: 'No matchup scheduled', detail: 'The commissioner has not generated a current matchup.' };
    const opponent = matchup.opponentTeamName || matchup.opponentEmail || 'Opponent';
    return {
      title: `Week ${number(matchup.week, 1)} vs ${opponent}`,
      detail: `${formatScore(matchup.scoreFor)}–${formatScore(matchup.scoreAgainst)} · ${matchup.status || 'scheduled'}`
    };
  }

  function lineupSummary(lineup) {
    const warnings = array(lineup.warnings);
    if (lineup.status === 'pre_draft') {
      return { title: 'Lineup opens after the draft', detail: `${number(lineup.rosterCount)} rostered players` };
    }
    if (warnings.length) {
      const missing = warnings.reduce((total, warning) => total + number(warning.missing), 0);
      return { title: `${missing} starter slot${missing === 1 ? '' : 's'} empty`, detail: warnings.map((item) => item.message).join(' ') };
    }
    return {
      title: lineup.lockStatus === 'locked' ? 'Lineup locked' : 'Lineup ready',
      detail: lineup.deadline ? `Deadline ${formatDate(lineup.deadline)}` : `${number(lineup.rosterCount)} rostered players`
    };
  }

  function standingsSummary(standings) {
    if (!standings.myTeam) return { title: 'Standings unavailable', detail: 'Standings appear after a scoring week is finalized.' };
    const team = standings.myTeam;
    return {
      title: `#${number(team.rank)} ${team.teamName || 'My Team'}`,
      detail: `${number(team.wins)}-${number(team.losses)}-${number(team.ties)} · ${formatScore(team.pointsFor)} PF`
    };
  }

  function dashboardViewModel(payload = {}, now = Date.now()) {
    const dashboard = normalizeDashboard(payload);
    const matchup = matchupSummary(dashboard.currentMatchup);
    const lineup = lineupSummary(dashboard.lineup);
    const standings = standingsSummary(dashboard.standings);
    const waiverCount = number(dashboard.waivers.pendingCount);
    const tradeCount = number(dashboard.trades.actionRequiredCount);
    return {
      dashboard,
      freshness: freshnessLabel(dashboard, now),
      nextAction: {
        label: dashboard.nextAction.label || 'Review your league',
        detail: dashboard.nextAction.detail || 'Open the league workspace to continue.',
        href: dashboard.nextAction.href || 'league.html'
      },
      matchup,
      lineup,
      standings,
      pending: {
        title: tradeCount || waiverCount ? `${tradeCount + waiverCount} item${tradeCount + waiverCount === 1 ? '' : 's'} need attention` : 'No pending actions',
        detail: `${tradeCount} trade${tradeCount === 1 ? '' : 's'} · ${waiverCount} waiver claim${waiverCount === 1 ? '' : 's'}`
      },
      deadlines: dashboard.deadlines
        .filter((item) => item?.at)
        .sort((left, right) => Date.parse(left.at) - Date.parse(right.at))
        .slice(0, 4),
      notices: dashboard.commissionerNotices.slice(0, 4),
      activity: dashboard.activity.slice(0, 6)
    };
  }

  function cacheScope(email, leagueId) {
    return `${String(email || '').trim().toLowerCase()}::${String(leagueId || '')}`;
  }

  function readStore(storage, key, fallback = {}) {
    try {
      return JSON.parse(storage?.getItem?.(key) || JSON.stringify(fallback));
    } catch {
      return fallback;
    }
  }

  function writeStore(storage, key, value) {
    try {
      storage?.setItem?.(key, JSON.stringify(value));
    } catch {
      // Read caching must never break the league page.
    }
  }

  function saveCache(storage, email, leagueId, payload, now = Date.now()) {
    const store = readStore(storage, CACHE_KEY, {});
    store[cacheScope(email, leagueId)] = { savedAt: now, payload: normalizeDashboard(payload) };
    writeStore(storage, CACHE_KEY, store);
  }

  function loadCache(storage, email, leagueId, now = Date.now()) {
    const cached = readStore(storage, CACHE_KEY, {})[cacheScope(email, leagueId)];
    if (!cached || now - number(cached.savedAt) > MAX_CACHE_AGE_MS) return null;
    const payload = normalizeDashboard(cached.payload);
    payload.readOnly = true;
    payload.freshness.stale = true;
    payload.freshness.source = 'cache';
    return payload;
  }

  function clearCache(storage, email, leagueId) {
    const store = readStore(storage, CACHE_KEY, {});
    const scope = cacheScope(email, leagueId);
    if (!Object.prototype.hasOwnProperty.call(store, scope)) return;
    delete store[scope];
    writeStore(storage, CACHE_KEY, store);
  }

  function isAuthorizationFailure(error) {
    return [401, 403, 404].includes(Number(error?.status));
  }

  const helpers = {
    normalizeDashboard,
    dashboardViewModel,
    freshnessLabel,
    matchupSummary,
    lineupSummary,
    standingsSummary,
    saveCache,
    loadCache,
    clearCache,
    cacheScope,
    isAuthorizationFailure,
    formatDate,
    readStore,
    writeStore,
    CACHE_KEY,
    VALIDATED_KEY
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  root.CFFLeagueDashboard = Object.freeze(helpers);
})(typeof window !== 'undefined' ? window : globalThis);
