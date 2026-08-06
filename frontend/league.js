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
const leagueFeedList = document.getElementById('league-feed-list');
const leagueFeedCount = document.getElementById('league-feed-count');
const commissionerPostForm = document.getElementById('commissioner-post-form');
const commissionerPost = document.getElementById('commissioner-post');
const commissionerPostStatus = document.getElementById('commissioner-post-status');
const commissionerSettings = document.getElementById('commissioner-settings');
const commissionerLocked = document.getElementById('commissioner-locked');
const settingsForm = document.getElementById('league-settings-form');
const settingsName = document.getElementById('settings-name');
const settingsDraftDate = document.getElementById('settings-draft-date');
const settingsDraftTime = document.getElementById('settings-draft-time');
const settingsDraftTimezone = document.getElementById('settings-draft-timezone');
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

function findAvailablePlayerById(playerId) {
  const selectedId = String(playerId || '');
  const availablePlayer = getAvailablePlayers().find((player) => String(player.id) === selectedId);
  if (availablePlayer) return availablePlayer;
  return isLocalDemoSession()
    ? samplePlayers.find((player) => String(player.id) === selectedId) || null
    : null;
}
const tradeExpiration = document.getElementById('trade-expiration');
const managerList = document.getElementById('manager-list');
let latestApiFeedItems = [];
const managerCount = document.getElementById('manager-count');
const copyInviteLinkBtn = document.getElementById('copy-invite-link');
const inviteLinkStatus = document.getElementById('invite-link-status');
const inviteFlow = document.getElementById('invite-flow');
const inviteFlowTitle = document.getElementById('invite-flow-title');
const inviteFlowCopy = document.getElementById('invite-flow-copy');
const inviteFlowStatus = document.getElementById('invite-flow-status');
const inviteFlowDetail = document.getElementById('invite-flow-detail');
const joinInviteNowBtn = document.getElementById('join-invite-now');
const inviteSigninLink = document.getElementById('invite-signin-link');
const stepInvites = document.getElementById('step-invites');
const stepRules = document.getElementById('step-rules');
const stepLobby = document.getElementById('step-lobby');
const draftLobbyStatus = document.getElementById('draft-lobby-status');
const draftLobbyLink = document.getElementById('draft-lobby-link');
const draftLobbyOverviewStatus = document.getElementById('draft-lobby-overview-status');
const draftLobbyOverviewLink = document.getElementById('draft-lobby-overview-link');
const draftLobbyOverviewBadge = document.getElementById('draft-lobby-overview-badge');
const leagueDraftTabLink = document.getElementById('league-draft-tab-link');
const teamDraftLink = document.getElementById('team-draft-link');
const leagueTabs = document.querySelectorAll('[data-league-tab]');
const leaguePanels = document.querySelectorAll('[data-league-panel]');
let activeScoreboardWeek = 1;
const CFF_PENDING_JOIN_KEY = 'cff_pending_join_requests';
const CFF_LEAGUE_FEED_POSTS_KEY = 'cff_league_feed_posts_by_league';

function renderLeague() {
  updateSharedNav('league');
  const authState = getAuthState();
  const leagueState = getLeagueState();
  const queue = getQueue();
  renderInviteFlow();
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
  renderLeagueFeed(leagueState);
  renderTransactions();
  renderApiOutageState();
}

function renderApiOutageState() {
  const meta = apiCacheMeta('league') || apiCacheMeta('leagues');
  const stale = Boolean(meta?.stale || mutationControlsDisabled());
  let banner = document.getElementById('api-stale-warning');
  if (!stale) {
    banner?.remove();
    details?.querySelectorAll('[data-cff-outage-disabled="true"]').forEach((button) => {
      button.disabled = false;
      delete button.dataset.cffOutageDisabled;
    });
  } else {
    if (!banner) {
      banner = document.createElement('div');
      banner.id = 'api-stale-warning';
      banner.className = 'notice notice--warning';
      details?.prepend(banner);
    }
    const fetched = meta?.fetchedAt ? ` Last server refresh: ${new Date(meta.fetchedAt).toLocaleString()}.` : '';
    banner.textContent = `Showing cached league data because the API is unavailable. Mutation controls are disabled until the service recovers.${fetched}`;
  }
  if (details) {
    details.querySelectorAll('button:not([data-league-tab]):not(#nav-logout)').forEach((button) => {
      if (!stale || button.disabled) return;
      button.disabled = true;
      button.dataset.cffOutageDisabled = 'true';
    });
  }
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
      } catch (error) {
        setSettingsStatus(mutationErrorMessage(error, 'Could not remove league. No local changes were made.'), true);
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
          const player = findAvailablePlayerById(button.dataset.addFreeAgent);
          if (player) {
            try {
              await addFreeAgentApi(player);
            } catch (error) {
              if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not add player. No local changes were made.');
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
          } catch (error) {
            if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not drop player. No local changes were made.');
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
    } catch (error) {
      if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not process waivers. No local changes were made.');
    }
    renderLeague();
  });
  waiverList.querySelectorAll('[data-process-waiver]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await processWaiverClaimApi(button.dataset.processWaiver);
      } catch (error) {
        if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not process waiver claim. No local changes were made.');
      }
      renderLeague();
    });
  });
  waiverList.querySelectorAll('[data-cancel-waiver]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await cancelWaiverClaimApi(button.dataset.cancelWaiver);
        if (waiverStatus) waiverStatus.textContent = 'Waiver claim cancelled.';
      } catch (error) {
        if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not cancel waiver claim. No local changes were made.');
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
    } catch (error) {
      if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not reorder waiver claims. No local changes were made.');
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
      } catch (error) {
        if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not accept trade. No local changes were made.');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-decline]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeDecline, 'Declined');
      } catch (error) {
        if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not decline trade. No local changes were made.');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-cancel]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeCancel, 'Cancelled');
      } catch (error) {
        if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not cancel trade. No local changes were made.');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-approve]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeApprove, 'Approved');
      } catch (error) {
        if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not approve trade. No local changes were made.');
      }
      renderLeague();
    });
  });
  tradeList.querySelectorAll('[data-trade-veto]').forEach((button) => {
    button.addEventListener('click', async () => {
      try {
        await updateTradeStatusApi(button.dataset.tradeVeto, 'Vetoed');
      } catch (error) {
        if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not veto trade. No local changes were made.');
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
    if (!roster.length) {
      tradeRequestPlayerId.innerHTML = '<option value="">No tradeable players</option>';
      tradeRequestPlayerId.disabled = true;
      return;
    }
    tradeRequestPlayerId.innerHTML = roster
      .map((player) => `<option value="${player.id}">${player.name} (${player.position})</option>`)
      .join('');
  } catch {
    tradeRequestPlayerId.innerHTML = '<option value="">Roster unavailable</option>';
    tradeRequestPlayerId.disabled = true;
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

function readFeedPostStore() {
  try {
    return JSON.parse(localStorage.getItem(CFF_LEAGUE_FEED_POSTS_KEY) || '{}') || {};
  } catch {
    return {};
  }
}

function feedPostKey(league = getLeagueState()) {
  return league?.id || 'local';
}

function getCommissionerPosts(league = getLeagueState()) {
  const store = readFeedPostStore();
  const posts = store[feedPostKey(league)];
  return Array.isArray(posts) ? posts : [];
}

function saveCommissionerPosts(posts = [], league = getLeagueState()) {
  const store = readFeedPostStore();
  store[feedPostKey(league)] = posts.slice(0, 50);
  localStorage.setItem(CFF_LEAGUE_FEED_POSTS_KEY, JSON.stringify(store));
}

async function loadLeagueFeedFromApi() {
  const league = getLeagueState();
  if (!league || !getAuthState()?.token) {
    latestApiFeedItems = [];
    return [];
  }
  const feed = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/feed`);
  latestApiFeedItems = Array.isArray(feed) ? feed.map(normalizeFeedItem).filter(Boolean) : [];
  return latestApiFeedItems;
}

function normalizeFeedItem(item) {
  if (!item || typeof item !== 'object') return null;
  const summary = String(item.summary || item.body || '').trim();
  if (!summary) return null;
  return feedItem(
    String(item.type || 'Feed'),
    summary,
    item.createdAt || new Date().toISOString(),
    item.managerEmail || '',
    item.badge || 'Feed'
  );
}

async function addCommissionerPost(message) {
  const league = getLeagueState();
  if (!league || !message.trim()) return false;
  const auth = getAuthState();
  if (auth?.token) {
    const post = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/feed/posts`, {
      method: 'POST',
      body: JSON.stringify({ body: message.trim() })
    });
    const normalized = normalizeFeedItem(post);
    if (normalized) {
      latestApiFeedItems = [normalized, ...latestApiFeedItems].slice(0, 100);
    }
    return Boolean(normalized);
  }
  const posts = getCommissionerPosts(league);
  posts.unshift({
    id: `post-${Date.now().toString(36)}`,
    type: 'Commissioner Post',
    summary: message.trim(),
    managerEmail: auth?.email || '',
    createdAt: new Date().toISOString()
  });
  saveCommissionerPosts(posts, league);
  return true;
}

function feedItem(type, summary, createdAt = new Date().toISOString(), managerEmail = '', badge = '') {
  return { type, summary, createdAt, managerEmail, badge };
}

function weeklyAwardItems(leagueState) {
  const items = [];
  const finalMatchups = getMatchups().filter((matchup) => String(matchup.status || '').toLowerCase() === 'final');
  if (finalMatchups.length) {
    const scored = finalMatchups.flatMap((matchup) => [
      { manager: matchup.homeManager, score: Number(matchup.homeScore || 0), opponent: matchup.awayManager, margin: Number(matchup.homeScore || 0) - Number(matchup.awayScore || 0), week: matchup.week || 1 },
      { manager: matchup.awayManager, score: Number(matchup.awayScore || 0), opponent: matchup.homeManager, margin: Number(matchup.awayScore || 0) - Number(matchup.homeScore || 0), week: matchup.week || 1 }
    ]).filter((entry) => entry.manager);
    const high = scored.slice().sort((a, b) => b.score - a.score)[0];
    const low = scored.slice().sort((a, b) => a.score - b.score)[0];
    const margin = scored.slice().sort((a, b) => b.margin - a.margin)[0];
    if (high) items.push(feedItem('Weekly Award', `${managerDisplayName(high.manager, leagueState)} posted the week's high score with ${high.score.toFixed(1)} points.`, new Date().toISOString(), high.manager, 'Highest Score'));
    if (low) items.push(feedItem('Weekly Award', `${managerDisplayName(low.manager, leagueState)} survived the lowest score at ${low.score.toFixed(1)} points.`, new Date().toISOString(), low.manager, 'Lowest Score'));
    if (margin && margin.margin > 0) items.push(feedItem('Weekly Award', `${managerDisplayName(margin.manager, leagueState)} won by ${margin.margin.toFixed(1)} points over ${managerDisplayName(margin.opponent, leagueState)}.`, new Date().toISOString(), margin.manager, 'Largest Margin'));
  }
  return items;
}

function buildLeagueFeedItems(leagueState = getLeagueState()) {
  if (latestApiFeedItems.length) {
    return latestApiFeedItems
      .filter((item) => item.summary)
      .sort((a, b) => new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime())
      .slice(0, 40);
  }
  const posts = getCommissionerPosts(leagueState).map((post) => feedItem(
    post.type || 'Commissioner Post',
    post.summary || '',
    post.createdAt,
    post.managerEmail,
    'Post'
  ));
  const transactions = getTransactions().map((txn) => feedItem(
    txn.type || 'Transaction',
    txn.summary || '',
    txn.createdAt,
    txn.managerEmail,
    'Transaction'
  ));
  const waivers = getWaiverClaims().map((claim) => feedItem(
    `Waiver ${claim.status || 'Claim'}`,
    `${claim.status || 'Claim'}: ${claim.addPlayer?.name || 'player'}${claim.dropPlayerId ? ' with a drop' : ''}`,
    claim.createdAt,
    claim.managerEmail,
    'Waiver'
  ));
  const trades = getTradeOffers().map((trade) => feedItem(
    `Trade ${trade.status || 'Offer'}`,
    `${trade.status || 'Offer'}: ${trade.offerPlayer?.name || 'player'} for ${trade.requestPlayer?.name || trade.requestPlayerName || 'return'}`,
    trade.createdAt,
    trade.offeredByEmail,
    'Trade'
  ));
  const matchupResults = getMatchups()
    .filter((matchup) => String(matchup.status || '').toLowerCase() === 'final')
    .map((matchup) => {
      const homeScore = Number(matchup.homeScore || 0);
      const awayScore = Number(matchup.awayScore || 0);
      const winner = homeScore >= awayScore ? matchup.homeManager : matchup.awayManager;
      const loser = homeScore >= awayScore ? matchup.awayManager : matchup.homeManager;
      const winnerScore = Math.max(homeScore, awayScore);
      const loserScore = Math.min(homeScore, awayScore);
      return feedItem(
        'Final Score',
        `${managerDisplayName(winner, leagueState)} beat ${managerDisplayName(loser, leagueState)} ${winnerScore.toFixed(1)}-${loserScore.toFixed(1)}.`,
        matchup.finalizedAt || new Date().toISOString(),
        winner,
        'Final'
      );
    });
  return [...posts, ...transactions, ...waivers, ...trades, ...matchupResults, ...weeklyAwardItems(leagueState)]
    .filter((item) => item.summary)
    .sort((a, b) => new Date(b.createdAt || 0).getTime() - new Date(a.createdAt || 0).getTime())
    .slice(0, 40);
}

function renderLeagueFeed(leagueState) {
  if (!leagueFeedList) return;
  const canPost = isCurrentCommissioner(leagueState);
  if (commissionerPostForm) commissionerPostForm.hidden = !canPost;
  const items = buildLeagueFeedItems(leagueState);
  if (leagueFeedCount) leagueFeedCount.textContent = String(items.length);
  if (!items.length) {
    leagueFeedList.textContent = 'No feed items yet. League activity, waiver claims, trades, final scores, and commissioner posts will appear here.';
    return;
  }
  leagueFeedList.innerHTML = items.map((item) => `
    <div class="row">
      <div>
        <strong>${escapeHtml(item.type)}</strong>
        <div class="muted">${escapeHtml(item.summary)}</div>
        ${item.managerEmail ? `<div class="muted small">${escapeHtml(managerDisplayName(item.managerEmail, leagueState))}</div>` : ''}
      </div>
      <div>
        <div class="badge">${escapeHtml(item.badge || 'Feed')}</div>
        <div class="label">${new Date(item.createdAt || Date.now()).toLocaleString()}</div>
      </div>
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
  settingsDraftDate.value = draftDatePart(leagueState.draftDate || '');
  populateDraftTimeSelect(settingsDraftTime, draftHourPart(leagueState.draftDate || ''));
  if (settingsDraftTimezone) settingsDraftTimezone.textContent = `Timezone: ${draftTimezone()}`;
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
  const pendingCount = managers.filter((member) => member.status === 'Pending').length;
  if (managerCount) managerCount.textContent = pendingCount ? `${managers.length} / ${pendingCount} pending` : String(managers.length);
  if (!managerList) return;
  if (!managers.length) {
    managerList.textContent = 'No managers invited yet.';
    return;
  }
  const canManage = isCurrentCommissioner(leagueState);
  managerList.innerHTML = managers.map((member, index) => {
    const status = member.status || 'Invited';
    const statusCopy = status === 'Pending'
      ? 'Requested access'
      : status === 'Active'
        ? 'Confirmed manager'
        : 'Invited, not joined';
    return `
    <div class="row">
      <div>
        <strong>${escapeHtml(member.teamName || `Manager ${index + 1}`)}</strong>
        <div class="muted">${escapeHtml(member.email)}</div>
        <div class="muted small">${member.role === 'commissioner' ? 'Commissioner' : 'Member'} / ${statusCopy}</div>
      </div>
      <div class="actions">
        ${canManage ? `
          <input class="lineup-select manager-name-input" data-member-team-name="${escapeHtml(member.email)}" type="text" value="${escapeHtml(member.teamName || '')}" placeholder="Team name">
          <button class="button" data-member-team-save="${escapeHtml(member.email)}" type="button">Save name</button>
        ` : ''}
        <span class="pill ${status === 'Pending' ? '' : 'pill--muted'}">${status}</span>
        ${canManage && member.email !== getAuthState()?.email ? `
          <button class="button ${status === 'Pending' ? 'button--primary' : ''}" data-member-activate="${member.email}" type="button">${status === 'Pending' ? 'Approve' : 'Confirm'}</button>
          <button class="button" data-member-role="${member.email}" data-role="${member.role === 'commissioner' ? 'member' : 'commissioner'}" type="button">
            ${member.role === 'commissioner' ? 'Make member' : 'Make commissioner'}
          </button>
          <button class="button button--ghost" data-member-remove="${member.email}" type="button">Remove</button>
        ` : ''}
      </div>
    </div>
  `;
  }).join('');
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
  const commissioner = isCurrentCommissioner(leagueState);
  const links = [draftLobbyLink, draftLobbyOverviewLink, leagueDraftTabLink, teamDraftLink].filter(Boolean);
  if (!leagueState) {
    if (draftLobbyStatus) draftLobbyStatus.textContent = 'Create a league before opening the draft lobby.';
    if (draftLobbyOverviewStatus) draftLobbyOverviewStatus.textContent = 'No league selected.';
    links.forEach((link) => {
      link.href = 'league.html';
      link.setAttribute('aria-disabled', 'true');
    });
    return;
  }
  const href = `draft.html?league=${encodeURIComponent(leagueState.id)}`;
  links.forEach((link) => {
    link.href = href;
    link.removeAttribute('aria-disabled');
  });
  if (!leagueState.draftLobbyOpen) {
    if (draftLobbyStatus) draftLobbyStatus.textContent = 'Not opened yet.';
    if (draftLobbyOverviewStatus) {
      draftLobbyOverviewStatus.textContent = commissioner
        ? 'Open the lobby when active managers are ready to enter.'
        : 'Waiting for the commissioner to open the room.';
    }
    if (draftLobbyOverviewBadge) draftLobbyOverviewBadge.textContent = 'Closed';
    if (draftLobbyOverviewLink) draftLobbyOverviewLink.hidden = !commissioner;
    if (draftLobbyLink) draftLobbyLink.hidden = !commissioner;
    if (stepLobby) stepLobby.textContent = 'Open lobby';
    return;
  }
  const opened = leagueState.draftLobbyStartedAt
    ? `Opened ${new Date(leagueState.draftLobbyStartedAt).toLocaleString()}.`
    : 'Open now.';
  const message = `${opened} Active managers can enter and wait for the commissioner to start.`;
  if (draftLobbyStatus) draftLobbyStatus.textContent = message;
  if (draftLobbyOverviewStatus) draftLobbyOverviewStatus.textContent = message;
  if (draftLobbyOverviewBadge) draftLobbyOverviewBadge.textContent = 'Open';
  if (draftLobbyOverviewLink) draftLobbyOverviewLink.hidden = false;
  if (draftLobbyLink) draftLobbyLink.hidden = false;
  if (stepLobby) stepLobby.textContent = 'Enter lobby';
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

function pendingJoinStore() {
  try {
    return JSON.parse(localStorage.getItem(CFF_PENDING_JOIN_KEY) || '{}') || {};
  } catch {
    return {};
  }
}

function pendingJoinKey() {
  return getAuthState()?.email || 'anonymous';
}

function pendingJoinRequests() {
  const store = pendingJoinStore();
  return Array.isArray(store[pendingJoinKey()]) ? store[pendingJoinKey()] : [];
}

function savePendingJoinRequests(requests = []) {
  const store = pendingJoinStore();
  const key = pendingJoinKey();
  store[key] = requests;
  localStorage.setItem(CFF_PENDING_JOIN_KEY, JSON.stringify(store));
}

function recordPendingJoin(payload = {}) {
  const leagueId = payload.id || payload.leagueId;
  if (!leagueId) return;
  const requests = pendingJoinRequests().filter((request) => request.id !== leagueId);
  requests.unshift({
    id: leagueId,
    message: payload.message || 'Join request submitted. A commissioner must approve access.',
    createdAt: new Date().toISOString()
  });
  savePendingJoinRequests(requests.slice(0, 10));
}

function clearPendingJoin(leagueId) {
  if (!leagueId) return;
  savePendingJoinRequests(pendingJoinRequests().filter((request) => request.id !== leagueId));
}

function currentInviteId() {
  return new URLSearchParams(window.location.search).get('invite') || '';
}

function renderInviteFlow() {
  if (!inviteFlow || !inviteFlowDetail) return;
  const invite = currentInviteId();
  const auth = getAuthState();
  const pending = pendingJoinRequests();
  const activeLeague = getLeagueState();
  if (activeLeague?.id) clearPendingJoin(activeLeague.id);
  if (!invite && !pending.length) {
    inviteFlow.hidden = true;
    return;
  }

  inviteFlow.hidden = false;
  if (inviteFlowTitle) inviteFlowTitle.textContent = invite ? 'League invite' : 'Pending league access';
  if (inviteFlowStatus) inviteFlowStatus.textContent = invite ? 'Invite' : 'Pending';
  if (inviteSigninLink) {
    inviteSigninLink.hidden = Boolean(auth);
    inviteSigninLink.href = invite ? `signin.html?invite=${encodeURIComponent(invite)}` : 'signin.html';
  }
  if (joinInviteNowBtn) {
    joinInviteNowBtn.hidden = !invite || !auth;
    joinInviteNowBtn.disabled = false;
  }

  if (invite && !auth) {
    if (inviteFlowCopy) inviteFlowCopy.textContent = 'Sign in or create an account to request access to this league.';
    inviteFlowDetail.innerHTML = `
      <div class="row">
        <div>
          <strong>Invite ready</strong>
          <div class="muted">After signing in, the app will submit your join request for commissioner approval.</div>
        </div>
      </div>
    `;
    return;
  }

  if (invite && auth) {
    if (inviteFlowCopy) inviteFlowCopy.textContent = 'Submit this invite to request access from the league commissioner.';
    inviteFlowDetail.innerHTML = `
      <div class="row">
        <div>
          <strong>Ready to join</strong>
          <div class="muted">League ID: ${escapeHtml(invite)}</div>
        </div>
      </div>
    `;
    return;
  }

  if (inviteFlowCopy) inviteFlowCopy.textContent = 'These requests are waiting for a commissioner to approve you.';
  inviteFlowDetail.innerHTML = pending.map((request) => `
    <div class="row">
      <div>
        <strong>League ${escapeHtml(request.id)}</strong>
        <div class="muted">${escapeHtml(request.message)}</div>
        <div class="muted small">${new Date(request.createdAt).toLocaleString()}</div>
      </div>
      <span class="badge">Pending</span>
    </div>
  `).join('');
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
    await loadLeagueFeedFromApi();
  } catch {
    // Keep the local cache usable when the API is unavailable.
  }
}

async function acceptInviteFromUrl() {
  const invite = currentInviteId();
  if (!invite || !getAuthState()?.token) return;
  try {
    const joined = await joinLeagueApi(invite);
    if (joined?.joinStatus === 'pending_approval') {
      recordPendingJoin(joined);
      window.history.replaceState({}, document.title, 'league.html');
      if (emptyState) emptyState.textContent = joined.message || 'Join request submitted. A commissioner must approve access.';
      renderInviteFlow();
      window.CFF_UI?.notify(joined.message || 'Join request submitted. A commissioner must approve access.', 'info');
      return;
    }
    if (joined?.id) {
      clearPendingJoin(joined.id);
      setActiveLeague(joined.id);
      window.history.replaceState({}, document.title, 'league.html');
    }
  } catch {
    setSettingsStatus('Invite could not be joined for this account.', true);
    renderInviteFlow();
  }
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  renderLeague();
});

joinInviteNowBtn?.addEventListener('click', async () => {
  if (!currentInviteId()) return;
  joinInviteNowBtn.disabled = true;
  await acceptInviteFromUrl();
  renderLeague();
});

clearLeagueBtn?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (current) {
    try {
      await removeLeagueFromApi(current.id);
    } catch (error) {
      setSettingsStatus(mutationErrorMessage(error, 'Could not remove league. No local changes were made.'), true);
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
    draftDate: combineDraftDateAndHour(settingsDraftDate.value, settingsDraftTime?.value || '19'),
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
  if (updated.draftDate && !isTopOfHourDraftDate(updated.draftDate)) {
    setSettingsStatus('Draft time must be scheduled at the top of an hour.', true);
    settingsDraftDate?.focus();
    return;
  }
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
  } catch (error) {
    setSettingsStatus(mutationErrorMessage(error, 'Could not save settings. No local changes were made.'), true);
  }
  renderLeague();
});

commissionerPostForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!requireCommissioner()) return;
  const message = commissionerPost?.value || '';
  if (!message.trim()) {
    if (commissionerPostStatus) commissionerPostStatus.textContent = 'Write an announcement before posting.';
    commissionerPost?.focus();
    return;
  }
  try {
    const saved = await addCommissionerPost(message);
    if (!saved) {
      if (commissionerPostStatus) commissionerPostStatus.textContent = 'Could not save this post.';
      return;
    }
  } catch {
    if (commissionerPostStatus) commissionerPostStatus.textContent = 'Could not save this post.';
    return;
  }
  if (commissionerPost) commissionerPost.value = '';
  if (commissionerPostStatus) commissionerPostStatus.textContent = 'Commissioner post added to the league feed.';
  renderLeague();
});

waiverForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (lineupLocked()) {
    if (waiverStatus) waiverStatus.textContent = 'Waivers are locked after finalized matchups.';
    return;
  }
  const player = findAvailablePlayerById(waiverAddPlayer.value);
  if (!player) {
    if (waiverStatus) waiverStatus.textContent = 'No player selected.';
    return;
  }
  try {
    await submitWaiverClaimApi(player, waiverDropPlayer.value);
    if (waiverStatus) waiverStatus.textContent = 'Waiver claim submitted.';
  } catch (error) {
    if (waiverStatus) waiverStatus.textContent = mutationErrorMessage(error, 'Could not submit waiver claim. No local changes were made.');
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
  let requestPlayer = null;
  try {
    const targetRoster = await getManagerRosterApi(tradeTargetManager.value);
    requestPlayer = targetRoster.find((item) => String(item.id) === String(tradeRequestPlayerId?.value)) || null;
  } catch {
    requestPlayer = null;
  }
  try {
    ok = await submitTradeOfferApi(tradeOfferPlayer.value, tradeRequestPlayer.value.trim(), tradeTargetManager.value, requestPlayer, tradeNote?.value.trim() || '');
  } catch (error) {
    if (tradeStatus) tradeStatus.textContent = mutationErrorMessage(error, 'Could not send trade offer. No local changes were made.');
    renderLeague();
    return;
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
  if (current.draftLobbyOpen) {
    window.location.href = `draft.html?league=${encodeURIComponent(current.id)}`;
    return;
  }
  const updated = normalizeLeague({
    ...current,
    draftLobbyOpen: true,
    draftLobbyStartedAt: current.draftLobbyStartedAt || new Date().toISOString()
  });
  try {
    await saveLeagueToApi(updated);
    await refreshLeagueFromApi();
  } catch (error) {
    setSettingsStatus(mutationErrorMessage(error, 'Could not open draft lobby. No local changes were made.'), true);
    renderLeague();
    return;
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
    await loadLeagueFeedFromApi();
  } catch (error) {
    console.error(error);
    if (scoreWeekStatus) {
      scoreWeekStatus.textContent = mutationErrorMessage(error, 'Could not generate season schedule from the API.');
    }
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
    await loadLeagueFeedFromApi();
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
    await loadLeagueFeedFromApi();
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
  if ([CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY, CFF_ROSTER_KEY, CFF_WAIVERS_KEY, CFF_WAIVER_PRIORITIES_KEY, CFF_TRADES_KEY, CFF_TRANSACTIONS_KEY, CFF_MATCHUPS_KEY, CFF_LEAGUE_FEED_POSTS_KEY].includes(event.key)) {
    renderLeague();
  }
});
