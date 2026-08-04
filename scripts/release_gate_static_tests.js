#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.join(__dirname, '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

const main = read('backend/src/main.cpp');
assert.match(main, /bool dbPersistToken\(/, 'dbPersistToken must return success/failure');
assert.match(main, /if \(!dbPersistToken\(token, email\)\) \{\s*return std::nullopt;/s, 'token issuance must fail if DB token persistence fails');
assert.match(main, /healthStatusCode\(payload\)/, 'health handler must derive HTTP status from health payload');
assert.doesNotMatch(main, /struct RateLimitBucket/, 'main.cpp must not retain duplicate local rate limiter');

const config = read('frontend/config.js');
assert.doesNotThrow(() => new vm.Script(config, { filename: 'frontend/config.js' }), 'shared frontend config must be valid JavaScript');
assert.match(config, /'api-client\.js'/, 'shared loader must retain the API client');
assert.match(config, /'auth-session-sync\.js'/, 'shared loader must retain auth session synchronization');
assert.match(config, /'league-onboarding\.js'/, 'shared loader must retain league onboarding reliability');
assert.match(config, /'landing-refresh\.js'/, 'shared loader must retain the landing-page refresh');
assert.match(config, /'landing-refresh\.css'/, 'shared loader must retain the landing-page stylesheet');

const state = read('frontend/state.js');
assert.match(state, /window\.CFF_ALLOW_LOCAL_DEMO === true/, 'local demo must default fail-closed');
assert.match(state, /localhostDemoAllowed\(\)/, 'local demo must be localhost-gated');
assert.match(state, /lastAuthSessionResult = \{ authenticated: false, unavailable: true/s, 'validation network errors must be unavailable, not authenticated');
assert.match(state, /retryAfter = resp\.headers\.get\('Retry-After'\)/, 'API errors must preserve Retry-After');
assert.match(state, /CFF_API_CACHE_META_KEY/, 'API cache metadata must be recorded');

const league = read('frontend/league.js');
assert.doesNotMatch(league, /catch\s*\{[\s\S]{0,120}(removeLeagueForCurrentAccount|setLeagueState|submitWaiverClaim|submitTradeOffer|updateTradeStatus|addFreeAgent|dropPlayer)\(/, 'league mutations must not fall back to local success in catch blocks');
assert.match(league, /api-stale-warning/, 'league page must show stale-data warning');

const draft = read('frontend/draft.js');
assert.doesNotMatch(draft, /catch\s*\{[\s\S]{0,120}(draftPlayer|clearDraftState|undoLastDraftPick|saveDraftOrder|setRoster|addPlayerToQueue)\(/, 'draft mutations must not fall back to local success in catch blocks');
assert.match(draft, /draft-stale-warning/, 'draft page must show stale-data warning');

console.log('release gate static tests passed');
