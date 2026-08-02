# Manual Alpha lifecycle test plan

Run this checklist against the deployed custom-domain environment before labeling the application Alpha. Use two normal accounts and one commissioner account. Record screenshots or notes for every failure.

## Environment and data

- [ ] Frontend custom domain loads over HTTPS.
- [ ] API custom domain `/health` reports `status=ok` and `database=ok`.
- [ ] Player page shows catalog count, team count, season, and last-sync time.
- [ ] Player browsing loads at least two pages with **Load more players** and does not duplicate cards.
- [ ] Player search works by name, school, position, and conference.
- [ ] Home schedule shows cache freshness and groups games by week and kickoff time.
- [ ] Week navigation, kickoff-group navigation, pause/resume, and reduced-motion behavior work.

## Account and email lifecycle

- [ ] Signup sends a verification message from the configured sender domain.
- [ ] Verification link opens the custom frontend domain and activates the account.
- [ ] Resend-verification sends one new usable link.
- [ ] Password-reset email arrives and the reset link works once.
- [ ] Login, session validation, logout, and expired-session handling work.

## League lifecycle

- [ ] Commissioner creates a league and saves settings.
- [ ] Two managers join or are invited and approved.
- [ ] Mobile league-section selector reaches every section.
- [ ] Team names, roster rules, scoring rules, waiver rules, and trade rules persist after refresh.
- [ ] Draft lobby can be opened and remains locked for unauthorized users.

## Draft lifecycle

- [ ] Draft order can be randomized/reset before the first pick.
- [ ] Player queue persists after refresh and across signed-in devices.
- [ ] Draft clock and sticky mobile on-clock bar update together.
- [ ] Snake order reverses correctly each round.
- [ ] Auto-pick/timeout behavior follows configured rules.
- [ ] Undo last pick and reset draft are commissioner-only.
- [ ] A complete draft finishes without direct database intervention.

## In-season lifecycle

- [ ] Roster slot changes persist.
- [ ] Add/drop works in free-agency mode.
- [ ] Waiver submit, cancel, priority, processing, and rejection states work.
- [ ] Trade propose, accept, reject, cancel, expiration, and approval states work.
- [ ] Season schedule generation creates expected weekly matchups.
- [ ] Score week and finalize week update matchup results and standings.
- [ ] One complete scoring week finishes without manual database changes.

## Responsive and accessibility pass

Test at 390×844, 768×1024, 1366×768, and 1920×1080.

- [ ] No horizontal page overflow.
- [ ] All controls have visible focus and at least 44px touch targets on mobile.
- [ ] Secondary draft cards can expand/collapse on mobile.
- [ ] League and draft primary actions remain visible and understandable.
- [ ] Loading, empty, stale-data, offline, unauthorized, and server-error states are readable.
- [ ] Browser console has no uncaught errors.
