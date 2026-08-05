'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const polishPath = path.join(__dirname, '..', 'polish-core.js');
const source = fs.readFileSync(polishPath, 'utf8');

assert.match(source, /const NOTIFICATION_DEDUP_MS = 8000;/, 'duplicate notifications should be suppressed');
assert.match(source, /const notificationHistory = new Map\(\);/, 'notification history should be tracked');
assert.match(
  source,
  /function notify[\s\S]*now - lastShownAt < NOTIFICATION_DEDUP_MS[\s\S]*return;/,
  'notify should ignore identical recent messages'
);

const requestErrorHandler = source.match(/document\.addEventListener\('cff:request-error',[\s\S]*?\n    \}\);/);
assert.ok(requestErrorHandler, 'request error handler should exist');
assert.match(requestErrorHandler[0], /setNetworkStatus\(false, message\);/);
assert.doesNotMatch(requestErrorHandler[0], /notify\(/, 'request failures should use one persistent outage surface');

const offlineHandler = source.match(/window\.addEventListener\('offline',[\s\S]*?\n    \}\);/);
assert.ok(offlineHandler, 'offline handler should exist');
assert.doesNotMatch(offlineHandler[0], /notify\(/, 'offline state should not duplicate the persistent banner');

assert.match(source, /Retry page/, 'retry action should remain a distinct button label');
assert.match(source, /function positionToastRegion\(\)/, 'mobile toasts should be positioned below the active banner');
assert.match(source, /anchor\?\.getBoundingClientRect\(\)\.bottom/, 'toast offset should use the rendered banner position');

assert.match(source, /function setMutationControlsUnavailable\(unavailable\)/, 'outage state should fail closed');
assert.match(source, /form:not\(\[method="get"\]\).*button\[type="submit"\]/, 'non-GET submit controls should be disabled');
assert.match(source, /data-cff-network-disabled/, 'only controls disabled by the outage layer should be restored');
assert.match(source, /document\.addEventListener\('submit',[\s\S]*event\.preventDefault\(\)/, 'mutations should also be blocked at submit time');
assert.match(source, /status >= 500[\s\S]*setNetworkStatus\(false/, 'server outages should disable mutation controls');

console.log('outage feedback contracts passed');
