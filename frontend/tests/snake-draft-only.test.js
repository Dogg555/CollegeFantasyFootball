'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  SNAKE_DRAFT_TYPE,
  FUTURE_RELEASE_LABEL,
  FUTURE_RELEASE_MESSAGE,
  normalizeDraftType,
  supportedDraftType
} = require(path.join('..', 'snake-draft-only.js'));

assert.equal(SNAKE_DRAFT_TYPE, 'snake');
assert.equal(normalizeDraftType(' SNAKE '), 'snake');
assert.equal(supportedDraftType('snake'), true);
assert.equal(supportedDraftType(''), true, 'omitted draft type must retain the snake default');
assert.equal(supportedDraftType('auction'), false);
assert.equal(supportedDraftType('linear'), false);
assert.match(FUTURE_RELEASE_LABEL, /coming in future release/i);
assert.match(FUTURE_RELEASE_MESSAGE, /auction drafts are coming in a future release/i);

console.log('snake draft only frontend tests passed');
