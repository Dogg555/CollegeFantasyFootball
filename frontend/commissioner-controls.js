(() => {
  'use strict';

  const PANEL_ID = 'commissioner-control-center';
  const PROTECTED_SETTING_IDS = [
    'settings-teams',
    'settings-scoring',
    'settings-draft-date',
    'settings-draft-time',
    'score-pass-yards',
    'score-pass-td',
    'score-interception',
    'score-rush-yards',
    'score-rush-td',
    'score-rec-yards',
    'score-rec-td',
    'score-reception',
    'score-fumble',
    'score-two-point',
    'rules-qb',
    'rules-rb',
    'rules-wr',
    'rules-te',
    'rules-flex',
    'rules-bench'
  ];

  let latestDashboard = null;
  let activeRequest = 0;
  let panelMessage = '';
  let panelMessageIsError = false;

  function escape(value) {
    if (typeof window.escapeHtml === 'function') return window.escapeHtml(String(value ?? ''));
    return String(value ?? '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#039;');
  }

  function currentLeague() {
    return window.getLeagueState?.() || null;
  }

  function currentLeagueId() {
    return String(currentLeague()?.id || '');
  }

  function isCommissionerSession() {
    const league = currentLeague();
    return Boolean(
      league?.id
      && window.getAuthState?.()?.token
      && !window.isLocalDemoSession?.()
      && window.isCurrentCommissioner?.(league)
    );
  }

  function operationKey(prefix = 'commissioner') {
    const random = window.crypto?.randomUUID?.()
      || `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    return `${prefix}-${random}`;
  }

  function memberBlockers(member = {}) {
    const blockers = [];
    if (Number(member.rosterPlayers || 0) > 0) blockers.push('rostered players');
    if (Number(member.draftPicks || 0) > 0) blockers.push('draft picks');
    if (Number(member.scheduledMatchups || 0) > 0) blockers.push('scheduled matchups');
    if (Number(member.openTrades || 0) > 0) blockers.push('open trades');
    if (Number(member.pendingWaivers || 0) > 0) blockers.push('pending waivers');
    return blockers;
  }

  function actionPlan(dashboard = {}, member = {}) {
    const status = String(member.status || '').toLowerCase();
    const actions = [];
    if (member.owner) return actions;

    if (status === 'pending' || status === 'invited') {
      const approvalBlocked = Boolean(dashboard.draftStarted)
        || Number(dashboard.activeManagers || 0) >= Number(dashboard.teamCount || 0);
      actions.push({
        action: 'approve',
        label: 'Approve',
        primary: true,
        disabled: approvalBlocked,
        reason: dashboard.draftStarted
          ? 'The draft has started.'
          : approvalBlocked ? 'The league is full.' : ''
      });
      actions.push({ action: 'reject', label: 'Reject', disabled: false, reason: '' });
      return actions;
    }

    if (status !== 'active') return actions;

    if (dashboard.actorIsOwner) {
      actions.push({
        action: member.role === 'commissioner' ? 'demote' : 'promote',
        label: member.role === 'commissioner' ? 'Make manager' : 'Make commissioner',
        disabled: false,
        reason: ''
      });
      actions.push({
        action: 'transfer',
        label: 'Transfer ownership',
        disabled: false,
        reason: ''
      });
    }

    const blockers = memberBlockers(member);
    actions.push({
      action: 'remove',
      label: 'Remove',
      disabled: Boolean(dashboard.draftStarted) || blockers.length > 0,
      reason: dashboard.draftStarted
        ? 'Managers cannot be removed after the draft starts.'
        : blockers.length ? `Resolve ${blockers.join(', ')} first.` : ''
    });
    return actions;
  }

  function hideLegacyMemberMutations() {
    const managerList = document.getElementById('manager-list');
    if (!managerList || !isCommissionerSession()) return;
    managerList
      .querySelectorAll('[data-member-activate], [data-member-role], [data-member-remove]')
      .forEach((button) => {
        button.hidden = true;
        button.disabled = true;
        button.setAttribute('aria-hidden', 'true');
      });
  }

  function applySettingsLock(dashboard) {
    const locked = Boolean(dashboard?.protectedSettingsLocked);
    PROTECTED_SETTING_IDS.forEach((id) => {
      const control = document.getElementById(id);
      if (!control) return;
      control.disabled = locked;
      control.dataset.cffCompetitionLocked = locked ? 'true' : 'false';
      control.title = locked ? String(dashboard.settingsLockReason || 'Locked after the draft starts.') : '';
    });

    const form = document.getElementById('league-settings-form');
    if (!form) return;
    let notice = document.getElementById('competition-settings-lock-notice');
    if (!locked) {
      notice?.remove();
      return;
    }
    if (!notice) {
      notice = document.createElement('div');
      notice.id = 'competition-settings-lock-notice';
      notice.className = 'notice notice--warning';
      form.prepend(notice);
    }
    notice.textContent = dashboard.settingsLockReason
      || 'Core competition settings are locked because the draft has started. League notes, waiver rules, and trade rules remain editable.';
  }

  function memberCounts(member) {
    const details = [];
    const roster = Number(member.rosterPlayers || 0);
    const waivers = Number(member.pendingWaivers || 0);
    const trades = Number(member.openTrades || 0);
    if (roster) details.push(`${roster} rostered`);
    if (waivers) details.push(`${waivers} pending waiver${waivers === 1 ? '' : 's'}`);
    if (trades) details.push(`${trades} open trade${trades === 1 ? '' : 's'}`);
    return details.length ? details.join(' / ') : 'No active transactions';
  }

  function actionButtons(dashboard, member) {
    return actionPlan(dashboard, member).map((item) => {
      const className = item.primary
        ? 'button button--primary'
        : item.action === 'remove' || item.action === 'reject'
          ? 'button button--ghost'
          : 'button';
      return `<button class="${className}" data-commissioner-action="${item.action}" data-member-email="${escape(member.email)}" type="button" ${item.disabled ? 'disabled' : ''} title="${escape(item.reason)}">${escape(item.label)}</button>`;
    }).join('');
  }

  function ensurePanel() {
    const managerList = document.getElementById('manager-list');
    if (!managerList) return null;
    let panel = document.getElementById(PANEL_ID);
    if (!panel) {
      panel = document.createElement('div');
      panel.id = PANEL_ID;
      panel.className = 'list';
      panel.setAttribute('aria-live', 'polite');
      managerList.insertAdjacentElement('beforebegin', panel);
      panel.addEventListener('submit', handleSubmit);
      panel.addEventListener('click', handleClick);
    }
    return panel;
  }

  function renderPanel(dashboard = latestDashboard) {
    const panel = ensurePanel();
    if (!panel) return;
    if (!isCommissionerSession()) {
      panel.remove();
      applySettingsLock(null);
      return;
    }
    if (!dashboard) {
      panel.innerHTML = '<div class="row"><div><strong>Commissioner controls</strong><div class="muted">Loading authoritative league controls...</div></div></div>';
      return;
    }

    const members = Array.isArray(dashboard.members) ? dashboard.members : [];
    const lockCopy = dashboard.protectedSettingsLocked
      ? 'Core settings locked after draft start'
      : 'Core settings editable before draft';
    const inviteDisabled = dashboard.draftStarted || Number(dashboard.openTeamSlots || 0) <= 0;
    const message = panelMessage
      ? `<div class="notice ${panelMessageIsError ? 'notice--warning' : ''}" role="status">${escape(panelMessage)}</div>`
      : '';

    panel.innerHTML = `
      <div class="row">
        <div>
          <strong>Commissioner control center</strong>
          <div class="muted">${Number(dashboard.activeManagers || 0)} active / ${Number(dashboard.teamCount || 0)} teams / ${Number(dashboard.openTeamSlots || 0)} open slots</div>
          <div class="muted small">Owner: ${escape(dashboard.ownerEmail)} / ${escape(lockCopy)}</div>
        </div>
        <span class="pill ${dashboard.protectedSettingsLocked ? '' : 'pill--muted'}">${dashboard.protectedSettingsLocked ? 'Competition locked' : 'Pre-draft'}</span>
      </div>
      ${message}
      <form id="commissioner-invite-form" class="form">
        <div class="field-group">
          <label class="field">
            <span>Invite manager by email</span>
            <input id="commissioner-invite-email" type="email" autocomplete="email" placeholder="manager@example.com" ${inviteDisabled ? 'disabled' : ''} />
          </label>
        </div>
        <div class="form__footer">
          <div class="muted small">${dashboard.draftStarted ? 'Invites close when the draft starts.' : inviteDisabled ? 'All configured manager spots are reserved.' : 'Invitations reserve a team slot until rejected or removed.'}</div>
          <button class="button button--primary" type="submit" ${inviteDisabled ? 'disabled' : ''}>Send invite</button>
        </div>
      </form>
      <div class="list" id="authoritative-manager-controls">
        ${members.map((member) => `
          <div class="row">
            <div>
              <strong>${escape(member.teamName || member.email)}</strong>
              <div class="muted">${escape(member.email)}</div>
              <div class="muted small">${member.owner ? 'Owner / ' : ''}${member.role === 'commissioner' ? 'Commissioner' : 'Manager'} / ${escape(member.status)} / ${escape(memberCounts(member))}</div>
            </div>
            <div class="actions">${actionButtons(dashboard, member)}</div>
          </div>
        `).join('')}
      </div>
    `;
    hideLegacyMemberMutations();
    applySettingsLock(dashboard);
  }

  async function loadDashboard() {
    const leagueId = currentLeagueId();
    if (!leagueId || !isCommissionerSession()) {
      latestDashboard = null;
      renderPanel(null);
      return null;
    }
    const requestId = ++activeRequest;
    renderPanel(latestDashboard);
    try {
      const payload = await window.apiRequest(`/leagues/${encodeURIComponent(leagueId)}/commissioner`);
      if (requestId !== activeRequest || leagueId !== currentLeagueId()) return null;
      latestDashboard = payload;
      panelMessage = '';
      panelMessageIsError = false;
      renderPanel(payload);
      return payload;
    } catch (error) {
      if (requestId !== activeRequest) return null;
      panelMessage = window.mutationErrorMessage?.(error, 'Commissioner controls are temporarily unavailable.')
        || error.message;
      panelMessageIsError = true;
      renderPanel(latestDashboard || {
        ownerEmail: currentLeague()?.commissionerEmail || '',
        activeManagers: 0,
        teamCount: Number(currentLeague()?.teams || 0),
        openTeamSlots: 0,
        members: []
      });
      return null;
    }
  }

  async function mutate(endpoint, body, successFallback) {
    const payload = await window.apiRequest(endpoint, {
      method: 'POST',
      headers: { 'Idempotency-Key': body.operationKey },
      body: JSON.stringify(body)
    });
    latestDashboard = payload;
    panelMessage = payload.message || successFallback;
    panelMessageIsError = false;
    try {
      await window.refreshLeagueFromApi?.();
      window.renderLeague?.();
    } catch {
      // The mutation response remains authoritative even when the broader refresh is unavailable.
    }
    renderPanel(payload);
    window.CFF_UI?.notify(panelMessage, 'success');
    return payload;
  }

  async function handleSubmit(event) {
    if (event.target?.id !== 'commissioner-invite-form') return;
    event.preventDefault();
    const emailInput = document.getElementById('commissioner-invite-email');
    const email = String(emailInput?.value || '').trim().toLowerCase();
    if (!email) {
      panelMessage = 'Enter a manager email.';
      panelMessageIsError = true;
      renderPanel();
      return;
    }
    const button = event.target.querySelector('button[type="submit"]');
    if (button) button.disabled = true;
    try {
      await mutate(
        `/leagues/${encodeURIComponent(currentLeagueId())}/commissioner/invitations`,
        { email, operationKey: operationKey('invite') },
        `Invited ${email}.`
      );
    } catch (error) {
      panelMessage = window.mutationErrorMessage?.(error, 'The invitation could not be saved.') || error.message;
      panelMessageIsError = true;
      renderPanel();
      window.CFF_UI?.notify(panelMessage, 'error');
    }
  }

  async function handleClick(event) {
    const button = event.target.closest?.('[data-commissioner-action]');
    if (!button || button.disabled) return;
    const action = button.dataset.commissionerAction;
    const email = button.dataset.memberEmail;
    if (!action || !email) return;

    if (action === 'remove' && !window.confirm(`Remove ${email} from this league? This is only allowed before competition data exists.`)) return;
    if (action === 'transfer' && !window.confirm(`Transfer league ownership to ${email}? Your account will become a regular manager.`)) return;

    button.disabled = true;
    try {
      await mutate(
        `/leagues/${encodeURIComponent(currentLeagueId())}/commissioner/members/${encodeURIComponent(email)}/${encodeURIComponent(action)}`,
        { operationKey: operationKey(action) },
        `${action} completed for ${email}.`
      );
    } catch (error) {
      const blockers = Array.isArray(error?.data?.blockers) && error.data.blockers.length
        ? ` Blockers: ${error.data.blockers.join(', ')}.`
        : '';
      panelMessage = `${window.mutationErrorMessage?.(error, 'The commissioner action failed.') || error.message}${blockers}`;
      panelMessageIsError = true;
      renderPanel();
      window.CFF_UI?.notify(panelMessage, 'error');
    }
  }

  function installRenderHook() {
    if (typeof window.renderLeague !== 'function' || window.renderLeague.__cffCommissionerControls) return false;
    const original = window.renderLeague;
    const wrapped = function renderLeagueWithCommissionerControls(...args) {
      const result = original.apply(this, args);
      window.setTimeout(() => {
        hideLegacyMemberMutations();
        loadDashboard();
      }, 0);
      return result;
    };
    wrapped.__cffCommissionerControls = true;
    window.renderLeague = wrapped;
    return true;
  }

  function initialize() {
    const managerList = document.getElementById('manager-list');
    if (!managerList) return;
    installRenderHook();
    const observer = new MutationObserver(() => hideLegacyMemberMutations());
    observer.observe(managerList, { childList: true, subtree: true });
    hideLegacyMemberMutations();
    loadDashboard();
  }

  window.CFF_COMMISSIONER_CONTROLS = Object.freeze({
    memberBlockers,
    actionPlan,
    loadDashboard
  });

  window.addEventListener('load', () => window.setTimeout(initialize, 0), { once: true });
})();
