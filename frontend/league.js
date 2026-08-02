const emptyState = document.getElementById('league-empty');
const details = document.getElementById('league-details');
const leagueName = document.getElementById('league-name');
const leagueTeams = document.getElementById('league-teams');
const leagueScoring = document.getElementById('league-scoring');
const leagueDraft = document.getElementById('league-draft');
const leagueId = document.getElementById('league-id');
const leagueNotes = document.getElementById('league-notes');
const leagueQueueCount = document.getElementById('league-queue-count');
const leagueDraftDate = document.getElementById('league-draft-date');
const clearLeagueBtn = document.getElementById('clear-league');
const leagueList = document.getElementById('league-list');
const leagueCount = document.getElementById('league-count');
const createLeagueLink = document.getElementById('create-league-link');
const teamRoster = document.getElementById('team-roster');
const teamSlots = document.getElementById('team-slots');
const scoreboardList = document.getElementById('scoreboard-list');
const scoreboardWeek = document.getElementById('scoreboard-week');
const generateSeasonBtn = document.getElementById('generate-season');
const scoreWeekBtn = document.getElementById('score-week');
const finalizeWeekBtn = document.getElementById('finalize-week');
const scoreWeekStatus = document.getElementById('score-week-status');
const standingsList = document.getElementById('standings-list');
const freeAgentList = document.getElementById('free-agent-list');
const dropPlayerList = document.getElementById('drop-player-list');
const waiverForm = document.getElementById('waiver-form');
const waiverAddPlayer = document.getElementById('waiver-add-player');
const waiverDropPlayer = document.getElementById('waiver-drop-player');
const waiverStatus = document.getElementById('waiver-status');
const waiverList = document.getElementById('waiver-list');
const waiverPriorityList = document.getElementById('waiver-priority-list');
const resetWaiverPriorityBtn = document.getElementById('reset-waiver-priority');
const waiverSubmitButton = waiverForm?.querySelector('button[type="submit"]');
const tradeForm = document.getElementById('trade-form');
const tradeOfferPlayer = document.getElementById('trade-offer-player');
const tradeTargetManager = document.getElementById('trade-target-manager');
const tradeRequestPlayer = document.getElementById('trade-request-player');
const tradeRequestPlayerId = document.getElementById('trade-request-player-id');
const tradeNote = document.getElementById('trade-note');
const tradeStatus = document.getElementById('trade-status');
const tradeSubmitButton = tradeForm?.querySelector('button[type="submit"]');
const tradeList = document.getElementById('trade-list');
const transactionList = document.getElementById('transaction-list');
const commissionerSettings = document.getElementById('commissioner-settings');
const commissionerLocked = document.getElementById('commissioner-locked');
const settingsForm = document.getElementById('league-settings-form');
const settingsName = document.getElementById('settings-name');
const settingsDraftDate = document.getElementById('settings-draft-date');
const settingsScoring = document.getElementById('settings-scoring');
const settingsTeams = document.getElementById('settings-teams');
const settingsInvites = document.getElementById('settings-invites');
const settingsNotes = document.getElementById('settings-notes');
const settingsStatus = document.getElementById('settings-status');
const scorePassYards = document.getElementById('score-pass-yards');
const scorePassTd = document.getElementById('score-pass-td');
const scoreInterception = document.getElementById('score-interception');
const scoreRushYards = document.getElementById('score-rush-yards');
const scoreRushTd = document.getElementById('score-rush-td');
const scoreRecYards = document.getElementById('score-rec-yards');
const scoreRecTd = document.getElementById('score-rec-td');
const scoreReception = document.getElementById('score-reception');
const scoreFumble = document.getElementById('score-fumble');
const scoreTwoPoint = document.getElementById('score-two-point');
const rulesQb = document.getElementById('rules-qb');
const rulesRb = document.getElementById('rules-rb');
const rulesWr = document.getElementById('rules-wr');
const rulesTe = document.getElementById('rules-te');
const rulesFlex = document.getElementById('rules-flex');
const rulesBench = document.getElementById('rules-bench');
const waiverMode = document.getElementById('waiver-mode');
const waiverDeadline = document.getElementById('waiver-deadline');
const waiverFaLock = document.getElementById('waiver-fa-lock');
const tradeApproval = document.getElementById('trade-approval');
const tradeExpiration = document.getElementById('trade-expiration');
const managerList = document.getElementById('manager-list');
const managerCount = document.getElementById('manager-count');
const copyInviteLinkBtn = document.getElementById('copy-invite-link');
const inviteLinkStatus = document.getElementById('invite-link-status');
const stepInvites = document.getElementById('step-invites');
const stepRules = document.getElementById('step-rules');
const stepLobby = document.getElementById('step-lobby');
const draftLobbyStatus = document.getElementById('draft-lobby-status');
const draftLobbyLink = document.getElementById('draft-lobby-link');
const leagueTabs = document.querySelectorAll('[data-league-tab]');
const leaguePanels = document.querySelectorAll('[data-league-panel]');
let activeScoreboardWeek = 1;

function renderLeague() {
  updateSharedNav('league');
  const authState = getAuthState();
  const leagueState = getLeagueState();
  const queue = getQueue();
  renderLeagueList();
  renderCommissionerAccess(isCurrentCommissioner(leagueState));

  if (!leagueState) {
    if (emptyState) emptyState.hidden = false;
    if (details) details.hidden = true;
    if (clearLeagueBtn) clearLeagueBtn.hidden = true;
    renderLobbyStatus(null);
    return;
  }

  if (emptyState) emptyState.hidden = true;
  if (details) details.hidden = false;
  if (clearLeagueBtn) clearLeagueBtn.hidden = false;

  const scoring = leagueState.scoringLabel || leagueState.scoring || 'PPR';
  const draft = leagueState.draftTypeLabel || leagueState.draftType || 'Snake';

  leagueName.textContent = leagueState.name || 'League';
  leagueTeams.textContent = leagueState.teams ? `${leagueState.teams} teams` : 'Teams TBD';
  leagueScoring.textContent = `${scoring} (${scoringSummary(leagueState.scoringSettings)})`;
  leagueDraft.textContent = draft;
  leagueId.textContent = leagueState.id ? `ID: ${leagueState.id}` : 'ID not assigned';
  leagueNotes.textContent = leagueState.notes || 'No notes yet.';
  if (leagueQueueCount) {
    leagueQueueCount.textContent = `${queue.length} player${queue.length === 1 ? '' : 's'} queued for draft day.`;
  }
  if (leagueDraftDate) {
    leagueDraftDate.textContent = leagueState.draftDate
      ? new Date(leagueState.draftDate).toLocaleString()
      : 'Not scheduled yet.';
  }
  populateSettings(leagueState);
  renderManagers(leagueState);
  renderLobbyStatus(leagueState);
  renderTeamPanel(leagueState);
  renderScoreboard(leagueState);
  renderStandings(leagueState);
  renderFreeAgency();
  renderWaivers();
  renderWaiverPriority(leagueState);
  renderTrades(leagueState);
  renderTransactions();
}

function renderLeagueList() {
  const leagues = getLeaguesForCurrentAccount();
  const active = getLeagueState();
  if (leagueCount) {
    leagueCount.textContent = `${leagues.length} / ${MAX_LEAGUES_PER_ACCOUNT}`;
  }
  if (createLeagueLink) {
    createLeagueLink.classList.toggle('is-disabled', leagues.length >= MAX_LEAGUES_PER_ACCOUNT);
    createLeagueLink.textContent = leagues.length >= MAX_LEAGUES_PER_ACCOUNT ? 'League limit reached' : 'Create league';
    createLeagueLink.href = leagues.length >= MAX_LEAGUES_PER_ACCOUNT ? 'league.html' : 'index.html';
  }
  if (!leagueList) return;
  if (!leagues.length) {
    leagueList.textContent = 'No leagues yet.';
    return;
  }
  leagueList.innerHTML = leagues.map((league) => `
    <div class="row">
      <div>
        <strong>${league.name}</strong>
        <div class="muted">${league.teams} teams / ${league.scoringLabel || league.scoring} / ${league.draftTypeLabel || league.draftType}</div>
        <div class="muted small">${league.id === active?.id ? 'Active league' : 'Saved league'}</div>
      </div>
      <div class="actions">
        <button class="button ${league.id === active?.id ? 'button--primary' : ''}" data-switch-league="${league.id}" type="button">
          ${league.id === active?.id ? 'Active' : 'Switch'}
        </button>
        <button class="button button--ghost" data-remove-league="${league.id}" type="button">Remove</button>
      </div>
    </div>
  `).join('');
  leagueList.querySelectorAll('[data-switch-league]').forEach((button) => {
    button.addEventListener('click', async () => {
      setActiveLeague(button.dataset.switchLeague);
      await refreshLeagueFromApi();
      renderLeague();
    });
  });
  leagueList.querySelectorAll('[data-remove-league]').forEach((button) => {
    button.addEventListener('click', async () => {
      if (!requireCommissioner()) return;
      try {
        await removeLeagueFromApi(button.dataset.removeLeague);
      } catch {
        removeLeagueForCurrentAccount(button.dataset.removeLeague);
      }
      renderLeague();
    });
  });
}

function setActiveLeagueTab(tabName = 'overview') {
  const safeTab = ['overview', 'team', 'scoreboard', 'standings', 'freeagency', 'waivers', 'trades', 'settings', 'managers', 'activity'].includes(tabName) ? tabName : 'overview';
  leagueTabs.forEach((tab) => {
    tab.classList.toggle('is-active', tab.dataset.leagueTab === safeTab);
  });
  leaguePanels.forEach((panel) => {
    panel.hidden = panel.dataset.leaguePanel !== safeTab;
  });
  if (window.location.hash !== `#${safeTab}` && safeTab !== 'overview') {
    window.history.replaceState({}, document.title, `#${safeTab}`);
  }
  if (safeTab === 'overview' && window.location.hash) {
    window.history.replaceState({}, document.title, window.location.pathname);
  }
  renderCommissionerAccess(isCurrentCommissioner(getLeagueState()));
}

function renderFreeAgency() {
  const available = getAvailablePlayers();
  const leagueState = getLeagueState();
  const hasRoom = rosterHasRoom(leagueState);
  const waiverLocked = freeAgencyLocked(leagueState);
  const lineupIsLocked = lineupLocked();
  if (freeAgentList) {
    if (!available.length) {
      freeAgentList.textContent = 'No free agents available.';
    } else {
      const lockMessage = lineupIsLocked
        ? ' / lineups locked after finalized matchups'
        : waiverLocked
          ? ' / free agency locked; submit waiver claims'
          : '';
      const capacityRow = `<div class="row"><div><strong>Roster capacity</strong><div class="muted">${getRoster().length} / ${rosterLimit(leagueState)} spots filled${hasRoom ? '' : ' / submit a waiver with a drop to add players'}${lockMessage}</div></div></div>`;
      freeAgentList.innerHTML = available.map((player) => `
        <div class="row">
          <div>
            <strong>${player.name}</strong>
            <div class="muted">${player.team} ${player.position} / ${player.availability || 'Free Agent'} / ${Number(player.projection).toFixed(1)} proj</div>
          </div>
          <button class="button button--primary" data-add-free-agent="${player.id}" type="button" ${hasRoom && !waiverLocked && !lineupIsLocked ? '' : 'disabled'}>${lineupIsLocked ? 'Locked' : waiverLocked ? 'Waivers' : hasRoom ? 'Add' : 'Roster full'}</button>
        </div>
      `).join('');
      freeAgentList.innerHTML = capacityRow + freeAgentList.innerHTML;
      freeAgentList.querySelectorAll('[data-add-free-agent]').forEach((button) => {
        button.addEventListener('click', async () => {
          const player = samplePlayers.find((item) => item.id === button.dataset.addFreeAgent);
          if (player) {
            try {
              await addFreeAgentApi(player);
            } catch {
              if (!addFreeAgent(player) && waiverStatus) {
                waiverStatus.textContent = 'Roster is full or player is unavailable. Use waivers with a drop.';
              }
            }
          }
          renderLeague();
        });
      });
    }
  }
  if (dropPlayerList) {
    const roster = getRoster();
    if (!roster.length) {
      dropPlayerList.textContent = 'No players on roster.';
    } else {
      dropPlayerList.innerHTML = roster.map((player) => {
        const tradeLocked = playerLockedInTrade(player.id);
        return `
        <div class="row">
          <div>
            <strong>${player.name}</strong>
            <div class="muted">${player.team} ${player.position}${lineupIsLocked ? ' / lineup locked' : tradeLocked ? ' / locked in open trade' : ''}</div>
          </div>
          <button class="button button--ghost" data-drop-player="${player.id}" type="button" ${lineupIsLocked || tradeLocked ? 'disabled' : ''}>Drop</button>
        </div>
      `;
      }).join('');
      dropPlayerList.querySelectorAll('[data-drop-player]').forEach((button) => {
        button.addEventListener('click', async () => {
          try {
            await dropPlayerApi(button.dataset.dropPlayer);
          } catch {
            dropPlayer(button.dataset.dropPlayer);
          }
          renderLeague();
        });
      });
    }
  }
}

function renderWaivers() {
  const available = getAvailablePlayers();
  const roster = getRoster();
  const deadlinePassed = waiverDeadlinePassed();
  const rules = waiverRules();
  const waiverIsLocked = lineupLocked();
  if (waiverAddPlayer) {
    waiverAddPlayer.innerHTML = available.map((player) => `<option value="${player.id}">${player.name} (${player.position})</option>`).join('');
    waiverAddPlayer.disabled = waiverIsLocked || !available.length;
  }
  if (waiverDropPlayer) {
    waiverDropPlayer.innerHTML = `<option value="">No drop</option>${roster.map((player) => `<option value="${player.id}">${player.name}</option>`).join('')}`;
    waiverDropPlayer.disabled = waiverIsLocked;
  }
  if (waiverSubmitButton) {
    waiverSubmitButton.disabled = waiverIsLocked || !available.length;
  }
  if (waiverStatus && waiverIsLocked) {
    waiverStatus.textContent = 'Waivers are locked after finalized matchups.';
  }
  const claims = getWaiverClaims();
  if (!waiverList) return;
  if (!claims.length) {
    waiverList.textContent = 'No waiver claims submitted.';
    return;
  }
  const authEmail = getAuthState()?.email || '';
  const orderedClaims = claims.slice().sort((a, b) => (
    Number(a.priority || 999) - Number(b.priority || 999)
    || Number(a.claimOrder || 999) - Number(b.claimOrder || 999)
    || new Date(a.createdAt).getTime() - new Date(b.createdAt).getTime()
  ));
  const myPendingIds = orderedClaims
    .filter((claim) => claim.status === 'Pending' && (!claim.managerEmail || claim.managerEmail === authEmail))
    .map((claim) => claim.id);
  const processAll = isCurrentCommissioner()
    ? `<div class="row"><div><strong>Pending waiver run</strong><div class="muted">${waiverIsLocked ? 'Waivers are locked after finalized matchups.' : deadlinePassed ? 'Process claims by priority, claim order, then submitted time.' : `Claims process after ${new Date(rules.claimDeadline).toLocaleString()}.`}</div></div><button class="button button--primary" data-process-all-waivers type="button" ${deadlinePassed && !waiverIsLocked ? '' : 'disabled'}>Process all</button></div>`
    : '';
  waiverList.innerHTML = processAll + orderedClaims.map((claim) => {
    const mine = !claim.managerEmail || claim.managerEmail === authEmail;
    const pending = claim.status === 'Pending';
    const myIndex = myPendingIds.indexOf(claim.id);
    return `
    <div class="row">
      <div>
        <strong>${claim.addPlayer.name}</strong>
        <div class="muted">${claim.status} / Priority ${claim.priority || '--'} / Claim ${claim.claimOrder || '--'} / ${escapeHtml(managerDisplayName(claim.managerEmail || authEmail, leagueState))} / ${new Date(claim.createdAt).toLocaleString()}</div>
      </div>
      <div class="actions">
        ${pending && mine ? `<button class="button button--ghost" data-waiver-up="${claim.id}" type="button" ${myIndex > 0 ? '' : 'disabled'}>Up</button>` : ''}
        ${pending && mine ? `<button class="button button--ghost" data-waiver-down="${claim.id}" type="button" ${myIndex >= 0 && myIndex < myPendingIds.length - 1 ? '' : 'disabled'}>Down</button>` : ''}
        ${pending && (mine || isCurrentCommissioner()) ? `<button class="button button--ghost" data-cancel-waiver="${claim.id}" type="button">Cancel</button>` : ''}
        ${pending && isCurrentCommissioner() ? `<button class="button" data-process-waiver="${claim.id}" type="button" ${deadlinePassed && !waiverIsLocked ? '' : 'disabled'}>Process</button>` : !pending ? `<span class="badge">Done</span>` : ''}
      </div>
    </div>
  `;
  }).join('');
  waiverList.querySelector('[data-process-all-waivers]')?.addEventListener('click', async () => {
    try {
      const result = await processWaiversApi();
      if (waiverStatus) waiverStatus.textContent = `Processed ${result?.processed?.length || 0} claim(s).`;
    } catch {
      const result = processAllWaiverClaims();
      if (waiverStatus) waiverStatus.textContent = waiverDeadlinePassed()
        ? `Processed ${result?.processed?.length || 0} local waiver claim(s).`
        : 'Waiver deadline has not passed yet.';
    }
    renderLeague();
  });
  waiverList.querySelectorAll('[data-process-waiver]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await processWaiverClaimApi(button.dataset.processWaiver);
      } catch {
        const processed = processWaiverClaim(button.dataset.processWaiver);
        if (waiverStatus && !processed) waiverStatus.textContent = 'Waiver deadline has not passed yet.';
      }
      renderLeague();
    });
  });
  waiverList.querySelectorAll('[data-cancel-waiver]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await cancelWaiverClaimApi(button.dataset.cancelWaiver);
        if (waiverStatus) waiverStatus.textContent = 'Waiver claim cancelled.';
      } catch {
        if (!cancelWaiverClaim(button.dataset.cancelWaiver) && waiverStatus) {
          waiverStatus.textContent = 'Could not cancel waiver claim.';
        }
      }
      renderLeague();
    });
  });
  const moveClaim = async (claimId, direction) => {
    const ids = myPendingIds.slice();
    const index = ids.indexOf(claimId);
    const nextIndex = index + direction;
    if (index < 0 || nextIndex < 0 || nextIndex >= ids.length) return;
    [ids[index], ids[nextIndex]] = [ids[nextIndex], ids[index]];
    try {
      await reorderWaiverClaimsApi(ids);
      if (waiverStatus) waiverStatus.textContent = 'Waiver claim order updated.';
    } catch {
      reorderWaiverClaims(ids);
      if (waiverStatus) waiverStatus.textContent = 'Waiver claim order saved locally.';
    }
    renderLeague();
  };
  waiverList.querySelectorAll('[data-waiver-up]').forEach((button) => {
    button.addEventListener('click', () => moveClaim(button.dataset.waiverUp, -1));
  });
  waiverList.querySelectorAll('[data-waiver-down]').forEach((button) => {
    button.addEventListener('click', () => moveClaim(button.dataset.waiverDown, 1));
  });
}

function renderTrades(leagueState) {
  const roster = getRoster();
  const canManage = isCurrentCommissioner(leagueState);
  const lineupIsLocked = lineupLocked();
  const managers = (leagueState?.members || [])
    .filter((member) => isActiveTradeTarget(member.email, leagueState))
    .map((member) => ({ email: member.email, label: managerDisplayName(member.email, leagueState) }));
  if (tradeOfferPlayer) {
    const unlocked = roster.filter((player) => !playerLockedInTrade(player.id));
    tradeOfferPlayer.innerHTML = unlocked.length
      ? unlocked.map((player) => `<option value="${player.id}">${player.name} (${player.position})</option>`).join('')
      : '<option value="">No tradeable players</option>';
    tradeOfferPlayer.disabled = lineupIsLocked || !unlocked.length;
  }
  if (tradeTargetManager) {
    tradeTargetManager.innerHTML = managers.length
      ? managers.map((manager) => `<option value="${escapeHtml(manager.email)}">${escapeHtml(manager.label)}</option>`).join('')
      : '<option value="">Invite another manager first</option>';
    tradeTargetManager.disabled = lineupIsLocked || !managers.length;
  }
  if (tradeSubmitButton) {
    tradeSubmitButton.disabled = lineupIsLocked || !managers.length || !roster.some((player) => !playerLockedInTrade(player.id));
  }
  if (tradeStatus && lineupIsLocked) {
    tradeStatus.textContent = 'Trades are locked after finalized matchups.';
  } else if (tradeStatus && !managers.length) {
    tradeStatus.textContent = 'Invite and confirm another manager before sending trade offers.';
  }
  if (tradeRequestPlayerId) {
    renderRequestedTradePlayers();
  }
  const offers = getTradeOffers();
  if (!tradeList) return;
  if (!offers.length) {
    tradeList.textContent = 'No trade offers yet.';
    return;
  }
  tradeList.innerHTML = offers.map((offer) => {
    const open = isOpenTradeStatus(offer.status);
    const needsApproval = offer.status === 'Accepted' && offer.requiresApproval;
    const expires = offer.expiresAt ? ` / Expires ${new Date(offer.expiresAt).toLocaleString()}` : '';
    const mine = offer.offeredByEmail === getAuthState()?.email;
    const target = offer.offeredToEmail === getAuthState()?.email || canManage;
    const note = offer.note ? `<div class="muted small">${escapeHtml(offer.note)}</div>` : '';
    return `
    <div class="row">
      <div>
        <strong>${escapeHtml(offer.offerPlayer.name)} for ${escapeHtml(offer.requestPlayer?.name || offer.requestPlayerName || 'requested return')}</strong>
        <div class="muted">${escapeHtml(managerDisplayName(offer.offeredToEmail || offer.targetManager, leagueState))} / ${offer.status}${offer.requiresApproval ? ' / Approval required' : ''}${expires}</div>
        ${note}
      </div>
      <div class="actions">
        ${open && target && offer.status === 'Pending' ? `<button class="button" data-trade-accept="${offer.id}" type="button" ${lineupIsLocked ? 'disabled' : ''}>Accept</button>` : ''}
        ${open && target ? `<button class="button button--ghost" data-trade-decline="${offer.id}" type="button">Decline</button>` : ''}
        ${open && mine ? `<button class="button button--ghost" data-trade-cancel="${offer.id}" type="button">Cancel</button>` : ''}
        ${needsApproval && canManage ? `<button class="button button--primary" data-trade-approve="${offer.id}" type="button" ${lineupIsLocked ? 'disabled' : ''}>Approve</button><button class="button button--ghost" data-trade-veto="${offer.id}" type="button">Veto</button>` : ''}
      </div>
    </div>
  `;
  }).join('');
  tradeList.querySelectorAll('[data-trade-accept]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeAccept, 'Accepted');
      } catch {
        updateTradeStatus(button.dataset.tradeAccept, 'Accepted');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-decline]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeDecline, 'Declined');
      } catch {
        updateTradeStatus(button.dataset.tradeDecline, 'Declined');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-cancel]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeCancel, 'Cancelled');
      } catch {
        updateTradeStatus(button.dataset.tradeCancel, 'Cancelled');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-approve]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeApprove, 'Approved');
      } catch {
        updateTradeStatus(button.dataset.tradeApprove, 'Approved');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-veto]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeVeto, 'Vetoed');
      } catch {
        updateTradeStatus(button.dataset.tradeVeto, 'Vetoed');
      }
      renderLeague();
    });
  });
}

async function renderRequestedTradePlayers() {
  if (!tradeRequestPlayerId) return;
  const target = tradeTargetManager?.value || '';
  if (!target) {
    tradeRequestPlayerId.innerHTML = '<option value="">No target manager selected</option>';
    tradeRequestPlayerId.disabled = true;
    return;
  }
  tradeRequestPlayerId.disabled = false;
  tradeRequestPlayerId.innerHTML = '<option value="">Loading roster...</option>';
  try {
    const roster = await getManagerRosterApi(target);
    const available = roster.length ? roster : samplePlayers.filter((player) => !getRoster().some((item) => item.id === player.id));
    tradeRequestPlayerId.innerHTML = available
      .map((player) => `<option value="${player.id}">${player.name} (${player.position})</option>`)
      .join('');
  } catch {
    const fallback = samplePlayers.filter((player) => !getRoster().some((item) => item.id === player.id));
    tradeRequestPlayerId.innerHTML = fallback
      .map((player) => `<option value="${player.id}">${player.name} (${player.position})</option>`)
      .join('');
  }
}

function renderTransactions() {
  if (!transactionList) return;
  const transactions = getTransactions();
  if (!transactions.length) {
    transactionList.textContent = 'No league activity yet.';
    return;
  }
  transactionList.innerHTML = transactions.map((txn) => `
    <div class="row">
      <div>
        <strong>${escapeHtml(txn.type)}</strong>
        <div class="muted">${escapeHtml(txn.summary)}</div>
        ${txn.managerEmail ? `<div class="muted small">${escapeHtml(managerDisplayName(txn.managerEmail))}</div>` : ''}
      </div>
      <div class="label">${new Date(txn.createdAt).toLocaleString()}</div>
    </div>
  `).join('');
}

function renderTeamPanel(leagueState) {
  const roster = getRoster();
  const errors = lineupErrors(roster, leagueState);
  const locked = lineupLocked();
  if (teamRoster) {
    if (!roster.length) {
      teamRoster.textContent = 'No players drafted yet.';
    } else {
      teamRoster.innerHTML = roster.map((player, index) => {
        const currentSlot = String(player.rosterSlot || player.position || 'bench').toLowerCase();
        const slots = Array.from(new Set([currentSlot, ...legalSlotsForPlayer(player, leagueState)]));
        const slotOptions = slots.map((slot) => `
          <option value="${slot}" ${slot === currentSlot ? 'selected' : ''} ${slot !== currentSlot && !canMoveToSlot(player.id, slot, roster, leagueState) ? 'disabled' : ''}>
            ${slot.toUpperCase()}
          </option>
        `).join('');
        return `
        <div class="row">
          <div>
            <strong>${currentSlot.toUpperCase()} - ${player.name}</strong>
            <div class="muted">${player.team} / ${player.conference} / ${player.position} / Pick ${index + 1}</div>
          </div>
          <div class="actions">
            <select class="lineup-select" data-roster-slot="${player.id}" aria-label="Roster slot for ${player.name}">
              ${slotOptions}
            </select>
            <span class="badge">${Number(player.projection).toFixed(1)}</span>
          </div>
        </div>
      `;
      }).join('');
      teamRoster.querySelectorAll('[data-roster-slot]').forEach((select) => {
        select.disabled = locked;
        select.addEventListener('change', async () => {
          const previousPlayer = roster.find((player) => player.id === select.dataset.rosterSlot);
          const previousSlot = String(previousPlayer?.rosterSlot || previousPlayer?.position || 'bench').toLowerCase();
          if (lineupLocked()) {
            select.value = previousSlot;
            return;
          }
          select.disabled = true;
          try {
            const updated = await updateRosterSlotApi(select.dataset.rosterSlot, select.value);
            if (!updated) {
              select.value = previousSlot;
            }
          } catch (error) {
            console.error(error);
            select.value = previousSlot;
          } finally {
            select.disabled = false;
            renderLeague();
          }
        });
      });
    }
  }
  if (teamSlots) {
    const rules = { ...defaultRosterRules, ...(leagueState?.rosterRules || {}) };
    const counts = roster.reduce((acc, player) => {
      const slot = String(player.rosterSlot || player.position || 'bench').toLowerCase();
      acc[slot] = (acc[slot] || 0) + 1;
      return acc;
    }, {});
    const status = errors.length
      ? `<div class="row grid-full"><div><strong>Lineup incomplete</strong><div class="muted">${errors.map((error) => error.message).join(' / ')}</div></div><span class="badge">Fix</span></div>`
      : locked
        ? `<div class="row grid-full"><div><strong>Lineup locked</strong><div class="muted">A matchup has been finalized, so roster slots are locked.</div></div><span class="badge">Locked</span></div>`
      : `<div class="row grid-full"><div><strong>Lineup ready</strong><div class="muted">All required starter slots are filled.</div></div><span class="badge">Ready</span></div>`;
    teamSlots.innerHTML = status + ['qb', 'rb', 'wr', 'te', 'flex', 'bench'].map((slot) => `
      <div>
        <div class="label">${slot.toUpperCase()}</div>
        <div class="value">${counts[slot] || 0} / ${rules[slot]}</div>
      </div>
    `).join('');
  }
}

function renderScoreboard(leagueState) {
  if (!scoreboardList) return;
  const allMatchups = getMatchups().length ? getMatchups() : generateLocalSeasonSchedule(leagueState);
  const weeks = Array.from(new Set(allMatchups.map((matchup) => Number(matchup.week || 1)))).sort((a, b) => a - b);
  if (!weeks.includes(activeScoreboardWeek)) {
    activeScoreboardWeek = weeks[0] || 1;
  }
  if (scoreboardWeek) {
    scoreboardWeek.innerHTML = (weeks.length ? weeks : [1]).map((week) => `
      <option value="${week}" ${week === activeScoreboardWeek ? 'selected' : ''}>Week ${week}</option>
    `).join('');
  }
  const matchups = allMatchups.filter((matchup) => Number(matchup.week || 1) === activeScoreboardWeek);
  if (!allMatchups.length) {
    scoreboardList.textContent = 'Invite managers to generate matchups.';
    return;
  }
  scoreboardList.innerHTML = matchups.map((matchup) => `
    <div class="row">
      <div>
        <strong>${escapeHtml(managerDisplayName(matchup.homeManager, leagueState))} vs ${escapeHtml(managerDisplayName(matchup.awayManager, leagueState))}</strong>
        <div class="muted">Week ${matchup.week || activeScoreboardWeek} / ${matchup.status === 'final' ? 'Final' : 'Projected matchup'}</div>
      </div>
      <div class="score">${Number(matchup.homeScore).toFixed(1)} - ${Number(matchup.awayScore).toFixed(1)}</div>
    </div>
  `).join('');
}

function renderStandings(leagueState) {
  if (!standingsList) return;
  const standings = standingsFromMatchups(leagueState);
  if (!standings.length) {
    standingsList.textContent = 'Invite managers to build standings.';
    return;
  }
  standingsList.innerHTML = standings.map((manager, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${escapeHtml(manager.teamName || manager.email)}</strong>
        <div class="muted">${escapeHtml(manager.email)} / ${manager.role === 'commissioner' ? 'Commissioner' : 'Manager'} / GP ${manager.gamesPlayed}</div>
        <div class="muted small">PF ${Number(manager.pointsFor).toFixed(1)} / PA ${Number(manager.pointsAgainst).toFixed(1)} / PCT ${Number(manager.winPct || 0).toFixed(3)}</div>
      </div>
      <span class="badge">${manager.wins}-${manager.losses}${manager.ties ? `-${manager.ties}` : ''}</span>
    </div>
  `).join('');
}

function renderCommissionerAccess(canManage) {
  const activeTab = currentLeagueTab();
  const isSignedIn = Boolean(getAuthState());
  if (commissionerSettings) commissionerSettings.hidden = !canManage || activeTab !== 'settings';
  if (commissionerLocked) {
    commissionerLocked.hidden = canManage || activeTab !== 'settings';
    const copy = commissionerLocked.querySelector('p');
    if (copy) {
      copy.textContent = isSignedIn
        ? 'Only league commissioners can edit settings, invite managers, change roster rules, or open the draft lobby.'
        : 'Sign in as the commissioner to edit league settings, invite managers, change roster rules, or open the draft lobby.';
    }
  }
  [stepInvites, stepRules, stepLobby, copyInviteLinkBtn, clearLeagueBtn].forEach((control) => {
    if (control) control.disabled = !canManage;
  });
  if (resetWaiverPriorityBtn) resetWaiverPriorityBtn.disabled = !canManage;
  const weekFinal = getMatchups().some((matchup) => Number(matchup.week || 1) === activeScoreboardWeek && matchup.status === 'final');
  const anyFinal = getMatchups().some((matchup) => matchup.status === 'final');
  const validLineup = lineupValid();
  if (scoreWeekBtn) scoreWeekBtn.disabled = !canManage || weekFinal || !validLineup;
  if (finalizeWeekBtn) finalizeWeekBtn.disabled = !canManage || weekFinal || !getMatchups().length || !validLineup;
  if (generateSeasonBtn) generateSeasonBtn.disabled = !canManage || anyFinal;
}

function renderWaiverPriority(leagueState = getLeagueState()) {
  if (!waiverPriorityList) return;
  const priority = getWaiverPriority();
  if (!priority.length) {
    waiverPriorityList.textContent = 'Invite managers to build waiver priority.';
    return;
  }
  const pendingByManager = getWaiverClaims().reduce((counts, claim) => {
    if (claim.status === 'Pending') {
      const email = claim.managerEmail || getAuthState()?.email || '';
      counts[email] = (counts[email] || 0) + 1;
    }
    return counts;
  }, {});
  waiverPriorityList.innerHTML = priority
    .slice()
    .sort((a, b) => Number(a.priority || 999) - Number(b.priority || 999))
    .map((item) => {
      const email = item.managerEmail || item.email || 'Manager';
      const active = email === getAuthState()?.email;
      return `
        <div class="row">
          <div>
            <strong>${Number(item.priority || 0)}. ${escapeHtml(managerDisplayName(email, leagueState))}</strong>
            <div class="muted">${item.role === 'commissioner' ? 'Commissioner' : 'Manager'} / ${item.status || 'Active'} / ${pendingByManager[email] || 0} pending claim${pendingByManager[email] === 1 ? '' : 's'}</div>
          </div>
          <span class="badge">${active ? 'You' : leagueState?.commissionerEmail === email ? 'Commish' : 'Priority'}</span>
        </div>
      `;
    }).join('');
}

function currentLeagueTab() {
  return Array.from(leagueTabs).find((tab) => tab.classList.contains('is-active'))?.dataset.leagueTab || 'overview';
}

function populateSettings(leagueState) {
  if (!settingsForm || !leagueState) return;
  settingsName.value = leagueState.name || '';
  settingsDraftDate.value = leagueState.draftDate || '';
  settingsScoring.value = leagueState.scoring || 'ppr';
  settingsTeams.value = String(leagueState.teams || 10);
  settingsInvites.value = (leagueState.invitedEmails || []).join(', ');
  settingsNotes.value = leagueState.notes || '';
  populateScoringSettings(leagueState.scoringSettings);
  const rules = { ...defaultRosterRules, ...(leagueState.rosterRules || {}) };
  rulesQb.value = rules.qb;
  rulesRb.value = rules.rb;
  rulesWr.value = rules.wr;
  rulesTe.value = rules.te;
  rulesFlex.value = rules.flex;
  rulesBench.value = rules.bench;
  const waiver = waiverRules(leagueState);
  waiverMode.value = waiver.mode;
  waiverDeadline.value = waiver.claimDeadline || '';
  waiverFaLock.value = String(Boolean(waiver.freeAgencyLocked));
  const trade = tradeRules(leagueState);
  tradeApproval.value = String(Boolean(trade.commissionerApproval));
  tradeExpiration.value = String(trade.expirationHours || 48);
}

function populateScoringSettings(settings = scoringPresets.ppr) {
  scorePassYards.value = settings.passingYardsPerPoint;
  scorePassTd.value = settings.passingTd;
  scoreInterception.value = settings.interception;
  scoreRushYards.value = settings.rushingYardsPerPoint;
  scoreRushTd.value = settings.rushingTd;
  scoreRecYards.value = settings.receivingYardsPerPoint;
  scoreRecTd.value = settings.receivingTd;
  scoreReception.value = settings.reception;
  scoreFumble.value = settings.fumbleLost;
  scoreTwoPoint.value = settings.twoPointConversion;
}

function renderManagers(leagueState) {
  const managers = leagueState?.members || [];
  if (managerCount) managerCount.textContent = String(managers.length);
  if (!managerList) return;
  if (!managers.length) {
    managerList.textContent = 'No managers invited yet.';
    return;
  }
  const canManage = isCurrentCommissioner(leagueState);
  managerList.innerHTML = managers.map((member, index) => `
    <div class="row">
      <div>
        <strong>${escapeHtml(member.teamName || `Manager ${index + 1}`)}</strong>
        <div class="muted">${escapeHtml(member.email)}</div>
        <div class="muted small">${member.role === 'commissioner' ? 'Commissioner' : 'Member'} / ${member.status || 'Invited'}</div>
      </div>
      <div class="actions">
        ${canManage ? `
          <input class="lineup-select manager-name-input" data-member-team-name="${escapeHtml(member.email)}" type="text" value="${escapeHtml(member.teamName || '')}" placeholder="Team name">
          <button class="button" data-member-team-save="${escapeHtml(member.email)}" type="button">Save name</button>
        ` : ''}
        <span class="pill pill--muted">${member.status || 'Invited'}</span>
        ${canManage && member.email !== getAuthState()?.email ? `
          <button class="button" data-member-activate="${member.email}" type="button">Confirm</button>
          <button class="button" data-member-role="${member.email}" data-role="${member.role === 'commissioner' ? 'member' : 'commissioner'}" type="button">
            ${member.role === 'commissioner' ? 'Make member' : 'Make commissioner'}
          </button>
          <button class="button button--ghost" data-member-remove="${member.email}" type="button">Remove</button>
        ` : ''}
      </div>
    </div>
  `).join('');
  managerList.querySelectorAll('[data-member-activate]').forEach((button) => {
    button.addEventListener('click', async () => {
      const member = managers.find((item) => item.email === button.dataset.memberActivate);
      await updateMemberApi(button.dataset.memberActivate, { role: member?.role || 'member', status: 'Active' });
      await refreshLeagueFromApi();
      renderLeague();
    });
  });
  managerList.querySelectorAll('[data-member-role]').forEach((button) => {
    button.addEventListener('click', async () => {
      const member = managers.find((item) => item.email === button.dataset.memberRole);
      await updateMemberApi(button.dataset.memberRole, { role: button.dataset.role, status: member?.status || 'Invited' });
      await refreshLeagueFromApi();
      renderLeague();
    });
  });
  managerList.querySelectorAll('[data-member-team-save]').forEach((button) => {
    button.addEventListener('click', async () => {
      const email = button.dataset.memberTeamSave;
      const member = managers.find((item) => item.email === email);
      const input = Array.from(managerList.querySelectorAll('[data-member-team-name]'))
        .find((item) => item.dataset.memberTeamName === email);
      await updateMemberApi(email, {
        role: member?.role || 'member',
        status: member?.status || 'Invited',
        teamName: input?.value?.trim() || ''
      });
      await refreshLeagueFromApi();
      renderLeague();
    });
  });
  managerList.querySelectorAll('[data-member-remove]').forEach((button) => {
    button.addEventListener('click', async () => {
      await updateMemberApi(button.dataset.memberRemove, { status: 'Removed' });
      await refreshLeagueFromApi();
      renderLeague();
    });
  });
}

function renderLobbyStatus(leagueState) {
  if (!draftLobbyStatus) return;
  if (!leagueState) {
    draftLobbyStatus.textContent = 'Create a league before opening the draft lobby.';
    return;
  }
  if (!leagueState.draftLobbyOpen) {
    draftLobbyStatus.textContent = 'Not opened yet.';
    return;
  }
  const opened = leagueState.draftLobbyStartedAt
    ? `Opened ${new Date(leagueState.draftLobbyStartedAt).toLocaleString()}.`
    : 'Open now.';
  draftLobbyStatus.textContent = `${opened} Managers can enter the draft room.`;
  if (draftLobbyLink) {
    draftLobbyLink.href = `draft.html?league=${encodeURIComponent(leagueState.id)}`;
  }
}

function setSettingsStatus(message, isError = false) {
  if (!settingsStatus) return;
  settingsStatus.textContent = message;
  settingsStatus.style.color = isError ? 'var(--danger)' : 'var(--muted)';
}

function requireCommissioner() {
  if (isCurrentCommissioner()) return true;
  setSettingsStatus(getAuthState() ? 'Only commissioners can change league settings.' : 'Sign in as commissioner to change league settings.', true);
  if (commissionerLocked) commissionerLocked.scrollIntoView({ behavior: 'smooth', block: 'start' });
  return false;
}

function readRosterRules() {
  return {
    qb: Number(rulesQb.value || defaultRosterRules.qb),
    rb: Number(rulesRb.value || defaultRosterRules.rb),
    wr: Number(rulesWr.value || defaultRosterRules.wr),
    te: Number(rulesTe.value || defaultRosterRules.te),
    flex: Number(rulesFlex.value || defaultRosterRules.flex),
    bench: Number(rulesBench.value || defaultRosterRules.bench)
  };
}

function readWaiverRules() {
  return {
    mode: waiverMode.value || defaultWaiverRules.mode,
    claimDeadline: waiverDeadline.value || '',
    freeAgencyLocked: waiverFaLock.value === 'true'
  };
}

function readTradeRules() {
  return {
    commissionerApproval: tradeApproval.value === 'true',
    expirationHours: Math.max(1, Number(tradeExpiration.value || defaultTradeRules.expirationHours))
  };
}

function readScoringSettings() {
  return {
    passingYardsPerPoint: Number(scorePassYards.value || scoringPresets.ppr.passingYardsPerPoint),
    passingTd: Number(scorePassTd.value || scoringPresets.ppr.passingTd),
    interception: Number(scoreInterception.value || scoringPresets.ppr.interception),
    rushingYardsPerPoint: Number(scoreRushYards.value || scoringPresets.ppr.rushingYardsPerPoint),
    rushingTd: Number(scoreRushTd.value || scoringPresets.ppr.rushingTd),
    receivingYardsPerPoint: Number(scoreRecYards.value || scoringPresets.ppr.receivingYardsPerPoint),
    receivingTd: Number(scoreRecTd.value || scoringPresets.ppr.receivingTd),
    reception: Number(scoreReception.value || scoringPresets.ppr.reception),
    fumbleLost: Number(scoreFumble.value || scoringPresets.ppr.fumbleLost),
    twoPointConversion: Number(scoreTwoPoint.value || scoringPresets.ppr.twoPointConversion)
  };
}

async function syncInviteEmailsFromSettings(inviteEmails = [], existingMembers = []) {
  const current = getLeagueState();
  if (!current) return { invited: 0, failed: 0 };
  const existing = new Set(existingMembers.map((member) => String(member.email || '').toLowerCase()));
  const commissionerEmail = getAuthState()?.email?.toLowerCase();
  let invited = 0;
  let failed = 0;
  for (const email of inviteEmails) {
    const key = String(email || '').toLowerCase();
    if (!key || key === commissionerEmail || existing.has(key)) continue;
    try {
      await inviteMemberApi(email);
      existing.add(key);
      invited += 1;
    } catch {
      failed += 1;
    }
  }
  return { invited, failed };
}

function inviteUrl() {
  const leagueState = getLeagueState();
  const base = `${window.location.origin}${window.location.pathname.replace('league.html', 'signin.html')}`;
  return leagueState ? `${base}?invite=${encodeURIComponent(leagueState.id)}` : base;
}

async function refreshLeagueFromApi() {
  if (!getAuthState()?.token) return;
  try {
    await syncLeaguesFromApi();
    await syncActiveLeagueCollectionsFromApi();
  } catch {
    // Keep the local cache usable when the API is unavailable.
  }
}

async function acceptInviteFromUrl() {
  const invite = new URLSearchParams(window.location.search).get('invite');
  if (!invite || !getAuthState()?.token) return;
  try {
    const joined = await joinLeagueApi(invite);
    if (joined?.joinStatus === 'pending_approval') {
      window.history.replaceState({}, document.title, 'league.html');
      if (emptyState) {
        emptyState.hidden = false;
        emptyState.querySelector('p')?.replaceChildren(document.createTextNode(joined.message || 'Join request submitted. A commissioner must approve access.'));
      }
      window.CFF_UI?.notify(joined.message || 'Join request submitted. A commissioner must approve access.', 'info');
      return;
    }
    if (joined?.id) {
      setActiveLeague(joined.id);
      window.history.replaceState({}, document.title, 'league.html');
    }
  } catch {
    setSettingsStatus('Invite could not be joined for this account.', true);
  }
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  renderLeague();
});

clearLeagueBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (current) {
    try {
      await removeLeagueFromApi(current.id);
    } catch {
      removeLeagueForCurrentAccount(current.id);
    }
  }
  renderLeague();
});

settingsForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (!current) {
    setSettingsStatus('Create a league before saving settings.', true);
    return;
  }
  const inviteEmails = parseEmailList(settingsInvites.value);
  const existingMembers = current.members || [];
  const updated = normalizeLeague({
    ...current,
    name: settingsName.value.trim() || current.name,
    draftDate: settingsDraftDate.value,
    scoring: settingsScoring.value,
    scoringLabel: scoringLabel(settingsScoring.value),
    scoringSettings: readScoringSettings(),
    teams: Number(settingsTeams.value),
    invitedEmails: inviteEmails,
    notes: settingsNotes.value.trim(),
    rosterRules: readRosterRules(),
    waiverRules: readWaiverRules(),
    tradeRules: readTradeRules()
  });
  try {
    await saveLeagueToApi(updated);
    const inviteResult = await syncInviteEmailsFromSettings(inviteEmails, existingMembers);
    const inviteCopy = inviteResult.invited
      ? ` Invited ${inviteResult.invited} manager${inviteResult.invited === 1 ? '' : 's'}.`
      : '';
    const failCopy = inviteResult.failed
      ? ` ${inviteResult.failed} invite${inviteResult.failed === 1 ? '' : 's'} failed.`
      : '';
    setSettingsStatus(`Settings saved.${inviteCopy}${failCopy}`, Boolean(inviteResult.failed));
    await refreshLeagueFromApi();
  } catch {
    setLeagueState(updated);
    setSettingsStatus('Settings saved locally. API unavailable.');
  }
  renderLeague();
});

waiverForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (lineupLocked()) {
    if (waiverStatus) waiverStatus.textContent = 'Waivers are locked after finalized matchups.';
    return;
  }
  const player = samplePlayers.find((item) => item.id === waiverAddPlayer.value);
  if (!player) {
    if (waiverStatus) waiverStatus.textContent = 'No player selected.';
    return;
  }
  try {
    await submitWaiverClaimApi(player, waiverDropPlayer.value);
    if (waiverStatus) waiverStatus.textContent = 'Waiver claim submitted.';
  } catch {
    submitWaiverClaim(player, waiverDropPlayer.value);
    if (waiverStatus) waiverStatus.textContent = 'Waiver claim saved locally.';
  }
  renderLeague();
});

tradeForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!tradeTargetManager?.value) {
    if (tradeStatus) tradeStatus.textContent = 'Invite and confirm another manager before sending trade offers.';
    return;
  }
  let ok = false;
  let requestPlayer = samplePlayers.find((item) => item.id === tradeRequestPlayerId?.value);
  try {
    const targetRoster = await getManagerRosterApi(tradeTargetManager.value);
    requestPlayer = targetRoster.find((item) => item.id === tradeRequestPlayerId?.value) || requestPlayer;
  } catch {
    // Keep the sample fallback selected above.
  }
  try {
    ok = await submitTradeOfferApi(tradeOfferPlayer.value, tradeRequestPlayer.value.trim(), tradeTargetManager.value, requestPlayer, tradeNote?.value.trim() || '');
  } catch {
    ok = submitTradeOffer(tradeOfferPlayer.value, tradeRequestPlayer.value.trim(), tradeTargetManager.value, requestPlayer, tradeNote?.value.trim() || '');
  }
  if (tradeStatus) {
    tradeStatus.textContent = ok
      ? 'Trade offer sent.'
      : lineupLocked()
        ? 'Trades are locked after finalized matchups.'
        : 'Select a roster player, target manager, and requested player before proposing a trade.';
  }
  if (ok) tradeRequestPlayer.value = '';
  if (ok && tradeNote) tradeNote.value = '';
  renderLeague();
});

settingsScoring?.addEventListener('change', () => {
  populateScoringSettings(normalizeScoringSettings(settingsScoring.value));
});

tradeTargetManager?.addEventListener('change', () => {
  renderRequestedTradePlayers();
});

copyInviteLinkBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const link = inviteUrl();
  try {
    await navigator.clipboard.writeText(link);
    if (inviteLinkStatus) inviteLinkStatus.textContent = 'Invite link copied.';
  } catch {
    if (inviteLinkStatus) inviteLinkStatus.textContent = link;
  }
});

resetWaiverPriorityBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  try {
    await resetWaiverPriorityApi();
    if (waiverStatus) waiverStatus.textContent = 'Waiver priority reset.';
  } catch {
    if (waiverStatus) waiverStatus.textContent = 'Could not reset waiver priority.';
  }
  renderLeague();
});

stepInvites?.addEventListener('click', () => {
  if (!requireCommissioner()) return;
  setActiveLeagueTab('settings');
  settingsInvites?.focus();
  settingsInvites?.scrollIntoView({ behavior: 'smooth', block: 'center' });
});

stepRules?.addEventListener('click', () => {
  if (!requireCommissioner()) return;
  setActiveLeagueTab('settings');
  rulesQb?.focus();
  rulesQb?.scrollIntoView({ behavior: 'smooth', block: 'center' });
});

stepLobby?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (!current) {
    setSettingsStatus('Create a league before opening the draft lobby.', true);
    return;
  }
  const updated = normalizeLeague({
    ...current,
    draftLobbyOpen: true,
    draftLobbyStartedAt: current.draftLobbyStartedAt || new Date().toISOString()
  });
  try {
    await saveLeagueToApi(updated);
  } catch {
    setLeagueState(updated);
  }
  renderLeague();
  window.location.href = `draft.html?league=${encodeURIComponent(updated.id)}`;
});

scoreboardWeek?.addEventListener('change', () => {
  activeScoreboardWeek = Number(scoreboardWeek.value || 1);
  renderLeague();
});

generateSeasonBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  generateSeasonBtn.disabled = true;
  if (scoreWeekStatus) scoreWeekStatus.textContent = 'Generating season schedule...';
  try {
    await generateSeasonScheduleApi(12);
    if (scoreWeekStatus) scoreWeekStatus.textContent = 'Season schedule generated.';
    await syncActiveLeagueCollectionsFromApi();
  } catch (error) {
    console.error(error);
    if (scoreWeekStatus) scoreWeekStatus.textContent = 'Could not generate season schedule from the API.';
  } finally {
    renderLeague();
  }
});

scoreWeekBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  scoreWeekBtn.disabled = true;
  if (scoreWeekStatus) scoreWeekStatus.textContent = `Calculating week ${activeScoreboardWeek} scores...`;
  try {
    const result = await scoreWeekApi(activeScoreboardWeek);
    if (scoreWeekStatus) {
      scoreWeekStatus.textContent = result?.scores?.length
        ? `Scored ${result.scores.length} active players from stat rows.`
        : 'No stat rows found yet; matchup scores were refreshed from available data.';
    }
    await syncActiveLeagueCollectionsFromApi();
  } catch (error) {
    console.error(error);
    if (scoreWeekStatus) scoreWeekStatus.textContent = error.lineupErrors?.length
      ? `Fix lineup first: ${error.lineupErrors.map((item) => item.message).join(' / ')}`
      : 'Could not score this week from the API.';
  } finally {
    scoreWeekBtn.disabled = !isCurrentCommissioner();
    renderLeague();
  }
});

finalizeWeekBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  finalizeWeekBtn.disabled = true;
  if (scoreWeekStatus) scoreWeekStatus.textContent = `Finalizing week ${activeScoreboardWeek}...`;
  try {
    await finalizeWeekApi(activeScoreboardWeek);
    if (scoreWeekStatus) scoreWeekStatus.textContent = `Week ${activeScoreboardWeek} is final. Standings are locked for this week.`;
    await syncActiveLeagueCollectionsFromApi();
  } catch (error) {
    console.error(error);
    if (scoreWeekStatus) scoreWeekStatus.textContent = error.lineupErrors?.length
      ? `Fix lineup first: ${error.lineupErrors.map((item) => item.message).join(' / ')}`
      : 'Could not finalize this week from the API.';
  } finally {
    renderLeague();
  }
});

async function initLeaguePage() {
  await validateAuthSession();
  renderLeague();
  setActiveLeagueTab(window.location.hash.replace('#', '') || 'overview');
  renderCommissionerAccess(isCurrentCommissioner(getLeagueState()));
  await acceptInviteFromUrl();
  await refreshLeagueFromApi();
  renderLeague();
  setActiveLeagueTab(window.location.hash.replace('#', '') || 'overview');
}

initLeaguePage();

leagueTabs.forEach((tab) => {
  tab.addEventListener('click', () => {
    setActiveLeagueTab(tab.dataset.leagueTab);
  });
});

window.addEventListener('hashchange', () => {
  setActiveLeagueTab(window.location.hash.replace('#', '') || 'overview');
});

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY, CFF_ROSTER_KEY, CFF_WAIVERS_KEY, CFF_WAIVER_PRIORITIES_KEY, CFF_TRADES_KEY, CFF_TRANSACTIONS_KEY, CFF_MATCHUPS_KEY].includes(event.key)) {
    renderLeague();
  }
});
