#!/usr/bin/env node
'use strict';

const { test, expect } = require('@playwright/test');

const baseUrl = process.env.CFF_FRONTEND_URL || 'http://127.0.0.1:8080';

test('login network failure creates no session', async ({ page }) => {
  await page.route('**/api/auth/login', (route) => route.abort());
  await page.goto(`${baseUrl}/signin.html`);
  await page.fill('#login-email', 'beta@example.com');
  await page.fill('#login-password', 'Password123!');
  await page.click('#login-form button[type="submit"]');
  await expect(page.locator('#login-status')).toContainText(/unavailable|disabled/i);
  const session = await page.evaluate(() => sessionStorage.getItem('cff_auth'));
  expect(session).toBeNull();
});

test('validation outage does not count as authenticated', async ({ page }) => {
  await page.addInitScript(() => sessionStorage.setItem('cff_auth', JSON.stringify({ email: 'beta@example.com', token: 'token-test' })));
  await page.route('**/auth/validate', (route) => route.fulfill({
    status: 503,
    contentType: 'application/json',
    body: JSON.stringify({ valid: false, unavailable: true })
  }));
  await page.goto(`${baseUrl}/league.html`);
  await expect(page.locator('html')).toHaveAttribute('data-cff-private-auth', 'cached');
});

test('401 validation clears session', async ({ page }) => {
  await page.goto(`${baseUrl}/index.html`);
  await page.evaluate(() => sessionStorage.setItem('cff_auth', JSON.stringify({ email: 'beta@example.com', token: 'token-test' })));
  await page.route('**/auth/validate', (route) => route.fulfill({
    status: 401,
    contentType: 'application/json',
    body: JSON.stringify({ valid: false })
  }));
  await page.goto(`${baseUrl}/league.html`);
  await expect(page).toHaveURL(/signin\.html/);
  const session = await page.evaluate(() => sessionStorage.getItem('cff_auth'));
  expect(session).toBeNull();
});

test('failed league creation creates no local league', async ({ page }) => {
  await page.addInitScript(() => sessionStorage.setItem('cff_auth', JSON.stringify({ email: 'beta@example.com', token: 'token-test' })));
  await page.route('**/api/leagues', (route) => route.fulfill({
    status: 503,
    contentType: 'application/json',
    body: JSON.stringify({ error: 'unavailable' })
  }));
  await page.goto(`${baseUrl}/index.html`);
  await page.click('.js-open-league');
  await page.fill('#league-name', 'Failure Mode League');
  await page.click('form button[type="submit"]');
  await expect(page.locator('#form-status')).toContainText(/unavailable|no local|retry safely|same operation/i);
  const localLeagues = await page.evaluate(() => localStorage.getItem('cff_leagues_by_account'));
  expect(localLeagues).toBeNull();
});

test('429 mutation shows retry guidance', async ({ page }) => {
  await page.addInitScript(() => {
    sessionStorage.setItem('cff_auth', JSON.stringify({ email: 'beta@example.com', token: 'token-test' }));
    localStorage.setItem('cff_leagues_by_account', JSON.stringify({
      'beta@example.com': {
        activeLeagueId: 'league-test',
        leagues: [{ id: 'league-test', name: 'Beta', teams: 4, members: [{ email: 'beta@example.com', role: 'commissioner', status: 'Active' }] }]
      }
    }));
  });
  await page.route('**/schedule/transactions', (route) => route.fulfill({
    status: 429,
    headers: { 'Retry-After': '60' },
    contentType: 'application/json',
    body: JSON.stringify({ error: 'Too many requests' })
  }));
  await page.route('**/schedule/state**', (route) => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({
      leagueId: 'league-test',
      season: new Date().getFullYear(),
      week: 1,
      version: 0,
      scheduleVersion: 0,
      schedule: []
    })
  }));
  await page.route('**/auth/validate', (route) => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({ valid: true, email: 'beta@example.com' })
  }));
  await page.goto(`${baseUrl}/league.html#scoreboard`);
  await expect(page.locator('#generate-season')).toBeVisible();
  await page.click('#generate-season');
  await expect(page.locator('#score-week-status')).toContainText(/retry after 60/i);
});
