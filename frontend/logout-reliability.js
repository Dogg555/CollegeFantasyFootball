(() => {
  'use strict';

  const signOutButton = document.getElementById('signout-btn');
  if (!signOutButton) return;

  const loginStatus = document.getElementById('login-status');
  const authNote = document.getElementById('auth-note');
  const originalLabel = signOutButton.textContent || 'Sign out';

  function setMessage(message, isError = false) {
    if (typeof window.setStatus === 'function') {
      window.setStatus(loginStatus, message, isError);
      return;
    }
    if (!loginStatus) return;
    loginStatus.textContent = message;
    loginStatus.style.color = isError ? '#ffb3b3' : 'var(--muted)';
  }

  function clearConfirmedSession(message) {
    if (typeof window.clearSessionState === 'function') {
      window.clearSessionState();
    } else {
      try {
        window.sessionStorage.removeItem('cff_auth');
        window.localStorage.removeItem('cff_auth');
      } catch {
        // The server-side revocation is still authoritative.
      }
    }
    if (typeof window.updateSharedNav === 'function') window.updateSharedNav('');
    signOutButton.hidden = true;
    if (authNote) authNote.textContent = 'Not signed in yet.';
    setMessage(message);
  }

  function describeLogoutFailure(error) {
    if (typeof window.describeRequestError === 'function') {
      return window.describeRequestError(
        error,
        'Secure sign out could not be confirmed. Your saved session is still available so you can retry.'
      );
    }
    if (error?.status === 503 || error?.unavailable) {
      return 'Secure sign out could not be confirmed because the authentication service is unavailable. Try again before closing this tab.';
    }
    return error?.message || 'Secure sign out could not be confirmed. Try again.';
  }

  signOutButton.addEventListener('click', async (event) => {
    event.preventDefault();
    event.stopImmediatePropagation();
    if (signOutButton.dataset.signingOut === 'true') return;

    const auth = typeof window.getAuthState === 'function' ? window.getAuthState() : null;
    const token = String(auth?.token || '');
    if (!token) {
      clearConfirmedSession('Signed out.');
      return;
    }
    if (token.startsWith('local-demo-')) {
      clearConfirmedSession('Local preview session cleared.');
      return;
    }

    signOutButton.dataset.signingOut = 'true';
    signOutButton.disabled = true;
    signOutButton.textContent = 'Signing out...';
    setMessage('Revoking this session...');

    try {
      if (typeof window.postJson !== 'function') {
        throw new Error('Authentication request helper is unavailable');
      }
      const payload = await window.postJson('/auth/logout', {}, token);
      if (payload?.status !== 'ok') {
        throw new Error('The server did not confirm session revocation');
      }
      clearConfirmedSession('Signed out securely.');
    } catch (error) {
      if (error?.status === 401 || error?.status === 403) {
        clearConfirmedSession('Signed out. The server no longer recognizes this session.');
      } else {
        setMessage(describeLogoutFailure(error), true);
      }
    } finally {
      delete signOutButton.dataset.signingOut;
      signOutButton.disabled = false;
      signOutButton.textContent = originalLabel;
    }
  }, true);
})();
