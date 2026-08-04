(function initAuthoritativeData(root) {
  'use strict';

  const SERVER_FUNCTIONS = Object.freeze([
    'syncLeaguesFromApi',
    'syncActiveLeagueCollectionsFromApi',
    'syncDraftFromApi',
    'draftPlayerApi',
    'saveDraftQueueApi',
    'saveDraftOrderApi',
    'startDraftApi',
    'resetDraftApi',
    'undoLastDraftPickApi',
    'saveLeagueToApi',
    'removeLeagueFromApi',
    'inviteMemberApi',
    'updateMemberApi',
    'joinLeagueApi',
    'addFreeAgentApi',
    'dropPlayerApi',
    'submitWaiverClaimApi',
    'processWaiverClaimApi',
    'cancelWaiverClaimApi',
    'reorderWaiverClaimsApi',
    'processWaiversApi',
    'resetWaiverPriorityApi',
    'submitTradeOfferApi',
    'updateTradeStatusApi',
    'getManagerRosterApi',
    'updateRosterSlotApi',
    'scoreWeekApi',
    'generateSeasonScheduleApi',
    'finalizeWeekApi'
  ]);

  const DEMO_ONLY_MUTATIONS = Object.freeze([
    'addPlayerToQueue',
    'draftPlayer',
    'undoLastDraftPick',
    'startDraft',
    'addFreeAgent',
    'dropPlayer',
    'setRosterSlot',
    'submitWaiverClaim',
    'cancelWaiverClaim',
    'reorderWaiverClaims',
    'processWaiverClaim',
    'processAllWaiverClaims',
    'submitTradeOffer',
    'updateTradeStatus'
  ]);

  function localDemoAllowed(location = root.location, enabled = root.CFF_ALLOW_LOCAL_DEMO) {
    const host = String(location?.hostname || '');
    return enabled === true && ['localhost', '127.0.0.1', '::1'].includes(host);
  }

  function explicitLocalDemo(auth = null, location = root.location, enabled = root.CFF_ALLOW_LOCAL_DEMO) {
    return localDemoAllowed(location, enabled)
      && String(auth?.token || '').startsWith('local-demo-');
  }

  function authorityError(action = 'continue') {
    const error = new Error(`Sign in with a verified server session to ${action}. No browser-only changes were made.`);
    error.status = 401;
    error.authRequired = true;
    error.authoritativeStateRequired = true;
    return error;
  }

  function requireServerSession(auth, demo, action = 'continue') {
    if (demo) return { mode: 'demo', auth };
    if (!auth?.token) throw authorityError(action);
    return { mode: 'server', auth };
  }

  function normalizedMemberStatus(status = 'Invited') {
    const lowered = String(status).toLowerCase();
    if (lowered === 'active') return 'Active';
    if (lowered === 'pending') return 'Pending';
    if (lowered === 'removed') return 'Removed';
    return 'Invited';
  }

  function normalizeMembersAuthoritatively(members = [], invitedEmails = [], auth = null, demo = false) {
    const byEmail = new Map();
    if (demo && auth?.email && (!Array.isArray(members) || members.length === 0)) {
      byEmail.set(auth.email, {
        email: auth.email,
        role: 'commissioner',
        status: 'Active',
        teamName: ''
      });
    }
    (Array.isArray(invitedEmails) ? invitedEmails : []).forEach((email) => {
      const normalizedEmail = String(email || '').trim();
      if (normalizedEmail && !byEmail.has(normalizedEmail)) {
        byEmail.set(normalizedEmail, {
          email: normalizedEmail,
          role: 'member',
          status: 'Invited',
          teamName: ''
        });
      }
    });
    (Array.isArray(members) ? members : []).forEach((member) => {
      const email = String(member?.email || '').trim();
      if (!email) return;
      byEmail.set(email, {
        email,
        role: member.role === 'commissioner' ? 'commissioner' : 'member',
        status: normalizedMemberStatus(member.status),
        invitedByEmail: member.invitedByEmail || '',
        teamName: member.teamName || member.team_name || ''
      });
    });
    return Array.from(byEmail.values()).filter((member) => member.status !== 'Removed');
  }

  function authoritativeDraftManager(meta = {}, draftManagerForPick = () => '') {
    const order = Array.isArray(meta?.draftOrder) ? meta.draftOrder : [];
    if (order.length) {
      return draftManagerForPick(order, meta.currentPick, meta.draftType || 'snake') || '';
    }
    return String(meta?.currentManager || '');
  }

  function authorizedDraftTurn(manager, auth = null) {
    return Boolean(manager && auth?.email && manager === auth.email);
  }

  const helpers = {
    SERVER_FUNCTIONS,
    DEMO_ONLY_MUTATIONS,
    localDemoAllowed,
    explicitLocalDemo,
    authorityError,
    requireServerSession,
    normalizeMembersAuthoritatively,
    authoritativeDraftManager,
    authorizedDraftTurn
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  let installed = false;
  let installAttempts = 0;

  function currentAuth() {
    return typeof root.getAuthState === 'function' ? root.getAuthState() : null;
  }

  function isDemo() {
    return explicitLocalDemo(currentAuth(), root.location, root.CFF_ALLOW_LOCAL_DEMO);
  }

  function wrapServerFunction(name) {
    const original = root[name];
    if (typeof original !== 'function' || original.__cffAuthoritativeServer) return;
    const wrapped = async function authoritativeServerFunction(...args) {
      requireServerSession(currentAuth(), isDemo(), String(name).replace(/Api$/, '').replace(/([a-z])([A-Z])/g, '$1 $2').toLowerCase());
      return original.apply(this, args);
    };
    wrapped.__cffAuthoritativeServer = true;
    wrapped.__cffOriginal = original;
    root[name] = wrapped;
  }

  function wrapDemoOnlyMutation(name) {
    const original = root[name];
    if (typeof original !== 'function' || original.__cffDemoOnlyMutation) return;
    const wrapped = function explicitDemoMutation(...args) {
      if (!isDemo()) throw authorityError(String(name).replace(/([a-z])([A-Z])/g, '$1 $2').toLowerCase());
      return original.apply(this, args);
    };
    wrapped.__cffDemoOnlyMutation = true;
    wrapped.__cffOriginal = original;
    root[name] = wrapped;
  }

  function installStrictMemberAuthority() {
    if (typeof root.normalizeMembers === 'function') {
      root.normalizeMembers = function strictNormalizeMembers(members = [], invitedEmails = []) {
        return normalizeMembersAuthoritatively(members, invitedEmails, currentAuth(), isDemo());
      };
    }

    if (typeof root.activeLeagueManagers === 'function') {
      root.activeLeagueManagers = function strictActiveLeagueManagers(league = root.getLeagueState?.()) {
        if (isDemo()) {
          const members = (league?.members || []).filter((member) => member.status !== 'Removed');
          return members.length ? members : currentAuth()?.email
            ? [{ email: currentAuth().email, status: 'Active', role: 'commissioner' }]
            : [];
        }
        return (league?.members || []).filter((member) => member.status !== 'Removed');
      };
    }
  }

  function installStrictAccountCacheAccess() {
    const originalGetLeagueState = root.getLeagueState;
    if (typeof originalGetLeagueState === 'function') {
      root.getLeagueState = function authoritativeLeagueState() {
        if (!currentAuth()?.email) return null;
        return originalGetLeagueState();
      };
    }

    const originalGetLeagues = root.getLeaguesForCurrentAccount;
    if (typeof originalGetLeagues === 'function') {
      root.getLeaguesForCurrentAccount = function authoritativeAccountLeagues() {
        if (!currentAuth()?.email) return [];
        return originalGetLeagues();
      };
    }

    const originalSaveLeague = root.saveLeagueForAccount;
    if (typeof originalSaveLeague === 'function') {
      root.saveLeagueForAccount = function cacheServerConfirmedLeague(...args) {
        requireServerSession(currentAuth(), isDemo(), 'save league data');
        return originalSaveLeague.apply(this, args);
      };
    }
  }

  function installStrictDraftAuthority() {
    const originalGetDraftMeta = root.getDraftMeta;
    if (typeof originalGetDraftMeta === 'function') {
      root.getDraftMeta = function authoritativeDraftMeta() {
        const meta = originalGetDraftMeta();
        if (isDemo()) return meta;
        const league = root.getLeagueState?.();
        const store = typeof root.readJson === 'function'
          ? root.readJson('cff_draft_meta_by_league', {})
          : {};
        const stored = Boolean(league?.id && Object.prototype.hasOwnProperty.call(store || {}, league.id));
        return stored ? { ...meta, currentManager: meta.currentManager || '' } : {
          ...meta,
          currentManager: '',
          pickDeadline: ''
        };
      };
    }

    if (typeof root.draftManagerForPick === 'function') {
      root.currentDraftManager = function strictCurrentDraftManager(meta = root.getDraftMeta?.() || {}) {
        return authoritativeDraftManager(meta, root.draftManagerForPick);
      };
    }

    root.isMyDraftTurn = function strictDraftTurn(meta = root.getDraftMeta?.() || {}) {
      return authorizedDraftTurn(root.currentDraftManager?.(meta) || '', currentAuth());
    };
  }

  function installNoSyntheticCompetitionData() {
    const originalAvailablePlayers = root.getAvailablePlayers;
    if (typeof originalAvailablePlayers === 'function') {
      root.getAvailablePlayers = function authoritativeAvailablePlayers() {
        return isDemo() ? originalAvailablePlayers() : [];
      };
    }

    ['generateLocalMatchups', 'generateLocalSeasonSchedule'].forEach((name) => {
      const original = root[name];
      if (typeof original !== 'function') return;
      root[name] = function demoScheduleOnly(...args) {
        return isDemo() ? original.apply(this, args) : [];
      };
    });

    const originalRecommended = root.renderRecommended;
    if (typeof originalRecommended === 'function') {
      root.renderRecommended = function authoritativeRecommendedBoard() {
        if (isDemo()) return originalRecommended();
        const list = document.getElementById('recommended-list');
        if (list) {
          list.textContent = 'Server-ranked recommendations are not available yet. Use the current player catalog to build your queue.';
        }
      };
    }
  }

  function installHomeQueueRenderer() {
    if (!document.getElementById('league-summary') || typeof root.renderSearchResults !== 'function') return;

    root.renderSearchResults = function authoritativeHomeSearchResults(players = [], fallback = false) {
      const results = document.getElementById('search-results');
      if (!results) return;
      if (!players.length) {
        results.textContent = 'No players matched that search.';
        return;
      }
      const queuedIds = new Set((root.getQueue?.() || []).map((player) => player.id));
      const previewOnly = Boolean(fallback && !isDemo());
      const safe = (value, fallbackValue = '') => root.escapeHtml?.(value ?? fallbackValue) || String(value ?? fallbackValue);
      const numeric = (value, fallbackValue = 0) => Number.isFinite(Number(value)) ? Number(value) : fallbackValue;
      const notice = fallback
        ? `<div class="row"><div><strong>${previewOnly ? 'Offline player preview' : 'Local demo player pool'}</strong><div class="muted">${previewOnly ? 'Preview players cannot be added to an authenticated draft queue.' : 'The local demo player pool is active.'}</div></div></div>`
        : '';
      results.innerHTML = notice + players.slice(0, 10).map((player, index) => {
        const queued = queuedIds.has(player.id);
        const disabled = queued || previewOnly;
        const label = queued ? 'Queued' : previewOnly ? 'Preview only' : 'Add to queue';
        return `
          <div class="row">
            <div>
              <strong>${safe(player.name, 'Unknown player')}</strong> - ${safe(player.team, 'Team TBD')} (${safe(player.position, 'FLEX')})
              <div class="muted">${safe(player.conference, 'Conference TBD')} / ${safe(player.class, 'Class TBD')} / ${numeric(player.projection).toFixed(1)} proj</div>
            </div>
            <button class="button" data-player-index="${index}" type="button" ${disabled ? 'disabled' : ''}>${label}</button>
          </div>
        `;
      }).join('');

      results.querySelectorAll('[data-player-index]').forEach((button) => {
        button.addEventListener('click', async () => {
          const player = players[Number(button.dataset.playerIndex)];
          if (!player) return;
          const nextQueue = [
            ...(root.getQueue?.() || []).filter((item) => item.id !== player.id),
            root.normalizePlayer(player)
          ];
          button.disabled = true;
          button.textContent = 'Saving...';
          try {
            await root.saveDraftQueueApi(nextQueue);
            root.setQueue(nextQueue);
            button.textContent = 'Queued';
            root.CFF_UI?.notify(`${player.name} added to your draft queue.`, 'success');
            root.renderDraftQueuePreview?.();
          } catch (error) {
            button.disabled = false;
            button.textContent = 'Add to queue';
            root.CFF_UI?.notify(root.mutationErrorMessage?.(error, 'Could not update draft queue. No local changes were made.') || error.message, 'error');
          }
        });
      });
    };
  }

  function installAuthoritativeLayer() {
    if (installed) return true;
    if (typeof root.getAuthState !== 'function' || typeof root.saveDraftQueueApi !== 'function') return false;

    installed = true;
    SERVER_FUNCTIONS.forEach(wrapServerFunction);
    DEMO_ONLY_MUTATIONS.forEach(wrapDemoOnlyMutation);
    installStrictMemberAuthority();
    installStrictAccountCacheAccess();
    installStrictDraftAuthority();
    installNoSyntheticCompetitionData();
    installHomeQueueRenderer();
    root.CFFAuthoritativeData = Object.freeze({
      installed: true,
      serverFunctions: [...SERVER_FUNCTIONS],
      demoOnlyMutations: [...DEMO_ONLY_MUTATIONS]
    });
    document.documentElement.dataset.cffAuthoritativeData = 'true';
    return true;
  }

  function installWhenReady() {
    installAttempts += 1;
    if (installAuthoritativeLayer() || installAttempts >= 200) return;
    root.setTimeout(installWhenReady, 0);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', installWhenReady, { once: true });
  }
  root.setTimeout(installWhenReady, 0);
})(typeof window !== 'undefined' ? window : globalThis);
