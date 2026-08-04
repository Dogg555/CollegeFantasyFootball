(() => {
  const originalValidateAuthSession = window.validateAuthSession;
  if (typeof originalValidateAuthSession === 'function') {
    window.validateAuthSession = async function validateDraftAuthSession() {
      try {
        await window.CFFAuthSessionSync?.recover();
      } catch {
        // Continue with the normal validator when cross-tab recovery is unavailable.
      }
      return originalValidateAuthSession();
    };
  }

  function canonicalEmail(value = '') {
    return String(value).trim().toLowerCase();
  }

  function currentUserIsActiveLeagueMember(league) {
    const accountEmail = canonicalEmail(window.getAuthState?.()?.email);
    if (!accountEmail || !league) return false;
    return (league.members || []).some((member) => (
      canonicalEmail(member.email) === accountEmail
      && String(member.status || '').toLowerCase() === 'active'
    ));
  }

  function installDraftRoomAccessGate() {
    if (typeof window.getLeagueState !== 'function' || typeof window.getAuthState !== 'function') return;

    // The server already authorizes draft state by active league membership.
    // Keep the frontend aligned so every confirmed manager can enter the room
    // and wait before the commissioner opens or starts the draft.
    window.canEnterDraftRoom = function canActiveMemberEnterDraftRoom(league = window.getLeagueState()) {
      return currentUserIsActiveLeagueMember(league);
    };

    window.draftLockedCopy = function activeMemberDraftLockedCopy(league = window.getLeagueState()) {
      if (!window.getAuthState()) return 'Sign in to enter this league draft room.';
      if (!league) return 'Select or join a league before entering a draft room.';
      return 'Only confirmed active league managers can enter this draft room.';
    };

    const lockedHeading = document.querySelector('#draft-locked h2');
    if (lockedHeading) lockedHeading.textContent = 'Draft room access';

    const lockedBadge = document.querySelector('#draft-locked .pill');
    if (lockedBadge) lockedBadge.textContent = 'Members only';

    if (typeof window.renderAll === 'function') {
      window.renderAll();
    }
  }

  if (document.readyState === 'complete') {
    installDraftRoomAccessGate();
  } else {
    window.addEventListener('load', installDraftRoomAccessGate, { once: true });
  }
})();
