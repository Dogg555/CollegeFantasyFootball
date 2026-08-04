'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  ALLOWED_TEAM_COUNTS,
  canonicalEmail,
  validEmail,
  createOperationId,
  normalizeInviteList,
  parseInviteText,
  validateCreatePayload,
  stableCreateFingerprint,
  pendingCreate,
  clearPendingCreate,
  joinOperation,
  clearJoinOperation,
  uncertainFailure,
  onboardingMessage
} = require(path.join('..', 'league-onboarding.js'));

class MemoryStorage {
  constructor() {
    this.values = new Map();
  }

  getItem(key) {
    return this.values.has(key) ? this.values.get(key) : null;
  }

  setItem(key, value) {
    this.values.set(key, String(value));
  }

  removeItem(key) {
    this.values.delete(key);
  }
}

assert.deepEqual(ALLOWED_TEAM_COUNTS, [4, 6, 8, 10, 12, 14, 16]);
assert.equal(canonicalEmail('  Manager@Example.COM '), 'manager@example.com');
assert.equal(validEmail('manager@example.com'), true);
assert.equal(validEmail('not-an-email'), false);
assert.match(createOperationId(null, () => 1000, () => 0.25), /^league-/);
assert.deepEqual(parseInviteText('a@example.com, b@example.com\nc@example.com'), [
  'a@example.com',
  'b@example.com',
  'c@example.com'
]);

const normalized = normalizeInviteList([
  'A@example.com',
  'a@example.com',
  'owner@example.com',
  'invalid',
  'b@example.com'
], 'OWNER@example.com');
assert.deepEqual(normalized.invites, ['a@example.com', 'b@example.com']);
assert.deepEqual(normalized.invalid, ['invalid']);

for (const teams of [4, 6]) {
  const result = validateCreatePayload({
    name: `${teams} Team Test`,
    teams,
    scoring: 'ppr',
    draftType: 'snake',
    draftDate: '2030-08-22T18:00:00Z',
    invitedEmails: Array.from({ length: teams - 1 }, (_, index) => `manager${index}@example.com`)
  }, 'owner@example.com', Date.parse('2026-08-04T16:00:00Z'));
  assert.equal(result.ok, true, `${teams}-team testing leagues must be accepted`);
  assert.equal(result.payload.invitedEmails.length, teams - 1);
}

const oddSize = validateCreatePayload({
  name: 'Odd League',
  teams: 5,
  invitedEmails: []
}, 'owner@example.com');
assert.equal(oddSize.ok, false);
assert.match(oddSize.errors.join(' '), /4, 6, 8, 10, 12, 14, or 16/);

const overInvited = validateCreatePayload({
  name: 'Four Team Test',
  teams: 4,
  invitedEmails: ['a@example.com', 'b@example.com', 'c@example.com', 'd@example.com']
}, 'owner@example.com');
assert.equal(overInvited.ok, false);
assert.match(overInvited.errors.join(' '), /at most 3/);

const invalidDraftDate = validateCreatePayload({
  name: 'Past Draft',
  teams: 4,
  draftDate: '2025-01-01T00:00:00Z',
  invitedEmails: []
}, 'owner@example.com', Date.parse('2026-08-04T16:00:00Z'));
assert.equal(invalidDraftDate.ok, false);
assert.match(invalidDraftDate.errors.join(' '), /future/);

const storage = new MemoryStorage();
const payload = {
  name: 'Idempotent League',
  teams: 4,
  scoring: 'ppr',
  draftType: 'snake',
  invitedEmails: ['a@example.com']
};
let sequence = 0;
const createId = () => `operation-${++sequence}`;
const first = pendingCreate(storage, payload, 'owner@example.com', createId);
const replay = pendingCreate(storage, { ...payload }, 'OWNER@example.com', createId);
assert.equal(first.operationKey, 'operation-1');
assert.equal(replay.operationKey, first.operationKey, 'same create payload must reuse its operation key');
assert.equal(stableCreateFingerprint(payload, 'owner@example.com'), first.fingerprint);

const changed = pendingCreate(storage, { ...payload, name: 'Different League' }, 'owner@example.com', createId);
assert.equal(changed.operationKey, 'operation-2', 'changed settings must begin a new operation');
clearPendingCreate(storage, 'wrong-operation');
assert.ok(storage.getItem('cff_league_create_operation'));
clearPendingCreate(storage, changed.operationKey);
assert.equal(storage.getItem('cff_league_create_operation'), null);

const joins = new MemoryStorage();
const firstJoin = joinOperation(joins, 'league-123', 'manager@example.com', createId);
const replayJoin = joinOperation(joins, 'league-123', 'MANAGER@example.com', createId);
assert.equal(firstJoin.operationKey, replayJoin.operationKey, 'join retries must reuse one operation key');
clearJoinOperation(joins, 'league-123', 'manager@example.com');
const nextJoin = joinOperation(joins, 'league-123', 'manager@example.com', createId);
assert.notEqual(nextJoin.operationKey, firstJoin.operationKey);

assert.equal(uncertainFailure({ timedOut: true }), true);
assert.equal(uncertainFailure({ status: 503 }), true);
assert.equal(uncertainFailure({ status: 409, data: { code: 'league_create_conflict' } }), true);
assert.equal(uncertainFailure({ status: 409, data: { code: 'league_full' } }), false);
assert.match(onboardingMessage({ data: { code: 'league_full' } }), /league is full/i);
assert.match(onboardingMessage({ timedOut: true }), /Retry safely/i);
assert.match(onboardingMessage({ data: { code: 'unsupported_team_count' } }), /4, 6, 8/);

console.log('league onboarding runtime tests passed');
