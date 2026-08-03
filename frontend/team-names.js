(() => {
  'use strict';

  const originalJoinLeagueApi = window.joinLeagueApi;

  function normalizeTeamName(value) {
    return String(value || '').trim().replace(/\s+/g, ' ');
  }

  function teamNameError(error, fallback) {
    return window.mutationErrorMessage?.(error, fallback) || error?.message || fallback;
  }

  function currentMember(league = window.getLeagueState?.()) {
    const email = String(window.getAuthState?.()?.email || '').trim().toLowerCase();
    if (!email || !league) return null;
    return (league.members || []).find((member) => String(member.email || '').trim().toLowerCase() === email) || null;
  }

  async function saveOwnTeamNameApi(leagueId, rawTeamName) {
    const teamName = normalizeTeamName(rawTeamName);
    if (teamName.length < 2 || teamName.length > 40) {
      const error = new Error('Team names must be between 2 and 40 characters.');
      error.status = 400;
      error.data = { code: 'INVALID_TEAM_NAME', error: error.message };
      throw error;
    }
    return window.apiRequest(`/leagues/${encodeURIComponent(leagueId)}/team-name`, {
      method: 'PUT',
      body: JSON.stringify({ teamName })
    });
  }

  window.saveOwnTeamNameApi = saveOwnTeamNameApi;

  function joinTeamNameValue(explicitValue = '') {
    return normalizeTeamName(
      explicitValue
      || document.getElementById('join-league-team-name')?.value
      || document.getElementById('invite-team-name')?.value
      || ''
    );
  }

  if (typeof originalJoinLeagueApi === 'function') {
    window.joinLeagueApi = async function joinLeagueWithTeamName(code, explicitTeamName = '') {
      const teamName = joinTeamNameValue(explicitTeamName);
      const joined = await originalJoinLeagueApi(code);
      const leagueId = joined?.id || joined?.leagueId;
      if (!leagueId || !teamName) return joined;

      const savedName = await saveOwnTeamNameApi(leagueId, teamName);
      if (joined?.joinStatus === 'pending_approval') {
        return { ...joined, teamName: savedName.teamName };
      }

      await window.syncLeaguesFromApi?.();
      return window.getLeagueState?.() || { ...joined, teamName: savedName.teamName };
    };
  }

  function injectManualJoinTeamName() {
    const form = document.getElementById('join-league-code-form');
    if (!form || document.getElementById('join-league-team-name')) return;
    const codeField = document.getElementById('join-league-code')?.closest('.field');
    const label = document.createElement('label');
    label.className = 'field';
    label.innerHTML = `
      <span>Your team name</span>
      <input id="join-league-team-name" type="text" minlength="2" maxlength="40"
        autocomplete="off" placeholder="Saturday Legends" required />
    `;
    if (codeField) codeField.insertAdjacentElement('afterend', label);
    else form.prepend(label);
  }

  function injectInviteTeamName() {
    const flow = document.getElementById('invite-flow');
    if (!flow || document.getElementById('invite-team-name-field')) return;
    const cta = flow.querySelector('.cta-row');
    const label = document.createElement('label');
    label.id = 'invite-team-name-field';
    label.className = 'field';
    label.hidden = true;
    label.innerHTML = `
      <span>Your team name</span>
      <input id="invite-team-name" type="text" minlength="2" maxlength="40"
        autocomplete="off" placeholder="Saturday Legends" />
      <small class="muted">You can change this later from My Team.</small>
    `;
    if (cta) cta.insertAdjacentElement('beforebegin', label);
    else flow.append(label);
  }

  function refreshInviteTeamName() {
    const field = document.getElementById('invite-team-name-field');
    const input = document.getElementById('invite-team-name');
    if (!field || !input) return;
    const hasInvite = Boolean(new URLSearchParams(window.location.search).get('invite'));
    const signedIn = Boolean(window.getAuthState?.()?.token);
    field.hidden = !(hasInvite && signedIn);
    input.required = hasInvite && signedIn;
  }

  function injectSelfTeamNameCard() {
    if (document.getElementById('self-team-name-card')) return;
    const firstTeamPanel = document.querySelector('[data-league-panel="team"]');
    if (!firstTeamPanel) return;

    const card = document.createElement('section');
    card.id = 'self-team-name-card';
    card.className = 'card';
    card.dataset.leaguePanel = 'team';
    card.innerHTML = `
      <div class="card__header">
        <div>
          <h2>Your team name</h2>
          <div class="muted small">This name appears in standings, matchups, trades, and league activity.</div>
        </div>
        <span class="pill pill--muted">Manager controlled</span>
      </div>
      <form id="self-team-name-form" class="form">
        <label class="field">
          <span>Team name</span>
          <input id="self-team-name-input" type="text" minlength="2" maxlength="40"
            autocomplete="off" placeholder="Saturday Legends" required />
        </label>
        <div class="form__footer">
          <div id="self-team-name-status" class="muted small" role="status"></div>
          <button class="button button--primary" type="submit">Save team name</button>
        </div>
      </form>
    `;
    firstTeamPanel.insertAdjacentElement('beforebegin', card);

    const form = card.querySelector('#self-team-name-form');
    const input = card.querySelector('#self-team-name-input');
    const status = card.querySelector('#self-team-name-status');
    form.addEventListener('input', () => {
      form.dataset.dirty = 'true';
      status.textContent = '';
    });
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      const league = window.getLeagueState?.();
      if (!league?.id) {
        status.textContent = 'Select a league before saving a team name.';
        return;
      }
      const button = form.querySelector('button[type="submit"]');
      button.disabled = true;
      status.textContent = 'Saving team name…';
      try {
        const saved = await saveOwnTeamNameApi(league.id, input.value);
        form.dataset.dirty = '';
        input.value = saved.teamName;
        status.textContent = 'Team name saved.';
        await window.syncLeaguesFromApi?.();
        window.CFF_UI?.notify('Team name saved.', 'success');
        window.renderLeague?.();
      } catch (error) {
        status.textContent = teamNameError(error, 'Could not save the team name.');
      } finally {
        button.disabled = false;
      }
    });
  }

  function refreshSelfTeamName() {
    const card = document.getElementById('self-team-name-card');
    const form = document.getElementById('self-team-name-form');
    const input = document.getElementById('self-team-name-input');
    const button = form?.querySelector('button[type="submit"]');
    if (!card || !form || !input || !button) return;

    const league = window.getLeagueState?.();
    const member = currentMember(league);
    const canEdit = Boolean(league?.id && member && member.status !== 'Removed');
    form.hidden = !canEdit;
    button.disabled = !canEdit;
    if (!canEdit) {
      const status = document.getElementById('self-team-name-status');
      if (status) status.textContent = 'Join this league before choosing a team name.';
      return;
    }
    if (form.dataset.dirty !== 'true') {
      input.value = member.teamName || '';
    }
  }

  function wrapLeagueRender() {
    if (typeof window.renderLeague !== 'function' || window.renderLeague.__cffTeamNamesWrapped) return;
    const original = window.renderLeague;
    const wrapped = function renderLeagueWithTeamNames(...args) {
      const result = original.apply(this, args);
      queueMicrotask(() => {
        refreshInviteTeamName();
        refreshSelfTeamName();
      });
      return result;
    };
    wrapped.__cffTeamNamesWrapped = true;
    window.renderLeague = wrapped;
  }

  document.addEventListener('DOMContentLoaded', () => {
    injectManualJoinTeamName();
    injectInviteTeamName();
    injectSelfTeamNameCard();
    wrapLeagueRender();
    refreshInviteTeamName();
    refreshSelfTeamName();
  });
})();
