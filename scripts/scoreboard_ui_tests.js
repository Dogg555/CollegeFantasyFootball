'use strict';

const assert = require('node:assert/strict');
const {
  normalizeGame,
  availableWeeks,
  chooseDefaultWeek,
  buildSlides,
} = require('../frontend/scoreboard.js');

const games = [
  normalizeGame({ id: 1, week: 2, startDate: '2026-09-05T16:00:00Z', away: 'A', home: 'B' }),
  normalizeGame({ id: 2, week: 1, startDate: '2026-08-29T19:30:00Z', away: 'C', home: 'D' }),
  normalizeGame({ id: 3, week: 1, startDate: '2026-08-29T19:30:00Z', away: 'E', home: 'F', live: true, quarter: 2 }),
  normalizeGame({ id: 4, week: 1, startDate: '2026-08-29T23:00:00Z', away: 'G', home: 'H' }),
];

assert.deepEqual(availableWeeks(games), [1, 2]);
assert.equal(chooseDefaultWeek(games, new Date('2026-08-29T20:00:00Z')), 1);

const slides = buildSlides(games, 1, 2);
assert.equal(slides.length, 2, 'separate kickoff times should become separate slides');
assert.equal(slides[0].games.length, 2);
assert.equal(slides[1].games.length, 1);
assert.ok(slides[0].games[0].startDate <= slides[1].games[0].startDate);

const fiveAtSameTime = Array.from({ length: 5 }, (_, index) => normalizeGame({
  id: `same-${index}`,
  week: 3,
  startDate: '2026-09-12T16:00:00Z',
  away: `Away ${index}`,
  home: `Home ${index}`,
}));
const paged = buildSlides(fiveAtSameTime, 3, 4);
assert.equal(paged.length, 2);
assert.deepEqual(paged.map((slide) => slide.games.length), [4, 1]);

console.log('scoreboard UI tests passed');
