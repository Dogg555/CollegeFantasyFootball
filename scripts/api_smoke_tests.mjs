#!/usr/bin/env node

const baseUrl = (process.env.CFF_API_BASE_URL || 'http://127.0.0.1:8080').replace(/\/$/, '');
const password = process.env.CFF_SMOKE_PASSWORD || 'SmokeTest123!';
const emailPrefix = process.env.CFF_SMOKE_EMAIL_PREFIX || 'smoke';

class SmokeFailure extends Error {}

async function request(method, path, body = undefined, token = '', expected = [200]) {
  const headers = { Accept: 'application/json' };
  const init = { method, headers };
  if (body !== undefined) {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }
  if (token) headers.Authorization = `Bearer ${token}`;

  let response;
  let text;
  try {
    response = await fetch(`${baseUrl}${path}`, init);
    text = await response.text();
  } catch (error) {
    throw new SmokeFailure(`${method} ${path} could not connect to ${baseUrl}: ${error.message}`);
  }

  if (!expected.includes(response.status)) {
    throw new SmokeFailure(`${method} ${path} expected ${expected.join('/')}, got ${response.status}: ${text}`);
  }
  if (!text) return {};
  try {
    return JSON.parse(text);
  } catch {
    return { raw: text };
  }
}

function assertTrue(condition, message) {
  if (!condition) throw new SmokeFailure(message);
}

function smokePlayer(suffix, id, name, position, projection = 10) {
  return {
    id: `${id}-${suffix}`,
    name,
    team: 'Test State',
    position,
    conference: 'Smoke',
    projection,
    rank: 99
  };
}

async function main() {
  const suffix = String(Math.floor(Date.now() / 1000));
  const email = `${emailPrefix}+${suffix}@example.com`;
  const managerEmail = `${emailPrefix}-manager+${suffix}@example.com`;

  const health = await request('GET', '/health');
  assertTrue(['ok', 'degraded'].includes(health.status), `unexpected health payload: ${JSON.stringify(health)}`);

  const apiHealth = await request('GET', '/api/health');
  assertTrue(apiHealth.service === 'college-ff-api', `unexpected api health payload: ${JSON.stringify(apiHealth)}`);

  await request('GET', '/api/leagues', undefined, '', [401]);

  const signup = await request('POST', '/api/auth/signup', { email, password }, '', [201]);
  const token = signup.token;
  assertTrue(token, `signup did not return token: ${JSON.stringify(signup)}`);

  const validate = await request('GET', '/api/auth/validate', undefined, token);
  assertTrue(validate.valid === true, `token did not validate: ${JSON.stringify(validate)}`);

  const login = await request('POST', '/api/auth/login', { email, password });
  const loginToken = login.token;
  assertTrue(loginToken, `login did not return token: ${JSON.stringify(login)}`);

  await request('POST', '/api/auth/logout', {}, loginToken);
  await request('GET', '/api/auth/validate', undefined, loginToken, [401]);

  const leaguePayload = {
    name: `Smoke League ${suffix}`,
    teams: 10,
    scoring: 'ppr',
    draftType: 'snake',
    invitedEmails: [managerEmail],
    rosterRules: { qb: 0, rb: 0, wr: 0, te: 0, flex: 0, bench: 8 },
    waiverRules: {
      mode: 'waivers',
      claimDeadline: '2000-01-01T00:00:00Z',
      freeAgencyLocked: true
    },
    tradeRules: {
      commissionerApproval: false,
      expirationHours: 48
    }
  };
  const league = await request('POST', '/api/leagues', leaguePayload, token, [201]);
  const leagueId = league.id;
  assertTrue(leagueId, `league create did not return id: ${JSON.stringify(league)}`);

  const leagues = await request('GET', '/api/leagues', undefined, token);
  assertTrue(leagues.some((item) => item.id === leagueId), 'created league missing from list');

  const settingsUpdate = { ...league, notes: 'smoke settings update' };
  const updated = await request('PUT', `/api/leagues/${leagueId}`, settingsUpdate, token);
  assertTrue(updated.notes === 'smoke settings update', `league update failed: ${JSON.stringify(updated)}`);

  const managerSignup = await request('POST', '/api/auth/signup', { email: managerEmail, password }, '', [201]);
  const managerToken = managerSignup.token;
  assertTrue(managerToken, `manager signup did not return token: ${JSON.stringify(managerSignup)}`);

  const joined = await request('POST', `/api/leagues/${leagueId}/join`, {}, managerToken);
  assertTrue(joined.id === leagueId, `manager could not join invited league: ${JSON.stringify(joined)}`);

  const members = await request('GET', `/api/leagues/${leagueId}/members`, undefined, token);
  const memberEmails = new Set(members.map((member) => member.email));
  assertTrue(memberEmails.has(email) && memberEmails.has(managerEmail), `joined members missing: ${JSON.stringify(members)}`);

  const orderState = await request('PUT', `/api/leagues/${leagueId}/draft/order`, { draftOrder: [email, managerEmail] }, token);
  assertTrue(JSON.stringify(orderState.draftOrder) === JSON.stringify([email, managerEmail]), `draft order was not saved: ${JSON.stringify(orderState)}`);

  const commPlayer = smokePlayer(suffix, 'smoke-rb', 'Smoke Test RB', 'RB', 18.4);
  const managerPlayer = smokePlayer(suffix, 'smoke-wr', 'Smoke Test WR', 'WR', 17.2);
  const extraPlayer = smokePlayer(suffix, 'smoke-qb', 'Smoke Test QB', 'QB', 22.1);

  const firstPick = await request('POST', `/api/leagues/${leagueId}/draft/picks`, { player: commPlayer }, token, [201]);
  assertTrue(firstPick.currentManager === managerEmail, `pick 2 should belong to manager: ${JSON.stringify(firstPick)}`);

  await request('POST', `/api/leagues/${leagueId}/draft/picks`, { player: managerPlayer }, managerToken, [201]);
  await request('POST', `/api/leagues/${leagueId}/draft/picks`, { player: smokePlayer(suffix, 'bad-turn', 'Smoke Bad Turn', 'TE') }, token, [409]);
  const snakeTurn = await request('POST', `/api/leagues/${leagueId}/draft/picks`, { player: extraPlayer }, managerToken, [201]);
  assertTrue(snakeTurn.currentManager === email, `pick 4 should return to commissioner: ${JSON.stringify(snakeTurn)}`);

  const undoState = await request('POST', `/api/leagues/${leagueId}/draft/undo`, {}, token);
  assertTrue((undoState.picks || []).length === 2, `draft undo did not remove last pick: ${JSON.stringify(undoState)}`);

  const pendingWaiverPlayer = smokePlayer(suffix, 'waiver-pending', 'Smoke Pending Waiver', 'QB', 9.8);
  const pendingWaiver = await request('POST', `/api/leagues/${leagueId}/waivers`, {
    addPlayer: pendingWaiverPlayer,
    dropPlayerId: commPlayer.id
  }, token, [201]);
  assertTrue(pendingWaiver.id, `pending waiver missing id: ${JSON.stringify(pendingWaiver)}`);

  await request('POST', `/api/leagues/${leagueId}/score/week/1`, { season: 2026 }, token);
  const finalized = await request('POST', `/api/leagues/${leagueId}/score/week/1/finalize`, {}, token);
  assertTrue(finalized.some((matchup) => String(matchup.status).toLowerCase() === 'final'), `week was not finalized: ${JSON.stringify(finalized)}`);

  await request('POST', `/api/leagues/${leagueId}/roster`, { player: smokePlayer(suffix, 'locked-add', 'Smoke Locked Add', 'TE') }, token, [409]);
  await request('POST', `/api/leagues/${leagueId}/roster/drop`, { playerId: commPlayer.id }, token, [409]);
  await request('POST', `/api/leagues/${leagueId}/roster/${commPlayer.id}/slot`, { slot: 'bench' }, token, [409]);
  await request('POST', `/api/leagues/${leagueId}/waivers`, { addPlayer: smokePlayer(suffix, 'locked-waiver', 'Smoke Locked Waiver', 'TE'), dropPlayerId: commPlayer.id }, token, [409]);
  await request('POST', `/api/leagues/${leagueId}/waivers/${pendingWaiver.id}/process`, {}, token, [409]);
  await request('POST', `/api/leagues/${leagueId}/waivers/${pendingWaiver.id}/status`, { status: 'Cancelled' }, token);
  await request('POST', `/api/leagues/${leagueId}/trades`, {
    offerPlayer: commPlayer,
    requestPlayer: managerPlayer,
    requestPlayerName: managerPlayer.name,
    targetManager: managerEmail
  }, token, [409]);

  await request('GET', '/api/admin/ingest/cfbd/status', undefined, token, [403]);

  const transactions = await request('GET', `/api/leagues/${leagueId}/transactions`, undefined, token);
  assertTrue(Array.isArray(transactions), `transactions response is not a list: ${JSON.stringify(transactions)}`);

  const reset = await request('POST', '/api/auth/request-password-reset', { email });
  if (reset.passwordResetToken) {
    const newPassword = `${password}Reset`;
    await request('POST', '/api/auth/reset-password', { token: reset.passwordResetToken, password: newPassword });
    await request('GET', '/api/auth/validate', undefined, token, [401]);
    const relogin = await request('POST', '/api/auth/login', { email, password: newPassword });
    assertTrue(relogin.token, `password reset login failed: ${JSON.stringify(relogin)}`);
  }

  console.log(JSON.stringify({
    status: 'ok',
    baseUrl,
    email,
    managerEmail,
    leagueId
  }, null, 2));
}

main().catch((error) => {
  const payload = {
    status: 'failed',
    error: error.message
  };
  console.error(JSON.stringify(payload, null, 2));
  process.exit(1);
});
