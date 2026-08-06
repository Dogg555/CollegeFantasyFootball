'use strict';

const assert = require('node:assert/strict');
const helpers = require('../lineup-management.js');

const rules = {
  qb: 1,
  rb: 2,
  wr: 2,
  te: 1,
  flex: 1,
  bench: 4
};

function player(id, position, rosterSlot = 'bench') {
  return { id, name: id, position, rosterSlot };
}

function testStarterSlotsRemainVisibleWhenEmpty() {
  const slots = helpers.starterSlots(rules);
  assert.equal(slots.length, 7);
  assert.deepEqual(slots.map((slot) => slot.label), [
    'QB', 'RB 1', 'RB 2', 'WR 1', 'WR 2', 'TE', 'FLEX'
  ]);
  assert.equal(helpers.emptyStarterCount([], rules), 7);
}

function testEmptyLineupsAreValid() {
  assert.deepEqual(helpers.lineupErrorsAllowEmpty([], { rosterRules: rules }), []);
  const partial = [player('qb', 'QB', 'qb'), player('rb', 'RB', 'rb')];
  assert.deepEqual(helpers.lineupErrorsAllowEmpty(partial, { rosterRules: rules }), []);
  assert.equal(helpers.emptyStarterCount(partial, rules), 5);
}

function testPositionEligibility() {
  assert.equal(helpers.positionEligible(player('rb', 'RB'), 'rb'), true);
  assert.equal(helpers.positionEligible(player('rb', 'RB'), 'flex'), true);
  assert.equal(helpers.positionEligible(player('wr', 'WR'), 'flex'), true);
  assert.equal(helpers.positionEligible(player('te', 'TE'), 'flex'), true);
  assert.equal(helpers.positionEligible(player('qb', 'QB'), 'flex'), false);
  assert.equal(helpers.positionEligible(player('wr', 'WR'), 'rb'), false);
}

function testLegalDestinationsRespectCapacity() {
  const roster = [
    player('rb-1', 'RB', 'rb'),
    player('rb-2', 'RB', 'rb'),
    player('rb-bench', 'RB', 'bench')
  ];
  assert.deepEqual(
    helpers.legalDestinations(roster[2], roster, rules),
    ['flex']
  );

  roster.push(player('flex-1', 'WR', 'flex'));
  assert.deepEqual(helpers.legalDestinations(roster[2], roster, rules), []);
}

function testInvalidAndOverfilledLineupsStillFail() {
  const roster = [
    player('qb-1', 'QB', 'qb'),
    player('qb-2', 'QB', 'qb'),
    player('wrong', 'WR', 'rb')
  ];
  const errors = helpers.lineupErrorsAllowEmpty(roster, { rosterRules: rules });
  assert.equal(errors.length, 2);
  assert.match(errors[0].message, /not eligible/i);
  assert.match(errors[1].message, /too many QB/i);
}

function testWeeklyPlayerLocks() {
  const context = { season: 2026, week: 3 };
  const state = {
    season: 2026,
    week: 3,
    weekLocked: false,
    players: [{ playerId: 'locked', season: 2026, week: 3, locked: true }]
  };
  assert.equal(helpers.playerLocked(player('locked', 'RB'), state, context), true);
  assert.equal(helpers.playerLocked(player('open', 'RB'), state, context), false);
  assert.equal(helpers.playerLocked(player('locked', 'RB'), state, { season: 2026, week: 4 }), false);
  assert.equal(helpers.playerLocked(player('open', 'RB'), { ...state, weekLocked: true }, context), true);
}

function testStarterAndBenchGrouping() {
  const roster = [
    player('qb', 'QB', 'qb'),
    player('bench', 'WR', 'bench'),
    player('unknown', 'WR', 'taxi')
  ];
  const grouped = helpers.groupRoster(roster, rules);
  assert.equal(grouped.startersBySlot.get('qb').length, 1);
  assert.deepEqual(grouped.bench.map((entry) => entry.id), ['bench', 'unknown']);
}

testStarterSlotsRemainVisibleWhenEmpty();
testEmptyLineupsAreValid();
testPositionEligibility();
testLegalDestinationsRespectCapacity();
testInvalidAndOverfilledLineupsStillFail();
testWeeklyPlayerLocks();
testStarterAndBenchGrouping();
console.log('lineup management frontend tests passed');
