const CFF_AUTH_KEY = 'cff_auth';
const CFF_LEAGUE_KEY = 'cff_league';
const CFF_LEAGUES_KEY = 'cff_leagues_by_account';
const CFF_QUEUE_KEY = 'cff_draft_queue';
const CFF_ROSTER_KEY = 'cff_roster';
const CFF_WAIVERS_KEY = 'cff_waivers_by_league';
const CFF_WAIVER_PRIORITIES_KEY = 'cff_waiver_priorities_by_league';
const CFF_TRADES_KEY = 'cff_trades_by_league';
const CFF_TRANSACTIONS_KEY = 'cff_transactions_by_league';
const CFF_MATCHUPS_KEY = 'cff_matchups_by_league';
const CFF_DRAFT_PICKS_KEY = 'cff_draft_picks_by_league';
const CFF_DRAFT_META_KEY = 'cff_draft_meta_by_league';
const CFF_API_CACHE_META_KEY = 'cff_api_cache_meta';
const MAX_LEAGUES_PER_ACCOUNT = 3;
const CFF_API_BASE = window.CFF_API_BASE || '/api';
const CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO === true;
let lastAuthSessionResult = { authenticated: false, unavailable: false, expired: false };
let apiServiceUnavailable = false;

function localhostDemoAllowed() {
  const host = window.location.hostname;
  return CFF_ALLOW_LOCAL_DEMO && (host === 'localhost' || host === '127.0.0.1' || host === '::1');
}

function isLocalDemoSession() {
  const token = getAuthState()?.token || '';
  return localhostDemoAllowed() && String(token).startsWith('local-demo-');
}

const samplePlayers = [
  { id: 'p-001', name: 'Garrett Nussmeier', team: 'LSU', position: 'QB', conference: 'SEC', class: 'SR', rank: 1, projection: 24.8 },
  { id: 'p-002', name: 'Jeremiyah Love', team: 'Notre Dame', position: 'RB', conference: 'Independent', class: 'JR', rank: 2, projection: 21.9 },
  { id: 'p-003', name: 'Ryan Williams', team: 'Alabama', position: 'WR', conference: 'SEC', class: 'SO', rank: 3, projection: 20.7 },
  { id: 'p-004', name: 'Cade Klubnik', team: 'Clemson', position: 'QB', conference: 'ACC', class: 'SR', rank: 4, projection: 23.1 },
  { id: 'p-005', name: 'Nicholas Singleton', team: 'Penn State', position: 'RB', conference: 'Big Ten', class: 'SR', rank: 5, projection: 19.6 },
  { id: 'p-006', name: 'Carnell Tate', team: 'Ohio State', position: 'WR', conference: 'Big Ten', class: 'SR', rank: 6, projection: 18.8 },
  { id: 'p-007', name: 'Dylan Raiola', team: 'Nebraska', position: 'QB', conference: 'Big Ten', class: 'JR', rank: 7, projection: 21.4 },
  { id: 'p-008', name: 'Makhi Hughes', team: 'Oregon', position: 'RB', conference: 'Big Ten', class: 'SR', rank: 8, projection: 18.9 },
  { id: 'p-009', name: 'Kevin Concepcion', team: 'Texas A&M', position: 'WR', conference: 'SEC', class: 'SR', rank: 9, projection: 18.3 },
  { id: 'p-010', name: 'Eli Stowers', team: 'Vanderbilt', position: 'TE', conference: 'SEC', class: 'SR', rank: 10, projection: 13.7 },
  { id: 'p-011', name: 'Arch Manning', team: 'Texas', position: 'QB', conference: 'SEC', class: 'JR', rank: 11, projection: 22.6 },
  { id: 'p-012', name: 'CJ Baxter', team: 'Texas', position: 'RB', conference: 'SEC', class: 'JR', rank: 12, projection: 17.5 }
];

const sampleScores = [
  { away: 'Oregon', home: 'Ohio State', quarter: 2, clock: '07:18', awayScore: 17, homeScore: 14 },
  { away: 'LSU', home: 'Alabama', quarter: 3, clock: '11:02', awayScore: 24, homeScore: 24 },
  { away: 'Clemson', home: 'Florida State', quarter: 1, clock: '02:44', awayScore: 10, homeScore: 7 }
];

const defaultRosterRules = {
  qb: 1,
  rb: 2,
  wr: 2,
  te: 1,
  flex: 2,
  bench: 6
};

const defaultWaiverRules = {
  mode: 'free_agency',
  claimDeadline: '',
  freeAgencyLocked: false
};

const defaultTradeRules = {
  commissionerApproval: false,
  expirationHours: 48
};

const scoringPresets = {
  ppr: {
    passingYardsPerPoint: 25,
    passingTd: 4,
    interception: -2,
    rushingYardsPerPoint: 10,
    rushingTd: 6,
    receivingYardsPerPoint: 10,
    receivingTd: 6,
    reception: 1,
    fumbleLost: -2,
    twoPointConversion: 2
  },
  half_ppr: {
    passingYardsPerPoint: 25,
    passingTd: 4,
    interception: -2,
    rushingYardsPerPoint: 10,
    rushingTd: 6,
    receivingYardsPerPoint: 10,
    receivingTd: 6,
    reception: 0.5,
    fumbleLost: -2,
    twoPointConversion: 2
  },
  standard: {
    passingYardsPerPoint: 25,
    passingTd: 4,
    interception: -2,
    rushingYardsPerPoint: 10,
    rushingTd: 6,
    receivingYardsPerPoint: 10,
    receivingTd: 6,
    reception: 0,
    fumbleLost: -2,
    twoPointConversion: 2
  }
};

function readJson(key, fallback = null) {
  try {
    const raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) : fallback;
  } catch {
    return fallback;
  }
}

function writeJson(key, value) {
  localStorage.setItem(key, JSON.stringify(value));
}

function readSessionJson(key, fallback = null) {
  try {
    const raw = sessionStorage.getItem(key);
    return raw ? JSON.parse(raw) : fallback;
  } catch {
    return fallback;
  }
}

function writeSessionJson(key, value) {
  sessionStorage.setItem(key, JSON.stringify(value));
}

function getAuthState() {
  const current = readSessionJson(CFF_AUTH_KEY, null);
  if (current) return current;
  const legacy = readJson(CFF_AUTH_KEY, null);
  if (legacy) {
    writeSessionJson(CFF_AUTH_KEY, legacy);
    localStorage.removeItem(CFF_AUTH_KEY);
  }
  return legacy;
}

function authHeaders() {
  const auth = getAuthState();
  return auth?.token ? { Authorization: `Bearer ${auth.token}` } : {};
}

async function apiRequest(path, options = {}) {
  const headers = {
    ...(options.body ? { 'Content-Type': 'application/json' } : {}),
    ...authHeaders(),
    ...(options.headers || {})
  };
  let resp;
  try {
    resp = await fetch(`${CFF_API_BASE}${path}`, { ...options, headers });
  } catch (error) {
    apiServiceUnavailable = true;
    throw error;
  }
  let data = null;
  try {
    data = await resp.json();
  } catch {
    data = null;
  }
  if (!resp.ok) {
    const err = new Error(data?.error || `Request failed with ${resp.status}`);
    err.status = resp.status;
    err.data = data;
    err.retryAfter = resp.headers.get('Retry-After') || '';
    if (resp.status === 503) apiServiceUnavailable = true;
    throw err;
  }
  apiServiceUnavailable = false;
  return data;
}

function mutationErrorMessage(error, fallback = 'Request failed. No local changes were made.') {
  if (error?.status === 429) {
    const retry = error.retryAfter ? ` Retry after ${error.retryAfter} seconds.` : ' Try again later.';
    return `Too many requests.${retry}`;
  }
  if (error?.status === 503 || error?.unavailable) {
    return 'Service is temporarily unavailable. No local changes were made.';
  }
  return error?.data?.error || error?.message || fallback;
}

function writeApiCacheMeta(scope, leagueId = getLeagueState()?.id || '') {
  const meta = readJson(CFF_API_CACHE_META_KEY, {});
  meta[scope] = {
    schemaVersion: 1,
    source: 'api',
    fetchedAt: new Date().toISOString(),
    leagueId,
    stale: false
  };
  writeJson(CFF_API_CACHE_META_KEY, meta);
}

function markApiCacheStale(scope = 'league') {
  const meta = readJson(CFF_API_CACHE_META_KEY, {});
  const current = meta[scope] || { schemaVersion: 1, source: 'api', fetchedAt: '', leagueId: getLeagueState()?.id || '' };
  meta[scope] = { ...current, stale: true };
  writeJson(CFF_API_CACHE_META_KEY, meta);
}

function apiCacheMeta(scope = 'league') {
  return readJson(CFF_API_CACHE_META_KEY, {})[scope] || null;
}

function mutationControlsDisabled() {
  return Boolean(getAuthState()?.token && !isLocalDemoSession() && apiServiceUnavailable);
}

function setAuthState(auth) {
  localStorage.removeItem(CFF_AUTH_KEY);
  if (!auth) {
    sessionStorage.removeItem(CFF_AUTH_KEY);
    return;
  }
  writeSessionJson(CFF_AUTH_KEY, auth);
}

async function validateAuthSession() {
  const result = await validateAuthSessionResult();
  return result.authenticated === true;
}

async function validateAuthSessionResult() {
  const auth = getAuthState();
  if (!auth?.token) {
    lastAuthSessionResult = { authenticated: false, unavailable: false, expired: false };
    return lastAuthSessionResult;
  }
  if (String(auth.token).startsWith('local-demo-')) {
    if (localhostDemoAllowed()) {
      lastAuthSessionResult = { authenticated: true, unavailable: false, expired: false, demo: true };
      return lastAuthSessionResult;
    }
    clearSessionState();
    lastAuthSessionResult = { authenticated: false, unavailable: false, expired: true };
    return lastAuthSessionResult;
  }
  try {
    const data = await apiRequest('/auth/validate');
    if (data?.valid) {
      if (data.email && data.email !== auth.email) {
        setAuthState({ ...auth, email: data.email });
      }
      lastAuthSessionResult = { authenticated: true, unavailable: false, expired: false, email: data.email || auth.email };
      return lastAuthSessionResult;
    }
    clearSessionState();
    lastAuthSessionResult = { authenticated: false, unavailable: false, expired: true };
    return lastAuthSessionResult;
  } catch (error) {
    if (error.status === 401 || error.status === 403) {
      clearSessionState();
      lastAuthSessionResult = { authenticated: false, unavailable: false, expired: true, status: error.status };
      return lastAuthSessionResult;
    }
    lastAuthSessionResult = { authenticated: false, unavailable: true, expired: false, status: error.status || 0, message: error.message };
    apiServiceUnavailable = true;
    return lastAuthSessionResult;
  }
}

function getLeagueState() {
  migrateSingleLeague();
  const auth = getAuthState();
  if (!auth?.email) {
    return readJson(CFF_LEAGUE_KEY, null);
  }
  const account = getAccountLeagueState(auth.email);
  return account.leagues.find((league) => league.id === account.activeLeagueId) || account.leagues[0] || null;
}

function setLeagueState(league) {
  migrateSingleLeague();
  if (!league) {
    const auth = getAuthState();
    if (!auth?.email) {
      localStorage.removeItem(CFF_LEAGUE_KEY);
      return;
    }
    const store = getLeaguesStore();
    store[auth.email] = { leagues: [], activeLeagueId: null };
    writeJson(CFF_LEAGUES_KEY, store);
    return;
  }
  saveLeagueForAccount(league);
}

function getLeaguesForCurrentAccount() {
  migrateSingleLeague();
  const auth = getAuthState();
  if (!auth?.email) {
    const league = readJson(CFF_LEAGUE_KEY, null);
    return league ? [normalizeLeague(league)] : [];
  }
  return getAccountLeagueState(auth.email).leagues;
}

function saveLeagueForAccount(league) {
  const normalized = normalizeLeague(league);
  const auth = getAuthState();
  if (!auth?.email) {
    writeJson(CFF_LEAGUE_KEY, normalized);
    return { ok: true, league: normalized, count: 1 };
  }
  const store = getLeaguesStore();
  const account = store[auth.email] || { leagues: [], activeLeagueId: null };
  const existingIndex = account.leagues.findIndex((item) => item.id === normalized.id);
  if (existingIndex === -1 && account.leagues.length >= MAX_LEAGUES_PER_ACCOUNT) {
    return { ok: false, error: `Each account can have up to ${MAX_LEAGUES_PER_ACCOUNT} leagues.`, count: account.leagues.length };
  }
  if (existingIndex >= 0) {
    account.leagues[existingIndex] = normalized;
  } else {
    account.leagues.push(normalized);
  }
  account.activeLeagueId = normalized.id;
  store[auth.email] = account;
  writeJson(CFF_LEAGUES_KEY, store);
  localStorage.removeItem(CFF_LEAGUE_KEY);
  return { ok: true, league: normalized, count: account.leagues.length };
}

function setActiveLeague(leagueId) {
  const auth = getAuthState();
  if (!auth?.email) return;
  const store = getLeaguesStore();
  const account = store[auth.email] || { leagues: [], activeLeagueId: null };
  if (account.leagues.some((league) => league.id === leagueId)) {
    account.activeLeagueId = leagueId;
    store[auth.email] = account;
    writeJson(CFF_LEAGUES_KEY, store);
  }
}

function replaceLeaguesForCurrentAccount(leagues = []) {
  const auth = getAuthState();
  if (!auth?.email) return;
  const normalized = leagues.map(normalizeLeague);
  const current = getAccountLeagueState(auth.email);
  const activeStillExists = normalized.some((league) => league.id === current.activeLeagueId);
  const store = getLeaguesStore();
  store[auth.email] = {
    leagues: normalized,
    activeLeagueId: activeStillExists ? current.activeLeagueId : normalized[0]?.id || null
  };
  writeJson(CFF_LEAGUES_KEY, store);
}

function removeLeagueForCurrentAccount(leagueId) {
  const auth = getAuthState();
  if (!auth?.email) {
    localStorage.removeItem(CFF_LEAGUE_KEY);
    return;
  }
  const store = getLeaguesStore();
  const account = store[auth.email] || { leagues: [], activeLeagueId: null };
  account.leagues = account.leagues.filter((league) => league.id !== leagueId);
  if (account.activeLeagueId === leagueId) {
    account.activeLeagueId = account.leagues[0]?.id || null;
  }
  store[auth.email] = account;
  writeJson(CFF_LEAGUES_KEY, store);
}

function canCreateLeague() {
  return getLeaguesForCurrentAccount().length < MAX_LEAGUES_PER_ACCOUNT;
}

function getLeaguesStore() {
  return readJson(CFF_LEAGUES_KEY, {});
}

function getAccountLeagueState(email) {
  const store = getLeaguesStore();
  const account = store[email] || { leagues: [], activeLeagueId: null };
  account.leagues = (account.leagues || []).map(normalizeLeague);
  account.activeLeagueId = account.activeLeagueId || account.leagues[0]?.id || null;
  return account;
}

function migrateSingleLeague() {
  const auth = getAuthState();
  const single = readJson(CFF_LEAGUE_KEY, null);
  if (!auth?.email || !single) return;
  const store = getLeaguesStore();
  const account = store[auth.email] || { leagues: [], activeLeagueId: null };
  const normalized = normalizeLeague(single);
  if (!account.leagues.some((league) => league.id === normalized.id)) {
    account.leagues.push(normalized);
  }
  account.activeLeagueId = account.activeLeagueId || normalized.id;
  store[auth.email] = account;
  writeJson(CFF_LEAGUES_KEY, store);
  localStorage.removeItem(CFF_LEAGUE_KEY);
}

function getQueue() {
  return readJson(CFF_QUEUE_KEY, []);
}

function setQueue(queue) {
  writeJson(CFF_QUEUE_KEY, queue);
}

function getRoster() {
  return readJson(CFF_ROSTER_KEY, []);
}

function setRoster(roster) {
  writeJson(CFF_ROSTER_KEY, roster);
}

function clearSessionState() {
  sessionStorage.removeItem(CFF_AUTH_KEY);
  [CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY, CFF_ROSTER_KEY,
   CFF_WAIVERS_KEY, CFF_WAIVER_PRIORITIES_KEY, CFF_TRADES_KEY, CFF_TRANSACTIONS_KEY,
   CFF_MATCHUPS_KEY, CFF_DRAFT_PICKS_KEY, CFF_DRAFT_META_KEY]
    .forEach((key) => localStorage.removeItem(key));
}

function normalizePlayer(player) {
  return {
    id: String(player.id || player.athleteId || `player-${player.name || Math.random()}`),
    name: player.name || player.fullName || 'Unknown player',
    team: player.team || player.school || 'Team TBD',
    position: player.position || 'FLEX',
    conference: player.conference || 'Conference TBD',
    class: player.class || player.year || 'Class TBD',
    rank: Number(player.rank || 99),
    projection: Number(player.projection || player.projectedPoints || 10),
    rosterSlot: player.rosterSlot || player.roster_slot || ''
  };
}

function normalizeLeague(league = {}) {
  const invitedEmails = Array.isArray(league.invitedEmails) ? league.invitedEmails : [];
  const members = normalizeMembers(league.members, invitedEmails);
  return {
    id: league.id || `local-${Date.now().toString(36)}`,
    name: league.name || 'College Saturdays',
    teams: Number(league.teams || 10),
    scoring: league.scoring || 'ppr',
    scoringLabel: league.scoringLabel || scoringLabel(league.scoring || 'ppr'),
    draftType: league.draftType || 'snake',
    draftTypeLabel: league.draftTypeLabel || draftTypeLabel(league.draftType || 'snake'),
    draftDate: league.draftDate || '',
    draftLobbyOpen: Boolean(league.draftLobbyOpen),
    draftLobbyStartedAt: league.draftLobbyStartedAt || '',
    notes: league.notes || '',
    invitedEmails,
    members,
    scoringSettings: normalizeScoringSettings(league.scoring || 'ppr', league.scoringSettings),
    rosterRules: { ...defaultRosterRules, ...(league.rosterRules || {}) },
    waiverRules: { ...defaultWaiverRules, ...(league.waiverRules || {}) },
    tradeRules: { ...defaultTradeRules, ...(league.tradeRules || {}) }
  };
}

const DRAFT_LOBBY_AUTO_OPEN_MINUTES = 30;

function draftTimezone() {
  return Intl.DateTimeFormat().resolvedOptions().timeZone || 'Local time';
}

function draftHourOptions() {
  return Array.from({ length: 24 }, (_, hour) => {
    const date = new Date(2000, 0, 1, hour, 0, 0, 0);
    return {
      value: String(hour).padStart(2, '0'),
      label: date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
    };
  });
}

function populateDraftTimeSelect(select, selectedHour = '19') {
  if (!select) return;
  const safeHour = String(selectedHour || '19').padStart(2, '0');
  select.innerHTML = draftHourOptions()
    .map((option) => `<option value="${option.value}">${option.label}</option>`)
    .join('');
  select.value = /^\d{2}$/.test(safeHour) ? safeHour : '19';
}

function draftDatePart(value = '') {
  if (!value) return '';
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) return String(value).slice(0, 10);
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

function draftHourPart(value = '', fallback = '19') {
  if (!value) return fallback;
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) {
    const match = String(value).match(/T(\d{2}):/);
    return match ? match[1] : fallback;
  }
  return String(date.getHours()).padStart(2, '0');
}

function combineDraftDateAndHour(dateValue = '', hourValue = '19') {
  if (!dateValue) return '';
  const [year, month, day] = String(dateValue).split('-').map(Number);
  const hour = Number(hourValue);
  if (!year || !month || !day || !Number.isInteger(hour) || hour < 0 || hour > 23) return '';
  return new Date(year, month - 1, day, hour, 0, 0, 0).toISOString();
}

function draftDateTime(value = '') {
  const timestamp = new Date(value).getTime();
  return Number.isFinite(timestamp) ? timestamp : null;
}

function isTopOfHourDraftDate(value = '') {
  if (!value) return true;
  const date = new Date(value);
  return Number.isFinite(date.getTime())
    && date.getMinutes() === 0
    && date.getSeconds() === 0
    && date.getMilliseconds() === 0;
}

function draftLobbyAutoOpen(league = getLeagueState(), now = Date.now()) {
  const draftAt = draftDateTime(league?.draftDate);
  if (!draftAt) return false;
  const opensAt = draftAt - DRAFT_LOBBY_AUTO_OPEN_MINUTES * 60 * 1000;
  return Number(now) >= opensAt;
}

function effectiveDraftLobbyOpen(league = getLeagueState(), now = Date.now()) {
  return Boolean(league?.draftLobbyOpen || draftLobbyAutoOpen(league, now));
}

window.DRAFT_LOBBY_AUTO_OPEN_MINUTES = DRAFT_LOBBY_AUTO_OPEN_MINUTES;
window.draftTimezone = draftTimezone;
window.draftHourOptions = draftHourOptions;
window.populateDraftTimeSelect = populateDraftTimeSelect;
window.draftDatePart = draftDatePart;
window.draftHourPart = draftHourPart;
window.combineDraftDateAndHour = combineDraftDateAndHour;
window.isTopOfHourDraftDate = isTopOfHourDraftDate;
window.draftLobbyAutoOpen = draftLobbyAutoOpen;
window.effectiveDraftLobbyOpen = effectiveDraftLobbyOpen;

function normalizeMembers(members = [], invitedEmails = []) {
  const auth = getAuthState();
  const byEmail = new Map();
  if (auth?.email && (!Array.isArray(members) || members.length === 0)) {
    byEmail.set(auth.email, {
      email: auth.email,
      role: 'commissioner',
      status: 'Active',
      teamName: ''
    });
  }
  invitedEmails.forEach((email) => {
    if (!byEmail.has(email)) {
      byEmail.set(email, { email, role: 'member', status: 'Invited', teamName: '' });
    }
  });
  if (Array.isArray(members)) {
    members.forEach((member) => {
      if (!member?.email) return;
      byEmail.set(member.email, {
        email: member.email,
        role: member.role === 'commissioner' ? 'commissioner' : 'member',
        status: normalizeMemberStatus(member.status),
        invitedByEmail: member.invitedByEmail || '',
        teamName: member.teamName || member.team_name || ''
      });
    });
  }
  return Array.from(byEmail.values()).filter((member) => member.status !== 'Removed');
}

function normalizeMemberStatus(status = 'Invited') {
  const lowered = String(status).toLowerCase();
  if (lowered === 'active') return 'Active';
  if (lowered === 'pending') return 'Pending';
  if (lowered === 'removed') return 'Removed';
  return 'Invited';
}

function currentMemberRole(league = getLeagueState()) {
  const auth = getAuthState();
  if (!auth?.email || !league) return null;
  return league.members?.find((member) => member.email === auth.email)?.role || null;
}

function isCurrentCommissioner(league = getLeagueState()) {
  return currentMemberRole(league) === 'commissioner';
}

function managerDisplayName(email, league = getLeagueState()) {
  if (!email) return 'Bye';
  const member = (league?.members || []).find((item) => item.email === email);
  return member?.teamName || email;
}

function isActiveTradeTarget(targetManager, league = getLeagueState()) {
  const auth = getAuthState();
  const currentMember = (league?.members || []).find((member) => member.email === auth?.email);
  return Boolean(targetManager
    && targetManager !== auth?.email
    && currentMember?.status === 'Active'
    && (league?.members || []).some((member) => member.email === targetManager && member.status === 'Active'));
}

function escapeHtml(value = '') {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function scoringLabel(scoring) {
  if (scoring === 'half_ppr') return 'Half-PPR';
  if (scoring === 'standard') return 'Standard';
  return 'PPR';
}

function draftTypeLabel(draftType) {
  return draftType === 'auction' ? 'Auction' : 'Snake';
}

function parseEmailList(value = '') {
  return value
    .split(/[,\n]/)
    .map((item) => item.trim())
    .filter(Boolean)
    .filter((item, index, arr) => arr.indexOf(item) === index);
}

function normalizeScoringSettings(scoring = 'ppr', settings = {}) {
  return {
    ...(scoringPresets[scoring] || scoringPresets.ppr),
    ...(settings || {})
  };
}

function scoringSummary(settings = scoringPresets.ppr) {
  return [
    `${settings.reception} REC`,
    `${settings.passingTd} Pass TD`,
    `${settings.rushingTd} Rush TD`,
    `${settings.receivingTd} Rec TD`
  ].join(' / ');
}

function rosterProjection() {
  return getRoster().reduce((total, player) => total + Number(player.projection || 0), 0);
}

function positionCounts(players = getRoster()) {
  return players.reduce((counts, player) => {
    const key = String(player.position || 'FLEX').toLowerCase();
    counts[key] = (counts[key] || 0) + 1;
    return counts;
  }, {});
}

function rosterLimit(league = getLeagueState()) {
  const rules = { ...defaultRosterRules, ...(league?.rosterRules || {}) };
  return ['qb', 'rb', 'wr', 'te', 'flex', 'bench'].reduce((total, slot) => total + Number(rules[slot] || 0), 0);
}

function rosterHasRoom(league = getLeagueState(), roster = getRoster()) {
  return roster.length < rosterLimit(league);
}

function waiverRules(league = getLeagueState()) {
  return { ...defaultWaiverRules, ...(league?.waiverRules || {}) };
}

function freeAgencyLocked(league = getLeagueState()) {
  const rules = waiverRules(league);
  return rules.mode === 'waivers' || Boolean(rules.freeAgencyLocked);
}

function waiverDeadlinePassed(league = getLeagueState()) {
  const deadline = waiverRules(league).claimDeadline;
  return !deadline || deadline <= new Date().toISOString().slice(0, 16);
}

function tradeRules(league = getLeagueState()) {
  return { ...defaultTradeRules, ...(league?.tradeRules || {}) };
}

function tradeExpiresAt(league = getLeagueState()) {
  const hours = Math.max(1, Number(tradeRules(league).expirationHours || 48));
  return new Date(Date.now() + hours * 60 * 60 * 1000).toISOString();
}

function isOpenTradeStatus(status = '') {
  return ['Pending', 'Accepted'].includes(status);
}

function playerLockedInTrade(playerId, trades = getTradeOffers()) {
  return trades.some((trade) => isOpenTradeStatus(trade.status) && (
    trade.offerPlayer?.id === playerId || trade.requestPlayer?.id === playerId
  ));
}

function flexEligible(position = '') {
  return ['RB', 'WR', 'TE'].includes(String(position).toUpperCase());
}

function assignRosterSlot(player, roster = getRoster(), league = getLeagueState()) {
  const rules = { ...defaultRosterRules, ...(league?.rosterRules || {}) };
  const counts = roster.reduce((acc, item) => {
    const slot = String(item.rosterSlot || item.position || 'bench').toLowerCase();
    acc[slot] = (acc[slot] || 0) + 1;
    return acc;
  }, {});
  const natural = String(player.position || 'flex').toLowerCase();
  if (rules[natural] && (counts[natural] || 0) < rules[natural]) return natural;
  if (flexEligible(player.position) && (counts.flex || 0) < rules.flex) return 'flex';
  if ((counts.bench || 0) < rules.bench) return 'bench';
  return null;
}

function legalSlotsForPlayer(player, league = getLeagueState()) {
  const rules = { ...defaultRosterRules, ...(league?.rosterRules || {}) };
  const position = String(player.position || '').toLowerCase();
  const slots = [];
  if (['qb', 'rb', 'wr', 'te'].includes(position) && Number(rules[position] || 0) > 0) {
    slots.push(position);
  }
  if (flexEligible(player.position) && Number(rules.flex || 0) > 0) {
    slots.push('flex');
  }
  if (Number(rules.bench || 0) > 0) {
    slots.push('bench');
  }
  return slots;
}

function lineupErrors(roster = getRoster(), league = getLeagueState()) {
  const rules = { ...defaultRosterRules, ...(league?.rosterRules || {}) };
  const counts = roster.reduce((acc, player) => {
    const slot = String(player.rosterSlot || 'bench').toLowerCase();
    acc[slot] = (acc[slot] || 0) + 1;
    return acc;
  }, {});
  const errors = [];
  ['qb', 'rb', 'wr', 'te', 'flex'].forEach((slot) => {
    const required = Number(rules[slot] || 0);
    const filled = Number(counts[slot] || 0);
    if (filled < required) {
      errors.push({ slot, message: `Missing ${required - filled} ${slot.toUpperCase()} starter${required - filled === 1 ? '' : 's'}` });
    }
    if (filled > required) {
      errors.push({ slot, message: `Too many ${slot.toUpperCase()} starters` });
    }
  });
  return errors;
}

function lineupValid(roster = getRoster(), league = getLeagueState()) {
  return lineupErrors(roster, league).length === 0;
}

function lineupLocked(matchups = getMatchups()) {
  return matchups.some((matchup) => String(matchup.status || '').toLowerCase() === 'final');
}

function canMoveToSlot(playerId, slot, roster = getRoster(), league = getLeagueState()) {
  const player = roster.find((item) => item.id === playerId);
  if (!player) return false;
  const requestedSlot = String(slot || '').toLowerCase();
  if (!legalSlotsForPlayer(player, league).includes(requestedSlot)) return false;
  const rules = { ...defaultRosterRules, ...(league?.rosterRules || {}) };
  const occupied = roster.filter((item) => item.id !== playerId && String(item.rosterSlot || 'bench').toLowerCase() === requestedSlot).length;
  return occupied < Number(rules[requestedSlot] || 0);
}

function setRosterSlot(playerId, slot) {
  const roster = getRoster();
  const requestedSlot = String(slot || '').toLowerCase();
  if (!canMoveToSlot(playerId, requestedSlot, roster)) return false;
  setRoster(roster.map((player) => player.id === playerId ? { ...player, rosterSlot: requestedSlot } : player));
  return true;
}

function filterSamplePlayers(term = '') {
  const needle = term.trim().toLowerCase();
  if (!needle) return samplePlayers;
  return samplePlayers.filter((player) => {
    const haystack = `${player.name} ${player.team} ${player.position} ${player.conference}`.toLowerCase();
    return haystack.includes(needle);
  });
}

function addPlayerToQueue(player) {
  const normalized = normalizePlayer(player);
  const queue = getQueue();
  if (!queue.some((item) => item.id === normalized.id)) {
    queue.push(normalized);
    setQueue(queue);
  }
  return queue;
}

function draftPlayer(player) {
  const normalized = normalizePlayer(player);
  const roster = getRoster();
  const meta = getDraftMeta();
  if (meta.status !== 'open') return roster;
  if (!roster.some((item) => item.id === normalized.id)) {
    const slot = assignRosterSlot(normalized, roster);
    if (!slot) return roster;
    roster.push({ ...normalized, rosterSlot: slot });
    setRoster(roster);
  }
  setQueue(getQueue().filter((item) => item.id !== normalized.id));
  const picks = getDraftPicks();
  const pickNumber = Number(meta.currentPick || picks.length + 1);
  saveDraftPicks([
    ...picks,
    {
      id: `pick-${Date.now()}`,
      managerEmail: currentDraftManager(meta) || getAuthState()?.email || '',
      pickNumber,
      player: normalized,
      createdAt: new Date().toISOString()
    }
  ]);
  const nextPick = pickNumber + 1;
  const complete = rosterHasRoom(getLeagueState(), getRoster()) === false;
  saveDraftMeta({
    ...meta,
    status: complete ? 'complete' : meta.status,
    currentPick: nextPick,
    currentManager: currentDraftManager({ ...meta, currentPick: nextPick }),
    pickDeadline: complete ? '' : new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString()
  });
  return roster;
}

function undoLastDraftPick() {
  const picks = getDraftPicks();
  const lastPick = picks[picks.length - 1];
  if (!lastPick) return false;
  const player = normalizePlayer(lastPick.player || {});
  saveDraftPicks(picks.slice(0, -1));
  setRoster(getRoster().filter((item) => item.id !== player.id));
  addPlayerToQueue(player);
  const meta = getDraftMeta();
  const pickNumber = Number(lastPick.pickNumber || picks.length);
  saveDraftMeta({
    ...meta,
    status: 'open',
    currentPick: pickNumber,
    currentManager: currentDraftManager({ ...meta, currentPick: pickNumber, currentManager: '' }),
    pickDeadline: new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString()
  });
  return true;
}

async function draftPlayerApi(player) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    return draftPlayer(player);
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/picks`, {
    method: 'POST',
    body: JSON.stringify({ player: normalizePlayer(player) })
  });
  applyDraftState(state);
  try {
    await syncActiveLeagueCollectionsFromApi();
  } catch {
    // The draft state is already applied; keep the pick from being replayed locally.
  }
  return getRoster();
}

async function autoPickFromQueueApi() {
  const player = getQueue()[0] || getAvailablePlayers()[0];
  if (!player) return false;
  await draftPlayerApi(player);
  return true;
}

async function releaseDraftedPlayerApi(playerId) {
  const player = getRoster().find((item) => item.id === playerId);
  if (!player) return false;
  await dropPlayerApi(playerId);
  const nextQueue = [...getQueue().filter((item) => item.id !== player.id), normalizePlayer(player)];
  await saveDraftQueueApi(nextQueue);
  setQueue(nextQueue);
  return true;
}

function removeFromQueue(playerId) {
  setQueue(getQueue().filter((item) => item.id !== playerId));
}

function clearDraftState() {
  localStorage.removeItem(CFF_QUEUE_KEY);
  localStorage.removeItem(CFF_ROSTER_KEY);
  const league = getLeagueState();
  if (league?.id) {
    const picks = readJson(CFF_DRAFT_PICKS_KEY, {});
    const meta = readJson(CFF_DRAFT_META_KEY, {});
    delete picks[league.id];
    delete meta[league.id];
    writeJson(CFF_DRAFT_PICKS_KEY, picks);
    writeJson(CFF_DRAFT_META_KEY, meta);
  }
}

function getAvailablePlayers() {
  const rosterIds = new Set(getRoster().map((player) => player.id));
  const queueIds = new Set(getQueue().map((player) => player.id));
  return samplePlayers.filter((player) => !rosterIds.has(player.id) && !queueIds.has(player.id));
}

function getDraftPicks() {
  return getLeagueScopedItems(CFF_DRAFT_PICKS_KEY);
}

function saveDraftPicks(picks) {
  setLeagueScopedItems(CFF_DRAFT_PICKS_KEY, picks);
}

function getDraftMeta() {
  const league = getLeagueState();
  const store = readJson(CFF_DRAFT_META_KEY, {});
  return league?.id
    ? store[league.id] || {
      status: 'not_started',
      currentPick: 1,
      draftOrder: [],
      draftType: league.draftType || 'snake',
      currentManager: getAuthState()?.email || '',
      pickClockSeconds: 90,
      pickDeadline: new Date(Date.now() + 90000).toISOString()
    }
    : { status: 'not_started', currentPick: 1, draftOrder: [], draftType: 'snake', pickClockSeconds: 90, pickDeadline: new Date(Date.now() + 90000).toISOString() };
}

function saveDraftMeta(meta = {}) {
  const league = getLeagueState();
  if (!league?.id) return;
  const store = readJson(CFF_DRAFT_META_KEY, {});
  store[league.id] = {
    status: meta.status || 'not_started',
    currentPick: Number(meta.currentPick || 1),
    draftOrder: Array.isArray(meta.draftOrder) ? meta.draftOrder : [],
    draftType: meta.draftType || league.draftType || 'snake',
    currentManager: meta.currentManager || '',
    pickClockSeconds: Number(meta.pickClockSeconds || 90),
    pickDeadline: meta.pickDeadline || '',
    startedAt: meta.startedAt || '',
    completedAt: meta.completedAt || '',
    version: Number(meta.version || meta.revision || 0),
    revision: Number(meta.revision || meta.version || 0),
    totalPicks: Number(meta.totalPicks || 0),
    picksRemaining: Number(meta.picksRemaining || 0),
    readiness: Array.isArray(meta.readiness) ? meta.readiness : [],
    activity: Array.isArray(meta.activity) ? meta.activity : [],
    lobbyOpen: typeof meta.lobbyOpen === 'boolean' ? meta.lobbyOpen : Boolean(league.draftLobbyOpen)
  };
  writeJson(CFF_DRAFT_META_KEY, store);
}

function saveDraftOrder(draftOrder = []) {
  const meta = getDraftMeta();
  const currentPick = Number(meta.currentPick || 1);
  saveDraftMeta({
    ...meta,
    status: meta.status || 'not_started',
    currentPick,
    draftOrder,
    currentManager: draftManagerForPick(draftOrder, currentPick, meta.draftType || getLeagueState()?.draftType || 'snake'),
    pickDeadline: meta.status === 'open'
      ? meta.pickDeadline || new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString()
      : ''
  });
}

function startDraft() {
  const league = getLeagueState();
  const activeManagers = (league?.members || []).filter((member) => String(member.status || '').toLowerCase() === 'active');
  if (!isCurrentCommissioner(league) || !league?.draftLobbyOpen || activeManagers.length < 2) return null;
  const meta = getDraftMeta();
  if (meta.status === 'open') return meta;
  const draftOrder = Array.isArray(meta.draftOrder) && meta.draftOrder.length
    ? meta.draftOrder
    : activeManagers.map((member) => member.email).filter(Boolean);
  const startedAt = new Date().toISOString();
  const next = {
    ...meta,
    status: 'open',
    currentPick: 1,
    draftOrder,
    currentManager: draftManagerForPick(draftOrder, 1, meta.draftType || league.draftType || 'snake'),
    startedAt,
    pickDeadline: new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString(),
    lobbyOpen: true
  };
  saveDraftMeta(next);
  return next;
}

function draftManagerForPick(order = [], currentPick = 1, draftType = 'snake') {
  if (!Array.isArray(order) || !order.length) return '';
  const pickIndex = Math.max(1, Number(currentPick || 1)) - 1;
  const round = Math.floor(pickIndex / order.length);
  let offset = pickIndex % order.length;
  if (String(draftType || 'snake').toLowerCase() === 'snake' && round % 2 === 1) {
    offset = order.length - 1 - offset;
  }
  return order[offset] || '';
}

function currentDraftManager(meta = getDraftMeta()) {
  const order = Array.isArray(meta.draftOrder) ? meta.draftOrder : [];
  if (!order.length) return meta.currentManager || getAuthState()?.email || '';
  return draftManagerForPick(order, meta.currentPick, meta.draftType || getLeagueState()?.draftType || 'snake') || meta.currentManager || '';
}

function isMyDraftTurn(meta = getDraftMeta()) {
  const manager = currentDraftManager(meta);
  return !manager || manager === getAuthState()?.email;
}

function draftClockRemaining(meta = getDraftMeta()) {
  if (meta.status !== 'open') return 0;
  if (!meta.pickDeadline) return Number(meta.pickClockSeconds || 90);
  return Math.max(0, Math.ceil((new Date(meta.pickDeadline).getTime() - Date.now()) / 1000));
}

function getLeagueScopedStore(key) {
  return readJson(key, {});
}

function getLeagueScopedItems(key) {
  const league = getLeagueState();
  if (!league?.id) return [];
  const store = getLeagueScopedStore(key);
  return store[league.id] || [];
}

function setLeagueScopedItemsForLeague(key, leagueId, items) {
  if (!leagueId) return;
  const store = getLeagueScopedStore(key);
  store[leagueId] = items;
  writeJson(key, store);
}

function setLeagueScopedItems(key, items) {
  const league = getLeagueState();
  if (!league?.id) return;
  const store = getLeagueScopedStore(key);
  store[league.id] = items;
  writeJson(key, store);
}

function getWaiverClaims() {
  return getLeagueScopedItems(CFF_WAIVERS_KEY);
}

function saveWaiverClaims(claims) {
  setLeagueScopedItems(CFF_WAIVERS_KEY, claims);
}

function getTradeOffers() {
  return getLeagueScopedItems(CFF_TRADES_KEY);
}

function saveTradeOffers(offers) {
  setLeagueScopedItems(CFF_TRADES_KEY, offers);
}

function getTransactions() {
  return getLeagueScopedItems(CFF_TRANSACTIONS_KEY);
}

function getMatchups() {
  return getLeagueScopedItems(CFF_MATCHUPS_KEY);
}

function saveMatchups(matchups) {
  setLeagueScopedItems(CFF_MATCHUPS_KEY, matchups);
}

function lineupProjection(roster = getRoster()) {
  return roster.reduce((total, player) => {
    const slot = String(player.rosterSlot || player.position || 'bench').toLowerCase();
    return slot === 'bench' ? total : total + Number(player.projection || 0);
  }, 0);
}

function activeLeagueManagers(league = getLeagueState()) {
  const auth = getAuthState();
  const members = (league?.members || []).filter((member) => member.status !== 'Removed');
  return members.length ? members : auth?.email ? [{ email: auth.email, status: 'Active' }] : [];
}

function generateLocalMatchups(league = getLeagueState(), week = 1) {
  const auth = getAuthState();
  const managerEmails = activeLeagueManagers(league).map((manager) => manager.email).filter(Boolean);
  if (!managerEmails.length) {
    saveMatchups([]);
    return [];
  }
  if (managerEmails.length % 2 === 1) managerEmails.push('');
  const roundCount = managerEmails.length > 1 ? managerEmails.length - 1 : 1;
  const round = (Math.max(1, Number(week || 1)) - 1) % roundCount;
  const rotated = [...managerEmails];
  if (rotated.length > 2) {
    const movable = rotated.slice(1);
    rotated.splice(1, movable.length, ...movable.slice(round), ...movable.slice(0, round));
  }
  const matchups = [];
  for (let index = 0; index < rotated.length / 2; index += 1) {
    let home = rotated[index] || '';
    let away = rotated[rotated.length - 1 - index] || '';
    if (!home && !away) continue;
    if (!home) [home, away] = [away, home];
    if (Number(week) % 2 === 0 && away) [home, away] = [away, home];
    matchups.push({
      id: `${league?.id || 'local'}-week-${week}-${index + 1}`,
      week: Number(week || 1),
      homeManager: home,
      awayManager: away,
      homeScore: home === auth?.email ? lineupProjection() : 0,
      awayScore: away === auth?.email ? lineupProjection() : 0,
      status: 'scheduled'
    });
  }
  return matchups;
}

function generateLocalSeasonSchedule(league = getLeagueState(), weeks = 12) {
  const schedule = [];
  for (let week = 1; week <= weeks; week += 1) {
    schedule.push(...generateLocalMatchups(league, week));
  }
  saveMatchups(schedule);
  return schedule;
}

function standingsFromMatchups(league = getLeagueState(), matchups = getMatchups()) {
  const rows = (league?.members || []).filter((member) => member.status !== 'Removed').map((member) => ({
    email: member.email,
    teamName: member.teamName || '',
    role: member.role,
    status: member.status || 'Invited',
    wins: 0,
    losses: 0,
    ties: 0,
    pointsFor: 0,
    pointsAgainst: 0,
    gamesPlayed: 0,
    winPct: 0
  }));
  const byEmail = new Map(rows.map((row) => [row.email, row]));
  matchups.forEach((matchup) => {
    const home = byEmail.get(matchup.homeManager);
    const away = byEmail.get(matchup.awayManager);
    const homeScore = Number(matchup.homeScore || 0);
    const awayScore = Number(matchup.awayScore || 0);
    if (home) home.pointsFor += homeScore;
    if (away) away.pointsFor += awayScore;
    if (home && away) {
      home.pointsAgainst += awayScore;
      away.pointsAgainst += homeScore;
    }
    if (!away || matchup.status !== 'final') return;
    if (home) home.gamesPlayed += 1;
    away.gamesPlayed += 1;
    if (homeScore > awayScore) {
      home.wins += 1;
      away.losses += 1;
    } else if (awayScore > homeScore) {
      away.wins += 1;
      home.losses += 1;
    } else {
      home.ties += 1;
      away.ties += 1;
    }
  });
  rows.forEach((row) => {
    row.winPct = row.gamesPlayed ? (row.wins + row.ties * 0.5) / row.gamesPlayed : 0;
  });
  return rows.sort((a, b) => b.winPct - a.winPct || b.wins - a.wins || a.losses - b.losses || b.pointsFor - a.pointsFor);
}

function addTransaction(type, summary) {
  const transactions = getTransactions();
  transactions.unshift({
    id: `txn-${Date.now().toString(36)}`,
    type,
    summary,
    createdAt: new Date().toISOString()
  });
  setLeagueScopedItems(CFF_TRANSACTIONS_KEY, transactions.slice(0, 50));
}

async function syncLeaguesFromApi() {
  if (!getAuthState()?.token) return getLeaguesForCurrentAccount();
  let leagues;
  try {
    leagues = await apiRequest('/leagues');
  } catch (error) {
    markApiCacheStale('leagues');
    throw error;
  }
  replaceLeaguesForCurrentAccount(leagues || []);
  writeApiCacheMeta('leagues');
  return getLeaguesForCurrentAccount();
}

async function syncActiveLeagueCollectionsFromApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || !league?.id) return;
  let roster;
  let waivers;
  let waiverPriority;
  let trades;
  let transactions;
  let members;
  let matchups;
  try {
    [roster, waivers, waiverPriority, trades, transactions, members, matchups] = await Promise.all([
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/roster`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/waiver-priority`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/transactions`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/members`),
      apiRequest(`/leagues/${encodeURIComponent(league.id)}/matchups`)
    ]);
  } catch (error) {
    markApiCacheStale('league');
    throw error;
  }
  setRoster((roster || []).map(normalizePlayer));
  setLeagueScopedItemsForLeague(CFF_WAIVERS_KEY, league.id, waivers || []);
  setLeagueScopedItemsForLeague(CFF_WAIVER_PRIORITIES_KEY, league.id, waiverPriority || []);
  setLeagueScopedItemsForLeague(CFF_TRADES_KEY, league.id, trades || []);
  setLeagueScopedItemsForLeague(CFF_TRANSACTIONS_KEY, league.id, transactions || []);
  setLeagueScopedItemsForLeague(CFF_MATCHUPS_KEY, league.id, matchups || []);
  saveLeagueForAccount({ ...league, members: members || league.members || [] });
  writeApiCacheMeta('league', league.id);
}

function applyDraftState(state = {}) {
  if (Array.isArray(state.queue)) {
    setQueue(state.queue.map(normalizePlayer));
  }
  if (Array.isArray(state.picks)) {
    saveDraftPicks(state.picks);
  }
  const league = getLeagueState();
  if (league && typeof state.lobbyOpen === 'boolean') {
    saveLeagueForAccount({
      ...league,
      draftLobbyOpen: state.lobbyOpen,
      draftLobbyStartedAt: league.draftLobbyStartedAt || ''
    });
  }
  saveDraftMeta(state);
}

async function syncDraftFromApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || !league?.id) return null;
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft`);
  applyDraftState(state);
  return state;
}

async function saveDraftQueueApi(queue = getQueue()) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    setQueue(queue);
    return null;
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/queue`, {
    method: 'PUT',
    body: JSON.stringify({ queue })
  });
  applyDraftState(state);
  return state;
}

async function saveDraftOrderApi(draftOrder = []) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    saveDraftOrder(draftOrder);
    return null;
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/order`, {
    method: 'PUT',
    body: JSON.stringify({ draftOrder })
  });
  applyDraftState(state);
  return state;
}

async function startDraftApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    return startDraft();
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/start`, {
    method: 'POST'
  });
  applyDraftState(state);
  return state;
}

async function resetDraftApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    clearDraftState();
    return null;
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/reset`, {
    method: 'POST'
  });
  applyDraftState(state);
  try {
    await syncActiveLeagueCollectionsFromApi();
  } catch {
    // The draft reset response is authoritative enough to keep the UI moving.
  }
  return state;
}

async function undoLastDraftPickApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    undoLastDraftPick();
    return null;
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/undo`, {
    method: 'POST'
  });
  applyDraftState(state);
  try {
    await syncActiveLeagueCollectionsFromApi();
  } catch {
    // The undo response already contains the updated draft board.
  }
  return state;
}

async function saveLeagueToApi(league) {
  const normalized = normalizeLeague(league);
  if (!getAuthState()?.token || isLocalDemoSession()) {
    return saveLeagueForAccount(normalized);
  }
  if (normalized.id.startsWith('local-')) {
    throw new Error('Local demo leagues cannot be saved to a production session');
  }
  const saved = normalizeLeague(await apiRequest(`/leagues/${encodeURIComponent(normalized.id)}`, {
    method: 'PUT',
    body: JSON.stringify(normalized)
  }));
  return saveLeagueForAccount(saved);
}

async function removeLeagueFromApi(leagueId) {
  if (getAuthState()?.token && !isLocalDemoSession() && leagueId && !leagueId.startsWith('local-')) {
    await apiRequest(`/leagues/${encodeURIComponent(leagueId)}`, { method: 'DELETE' });
    removeLeagueForCurrentAccount(leagueId);
    return;
  }
  if (getAuthState()?.token && !isLocalDemoSession() && String(leagueId || '').startsWith('local-')) {
    throw new Error('Local demo leagues cannot be removed from a production session');
  }
  removeLeagueForCurrentAccount(leagueId);
}

async function inviteMemberApi(email, role = 'member') {
  const league = getLeagueState();
  if (!email || !league?.id) return false;
  if (!getAuthState()?.token || isLocalDemoSession()) {
    const members = normalizeMembers([...(league.members || []), { email, role, status: 'Invited' }], [...(league.invitedEmails || []), email]);
    saveLeagueForAccount({ ...league, invitedEmails: [...new Set([...(league.invitedEmails || []), email])], members });
    return true;
  }
  const members = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/members`, {
    method: 'POST',
    body: JSON.stringify({ email, role })
  });
  saveLeagueForAccount({ ...league, invitedEmails: [...new Set([...(league.invitedEmails || []), email])], members });
  return true;
}

async function updateMemberApi(email, changes = {}) {
  const league = getLeagueState();
  if (!email || !league?.id) return false;
  if (!getAuthState()?.token || isLocalDemoSession()) {
    const members = normalizeMembers((league.members || []).map((member) => (
      member.email === email ? { ...member, ...changes } : member
    )), league.invitedEmails || []);
    saveLeagueForAccount({ ...league, members, invitedEmails: members.filter((member) => member.status !== 'Removed' && member.role !== 'commissioner').map((member) => member.email) });
    return true;
  }
  const member = league.members?.find((item) => item.email === email) || {};
  const members = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/members/${encodeURIComponent(email)}`, {
    method: 'PUT',
    body: JSON.stringify({
      role: changes.role || member.role || 'member',
      status: changes.status || member.status || 'Invited',
      teamName: changes.teamName ?? member.teamName ?? ''
    })
  });
  saveLeagueForAccount({
    ...league,
    members,
    invitedEmails: (members || []).filter((item) => item.status !== 'Removed' && item.role !== 'commissioner').map((item) => item.email)
  });
  return true;
}

async function joinLeagueApi(leagueId) {
  if (!getAuthState()?.token || !leagueId) return null;
  const payload = await apiRequest(`/leagues/${encodeURIComponent(leagueId)}/join`, {
    method: 'POST'
  });
  if (payload?.joinStatus === 'pending_approval') {
    return payload;
  }
  const league = normalizeLeague(payload);
  saveLeagueForAccount(league);
  await syncActiveLeagueCollectionsFromApi();
  return league;
}

async function addFreeAgentApi(player) {
  const league = getLeagueState();
  if (freeAgencyLocked(league)) return false;
  if (!getAuthState()?.token || isLocalDemoSession()) return addFreeAgent(player);
  if (!league?.id) throw new Error('No server league selected');
  const roster = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/roster`, {
    method: 'POST',
    body: JSON.stringify({ player: normalizePlayer(player) })
  });
  setRoster((roster || []).map(normalizePlayer));
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function dropPlayerApi(playerId) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) return dropPlayer(playerId);
  if (!league?.id) throw new Error('No server league selected');
  const roster = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/roster/drop`, {
    method: 'POST',
    body: JSON.stringify({ playerId })
  });
  setRoster((roster || []).map(normalizePlayer));
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function submitWaiverClaimApi(addPlayer, dropPlayerId = '') {
  const league = getLeagueState();
  if (lineupLocked()) return false;
  if (!getAuthState()?.token || isLocalDemoSession()) {
    submitWaiverClaim(addPlayer, dropPlayerId);
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers`, {
    method: 'POST',
    body: JSON.stringify({ addPlayer: normalizePlayer(addPlayer), dropPlayerId })
  });
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function processWaiverClaimApi(claimId) {
  const league = getLeagueState();
  if (lineupLocked()) return false;
  if (!getAuthState()?.token || isLocalDemoSession()) {
    processWaiverClaim(claimId);
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/${encodeURIComponent(claimId)}/process`, {
    method: 'POST'
  });
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function cancelWaiverClaimApi(claimId) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    cancelWaiverClaim(claimId);
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  const claims = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/${encodeURIComponent(claimId)}/status`, {
    method: 'POST',
    body: JSON.stringify({ status: 'Cancelled' })
  });
  if (Array.isArray(claims)) {
    saveWaiverClaims(claims);
  }
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function reorderWaiverClaimsApi(claimIds = []) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    reorderWaiverClaims(claimIds);
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  const claims = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/reorder`, {
    method: 'POST',
    body: JSON.stringify({ claimIds })
  });
  if (Array.isArray(claims)) {
    saveWaiverClaims(claims);
  }
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function processWaiversApi() {
  const league = getLeagueState();
  if (lineupLocked()) {
    return { processed: [], cancelled: [], claims: getWaiverClaims() };
  }
  if (!getAuthState()?.token || isLocalDemoSession()) {
    processAllWaiverClaims();
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  const result = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waivers/process`, {
    method: 'POST'
  });
  if (Array.isArray(result?.claims)) {
    saveWaiverClaims(result.claims);
  }
  await syncActiveLeagueCollectionsFromApi();
  return result;
}

function getWaiverPriority() {
  const stored = getLeagueScopedItems(CFF_WAIVER_PRIORITIES_KEY);
  if (stored.length) return stored;
  return (getLeagueState()?.members || [])
    .filter((member) => member.status !== 'Removed')
    .map((member, index) => ({
      managerEmail: member.email,
      role: member.role || 'member',
      status: member.status || 'Active',
      priority: index + 1
    }));
}

function saveWaiverPriority(priority = []) {
  setLeagueScopedItems(CFF_WAIVER_PRIORITIES_KEY, priority);
}

async function resetWaiverPriorityApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    const priority = (league?.members || [])
      .filter((member) => member.status !== 'Removed')
      .map((member, index) => ({
        managerEmail: member.email,
        role: member.role || 'member',
        status: member.status || 'Active',
        priority: index + 1
      }));
    saveWaiverPriority(priority);
    addTransaction('Waiver Priority', 'Reset waiver priority order');
    return priority;
  }
  if (!league?.id) throw new Error('No server league selected');
  const priority = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/waiver-priority/reset`, {
    method: 'POST'
  });
  saveWaiverPriority(priority || []);
  await syncActiveLeagueCollectionsFromApi();
  return priority;
}

async function submitTradeOfferApi(offerPlayerId, requestPlayerName, targetManager, requestPlayer = null, note = '') {
  const league = getLeagueState();
  const player = getRoster().find((item) => item.id === offerPlayerId);
  if (!getAuthState()?.token || isLocalDemoSession()) return submitTradeOffer(offerPlayerId, requestPlayerName, targetManager, requestPlayer, note);
  if (!league?.id) throw new Error('No server league selected');
  if (lineupLocked() || !player || !requestPlayer?.id || !isActiveTradeTarget(targetManager, league)) return false;
  await apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades`, {
    method: 'POST',
    body: JSON.stringify({ offerPlayer: player, requestPlayer: requestPlayer ? normalizePlayer(requestPlayer) : null, requestPlayerName, targetManager, note })
  });
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function updateTradeStatusApi(tradeId, status) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    updateTradeStatus(tradeId, status);
    return true;
  }
  if (!league?.id) throw new Error('No server league selected');
  await apiRequest(`/leagues/${encodeURIComponent(league.id)}/trades/${encodeURIComponent(tradeId)}/status`, {
    method: 'POST',
    body: JSON.stringify({ status })
  });
  await syncActiveLeagueCollectionsFromApi();
  return true;
}

async function getManagerRosterApi(managerEmail) {
  const league = getLeagueState();
  if (!getAuthState()?.token || !league?.id || !managerEmail) {
    return samplePlayers.filter((player) => !getRoster().some((item) => item.id === player.id)).map(normalizePlayer);
  }
  const roster = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/rosters/${encodeURIComponent(managerEmail)}`);
  return (roster || []).map(normalizePlayer);
}

function addFreeAgent(player) {
  if (freeAgencyLocked() || lineupLocked()) return false;
  const normalized = normalizePlayer(player);
  const roster = getRoster();
  if (roster.some((item) => item.id === normalized.id)) {
    return false;
  }
  if (!rosterHasRoom()) {
    return false;
  }
  const slot = assignRosterSlot(normalized, roster);
  if (!slot) return false;
  setRoster([...roster, { ...normalized, rosterSlot: slot }]);
  addTransaction('Free Agent', `Added ${normalized.name}`);
  return true;
}

function dropPlayer(playerId) {
  if (lineupLocked() || playerLockedInTrade(playerId)) return false;
  const roster = getRoster();
  const player = roster.find((item) => item.id === playerId);
  if (!player) return false;
  setRoster(roster.filter((item) => item.id !== playerId));
  addTransaction('Drop', `Dropped ${player.name}`);
  return true;
}

async function updateRosterSlotApi(playerId, slot) {
  const league = getLeagueState();
  const requestedSlot = String(slot || '').toLowerCase();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    if (lineupLocked()) return false;
    return setRosterSlot(playerId, requestedSlot);
  }
  if (!league?.id) throw new Error('No server league selected');
  const roster = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/roster/${encodeURIComponent(playerId)}/slot`, {
    method: 'POST',
    body: JSON.stringify({ slot: requestedSlot })
  });
  setRoster((roster || []).map(normalizePlayer));
  return true;
}

async function scoreWeekApi(week = 1, season = new Date().getFullYear()) {
  const league = getLeagueState();
  const errors = lineupErrors(getRoster(), league);
  if (errors.length) {
    const error = new Error('Invalid lineup');
    error.lineupErrors = errors;
    throw error;
  }
  if (!getAuthState()?.token || isLocalDemoSession()) {
    const others = getMatchups().filter((matchup) => Number(matchup.week || 1) !== Number(week));
    const matchups = generateLocalMatchups(league, week);
    saveMatchups([...others, ...matchups]);
    return { season, week, scores: [], matchups };
  }
  if (!league?.id) throw new Error('No server league selected');
  const result = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/score/week/${encodeURIComponent(week)}`, {
    method: 'POST',
    body: JSON.stringify({ season })
  });
  if (Array.isArray(result?.matchups)) {
    const others = getMatchups().filter((matchup) => Number(matchup.week || 1) !== Number(week));
    saveMatchups([...others, ...result.matchups]);
  }
  return result;
}

async function generateSeasonScheduleApi(weeks = 12) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    return generateLocalSeasonSchedule(league, weeks);
  }
  if (!league?.id) throw new Error('No server league selected');
  const schedule = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/matchups/generate-season`, {
    method: 'POST',
    body: JSON.stringify({ weeks })
  });
  if (Array.isArray(schedule)) {
    saveMatchups(schedule);
  }
  return schedule;
}

async function finalizeWeekApi(week = 1) {
  const league = getLeagueState();
  const errors = lineupErrors(getRoster(), league);
  if (errors.length) {
    const error = new Error('Invalid lineup');
    error.lineupErrors = errors;
    throw error;
  }
  if (!getAuthState()?.token || isLocalDemoSession()) {
    const now = new Date().toISOString();
    const matchups = (getMatchups().length ? getMatchups() : generateLocalMatchups(league)).map((matchup) => (
      Number(matchup.week || 1) === Number(week)
        ? { ...matchup, status: 'final', finalizedAt: matchup.finalizedAt || now }
        : matchup
    ));
    saveMatchups(matchups);
    addTransaction('Scoring Finalized', `Finalized week ${week}`);
    return matchups;
  }
  if (!league?.id) throw new Error('No server league selected');
  const matchups = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/score/week/${encodeURIComponent(week)}/finalize`, {
    method: 'POST'
  });
  if (Array.isArray(matchups)) {
    const others = getMatchups().filter((matchup) => Number(matchup.week || 1) !== Number(week));
    saveMatchups([...others, ...matchups]);
  }
  return matchups;
}

function submitWaiverClaim(addPlayer, dropPlayerId = '') {
  if (lineupLocked()) return false;
  const player = normalizePlayer(addPlayer);
  const claims = getWaiverClaims();
  const auth = getAuthState();
  const claimOrder = claims
    .filter((claim) => claim.status === 'Pending' && (claim.managerEmail || auth?.email || '') === (auth?.email || ''))
    .reduce((max, claim) => Math.max(max, Number(claim.claimOrder || 0)), 0) + 1;
  claims.unshift({
    id: `waiver-${Date.now().toString(36)}`,
    addPlayer: player,
    dropPlayerId,
    status: 'Pending',
    managerEmail: auth?.email || '',
    priority: getWaiverPriority().find((item) => item.managerEmail === auth?.email)?.priority || 1,
    claimOrder,
    createdAt: new Date().toISOString()
  });
  saveWaiverClaims(claims);
  addTransaction('Waiver Claim', `Claimed ${player.name}`);
  return true;
}

function cancelWaiverClaim(claimId) {
  const auth = getAuthState();
  const commissioner = isCurrentCommissioner();
  const claims = getWaiverClaims();
  let cancelled = false;
  const next = claims.map((claim) => {
    const mine = !claim.managerEmail || claim.managerEmail === auth?.email;
    if (claim.id === claimId && claim.status === 'Pending' && (mine || commissioner)) {
      cancelled = true;
      return { ...claim, status: 'Cancelled' };
    }
    return claim;
  });
  if (cancelled) {
    saveWaiverClaims(next);
    addTransaction('Waiver Cancelled', 'Cancelled waiver claim');
  }
  return cancelled;
}

function reorderWaiverClaims(claimIds = []) {
  const auth = getAuthState();
  const orderById = new Map(claimIds.map((id, index) => [id, index + 1]));
  const claims = getWaiverClaims().map((claim) => {
    const mine = !claim.managerEmail || claim.managerEmail === auth?.email;
    if (mine && claim.status === 'Pending' && orderById.has(claim.id)) {
      return { ...claim, claimOrder: orderById.get(claim.id) };
    }
    return claim;
  });
  saveWaiverClaims(claims);
  return claims;
}

function processWaiverClaim(claimId) {
  if (lineupLocked()) return false;
  if (!waiverDeadlinePassed()) return false;
  const claims = getWaiverClaims();
  const claim = claims.find((item) => item.id === claimId);
  if (!claim) return false;
  if (claim.dropPlayerId) {
    dropPlayer(claim.dropPlayerId);
  }
  const roster = getRoster();
  const player = normalizePlayer(claim.addPlayer);
  const slot = assignRosterSlot(player, roster);
  if (!slot) return false;
  setRoster([...roster.filter((item) => item.id !== player.id), { ...player, rosterSlot: slot }]);
  addTransaction('Waiver Processed', `Added ${player.name}`);
  claim.status = 'Processed';
  saveWaiverClaims(claims);
  return true;
}

function processAllWaiverClaims() {
  if (lineupLocked()) {
    return { processed: [], cancelled: [], claims: getWaiverClaims() };
  }
  if (!waiverDeadlinePassed()) {
    return { processed: [], cancelled: [], claims: getWaiverClaims() };
  }
  const claims = getWaiverClaims().slice().sort((a, b) => (
    Number(a.priority || 999) - Number(b.priority || 999)
    || Number(a.claimOrder || 999) - Number(b.claimOrder || 999)
    || new Date(a.createdAt).getTime() - new Date(b.createdAt).getTime()
  ));
  const processed = [];
  claims.forEach((claim) => {
    if (claim.status === 'Pending') {
      if (processWaiverClaim(claim.id)) processed.push(claim.id);
    }
  });
  return { processed, cancelled: [], claims: getWaiverClaims() };
}

function submitTradeOffer(offerPlayerId, requestPlayerName, targetManager, requestPlayer = null, note = '') {
  const player = getRoster().find((item) => item.id === offerPlayerId);
  if (lineupLocked() || !player || !requestPlayer?.id || playerLockedInTrade(offerPlayerId) || !isActiveTradeTarget(targetManager)) return false;
  const rules = tradeRules();
  const auth = getAuthState();
  const offers = getTradeOffers();
  offers.unshift({
    id: `trade-${Date.now().toString(36)}`,
    offerPlayer: player,
    requestPlayer: requestPlayer ? normalizePlayer(requestPlayer) : null,
    requestPlayerName,
    targetManager,
    offeredByEmail: auth?.email || '',
    offeredToEmail: targetManager || '',
    note,
    requiresApproval: Boolean(rules.commissionerApproval),
    expiresAt: tradeExpiresAt(),
    status: 'Pending',
    createdAt: new Date().toISOString()
  });
  saveTradeOffers(offers);
  addTransaction('Trade Offer', `Offered ${player.name} to ${managerDisplayName(targetManager) || 'another manager'}`);
  return true;
}

function updateTradeStatus(tradeId, status) {
  const offers = getTradeOffers();
  const trade = offers.find((item) => item.id === tradeId);
  if (!trade || !isOpenTradeStatus(trade.status)) return;
  const authEmail = getAuthState()?.email || '';
  const commissioner = isCurrentCommissioner();
  if (status === 'Accepted' && trade.offeredToEmail && trade.offeredToEmail !== authEmail && !commissioner) return;
  if (status === 'Declined' && trade.offeredToEmail && trade.offeredToEmail !== authEmail && !commissioner) return;
  if (status === 'Cancelled' && trade.offeredByEmail && trade.offeredByEmail !== authEmail && !commissioner) return;
  if ((status === 'Approved' || status === 'Vetoed') && !commissioner) return;
  const executeTrade = status === 'Approved' || (status === 'Accepted' && !trade.requiresApproval);
  if (executeTrade && lineupLocked()) return;
  trade.status = executeTrade ? 'Approved' : status;
  if (executeTrade && trade.requestPlayer?.id) {
    const roster = getRoster();
    setRoster(roster.filter((player) => player.id !== trade.offerPlayer.id).concat([trade.requestPlayer]));
  }
  addTransaction('Trade', `${trade.status}: ${trade.offerPlayer.name}`);
  saveTradeOffers(offers);
}

function updateSharedNav(activePage = '') {
  const authState = getAuthState();
  const navSignInBtn = document.getElementById('nav-sign-in');
  const navLogoutBtn = document.getElementById('nav-logout');
  document.querySelectorAll('.nav__link').forEach((link) => {
    link.classList.toggle('is-active', link.dataset.page === activePage);
  });
  if (navSignInBtn) {
    navSignInBtn.textContent = authState?.email || 'Sign in';
    navSignInBtn.href = authState ? 'league.html' : 'signin.html';
  }
  if (navLogoutBtn) {
    navLogoutBtn.hidden = !authState;
    navLogoutBtn.onclick = () => {
      clearSessionState();
      window.location.href = 'index.html';
    };
  }
}
