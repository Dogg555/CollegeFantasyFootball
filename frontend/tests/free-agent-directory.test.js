'use strict';

const assert = require('node:assert/strict');
const directory = require('../free-agent-directory.js');

const rules = { qb: 1, rb: 1, wr: 1, te: 0, flex: 1, k: 0, def: 0, bench: 2 };
const roster = [
  { id: 'qb-start', name: 'QB Start', position: 'QB', rosterSlot: 'qb' },
  { id: 'rb-start', name: 'RB Start', position: 'RB', rosterSlot: 'rb' },
  { id: 'wr-start', name: 'WR Start', position: 'WR', rosterSlot: 'wr' },
  { id: 'te-flex', name: 'TE Flex', position: 'TE', rosterSlot: 'flex' },
  { id: 'qb-bench', name: 'QB Bench', position: 'QB', rosterSlot: 'bench' },
  { id: 'rb-bench', name: 'RB Bench', position: 'RB', rosterSlot: 'bench' }
];

assert.equal(directory.rosterLimit(rules), 6, 'configured roster capacity should include starters and bench');
assert.equal(directory.requiresDrop(roster, rules), true, 'full roster should require a drop');
assert.equal(directory.playerPoolEligible({ position: 'OL' }, rules), false, 'offensive linemen are not fantasy-eligible');
assert.equal(directory.playerPoolEligible({ position: 'TE' }, rules), true, 'TE remains eligible through FLEX');
assert.equal(directory.destinationSlot({ position: 'QB' }, roster, rules), '', 'full roster should not have an add destination');
assert.equal(directory.destinationSlot({ position: 'QB' }, roster, rules, 'qb-bench'), 'bench', 'dropping a bench QB should open bench capacity');
assert.equal(directory.destinationSlot({ position: 'QB' }, roster, rules, 'wr-start'), '', 'dropping an unrelated starter must not create an illegal QB destination');

const candidates = directory.eligibleDropCandidates(
  { id: 'new-qb', position: 'QB' },
  roster,
  rules,
  [{ playerId: 'qb-bench', locked: true }]
);
assert.deepEqual(
  candidates.map((player) => player.id),
  ['qb-start', 'rb-bench'],
  'locked drops must be excluded while other structurally valid drops remain available'
);

const allValidDropsLocked = directory.eligibleDropCandidates(
  { id: 'new-qb', position: 'QB' },
  roster,
  rules,
  [
    { playerId: 'qb-start', locked: true },
    { playerId: 'qb-bench', locked: true },
    { playerId: 'rb-bench', locked: true }
  ]
);
assert.deepEqual(allValidDropsLocked, [], 'no drop should be offered when every structurally valid drop is locked');

const unlocked = directory.eligibleDropCandidates({ id: 'new-qb', position: 'QB' }, roster, rules, []);
assert.deepEqual(
  unlocked.map((player) => player.id),
  ['qb-start', 'qb-bench', 'rb-bench'],
  'every drop that creates a legal QB destination should be offered'
);

const preview = directory.buildRosterPreview({ id: 'new-qb', name: 'New QB', position: 'QB' }, roster, rules, 'qb-start');
assert.equal(preview.valid, true);
assert.equal(preview.destination, 'qb');
assert.equal(preview.rosterCountBefore, 6);
assert.equal(preview.rosterCountAfter, 6);
assert.equal(preview.resultingRoster.some((player) => player.id === 'qb-start'), false);
assert.equal(preview.resultingRoster.some((player) => player.id === 'new-qb' && player.rosterSlot === 'qb'), true);

assert.deepEqual(directory.availabilityAction({ availability: 'available' }), { label: 'Add', enabled: true, action: 'add' });
assert.equal(directory.availabilityAction({ availability: 'waivers' }).enabled, false);
assert.equal(directory.availabilityAction({ availability: 'owned' }).label, 'Rostered');
assert.equal(directory.availabilityAction({ availability: 'available' }, false).label, 'Select a league');

console.log('free-agent directory browser contracts passed');
