(() => {
  const PENDING_INVITE_KEY = 'cff_pending_invite';
  const initialParams = new URLSearchParams(window.location.search);
  const inviteFromUrl = initialParams.get('invite') || '';

  if (inviteFromUrl) {
    sessionStorage.setItem(PENDING_INVITE_KEY, inviteFromUrl);
  }

  function pendingInvite() {
    return inviteFromUrl || sessionStorage.getItem(PENDING_INVITE_KEY) || '';
  }

  function authState() {
    try {
      return typeof getAuthState === 'function' ? getAuthState() : null;
    } catch {
      return null;
    }
  }

  function pageName() {
    return window.location.pathname.split('/').pop() || 'index.html';
  }

  function leagueInviteUrl(leagueId) {
    const target = new URL('league.html', window.location.href);
    target.searchParams.set('invite', leagueId);
    return target.toString();
  }

  function preserveInviteOnAuthLinks() {
    const invite = pendingInvite();
    if (!invite) return;
    document.querySelectorAll('a[href]').forEach((link) => {
      const href = link.getAttribute('href') || '';
      if (!/^(signin|signup|verify-email|resend-verification)\.html(?:[?#]|$)/.test(href)) return;
      const target = new URL(href, window.location.href);
      target.searchParams.set('invite', invite);
      link.setAttribute('href', `${target.pathname.split('/').pop()}${target.search}${target.hash}`);
    });
  }

  function continueStoredInvite() {
    const invite = pendingInvite();
    if (!invite) return;
    const auth = authState();
    const page = pageName();

    if (page === 'signin.html' && auth?.token) {
      window.location.replace(leagueInviteUrl(invite));
      return;
    }

    if (page === 'league.html' && auth?.token) {
      if (!new URLSearchParams(window.location.search).get('invite')) {
        window.location.replace(leagueInviteUrl(invite));
        return;
      }
      sessionStorage.removeItem(PENDING_INVITE_KEY);
    }
  }

  const nativeFetch = window.fetch.bind(window);

  function requestUrl(input) {
    if (typeof input === 'string' || input instanceof URL) return new URL(input, window.location.href);
    return new URL(input.url, window.location.href);
  }

  function requestMethod(input, init = {}) {
    return String(init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
  }

  function requestHeaders(input, init = {}) {
    const headers = new Headers(input instanceof Request ? input.headers : undefined);
    new Headers(init.headers || {}).forEach((value, key) => headers.set(key, value));
    return headers;
  }

  function requestJson(init = {}) {
    if (typeof init.body !== 'string') return null;
    try {
      return JSON.parse(init.body);
    } catch {
      return null;
    }
  }

  async function sendCreationInvites(apiUrl, input, init, response) {
    const body = requestJson(init);
    const invitedEmails = Array.isArray(body?.invitedEmails) ? body.invitedEmails : [];
    if (!response.ok || !invitedEmails.length) return;

    const createdLeague = await response.clone().json().catch(() => null);
    if (!createdLeague?.id) return;

    const commissionerEmail = String(authState()?.email || '').toLowerCase();
    const uniqueEmails = [...new Set(invitedEmails
      .map((email) => String(email || '').trim().toLowerCase())
      .filter((email) => email && email !== commissionerEmail))];
    if (!uniqueEmails.length) return;

    const headers = requestHeaders(input, init);
    headers.set('Content-Type', 'application/json');
    const basePath = apiUrl.pathname.replace(/\/$/, '');

    await Promise.allSettled(uniqueEmails.map((email) => nativeFetch(
      `${apiUrl.origin}${basePath}/${encodeURIComponent(createdLeague.id)}/members`,
      {
        method: 'POST',
        headers,
        body: JSON.stringify({ email, role: 'member' })
      }
    )));
  }

  window.fetch = async (input, init = {}) => {
    const apiUrl = requestUrl(input);
    const method = requestMethod(input, init);
    const response = await nativeFetch(input, init);

    if (method === 'POST' && /\/api\/leagues\/?$/.test(apiUrl.pathname)) {
      sendCreationInvites(apiUrl, input, init, response).catch((error) => {
        console.error('Unable to send initial league invitations.', error);
      });
    }

    return response;
  };

  document.addEventListener('click', async (event) => {
    const button = event.target.closest?.('#copy-invite-link');
    if (!button) return;

    event.preventDefault();
    event.stopImmediatePropagation();

    const league = typeof getLeagueState === 'function' ? getLeagueState() : null;
    const status = document.getElementById('invite-link-status');
    if (!league?.id) {
      if (status) status.textContent = 'Create a league before copying an invite link.';
      return;
    }

    const link = leagueInviteUrl(league.id);
    try {
      await navigator.clipboard.writeText(link);
      if (status) status.textContent = 'Invite link copied.';
    } catch {
      if (status) status.textContent = link;
    }
  }, true);

  document.addEventListener('DOMContentLoaded', () => {
    preserveInviteOnAuthLinks();
    continueStoredInvite();
  }, { once: true });

  window.setTimeout(() => {
    preserveInviteOnAuthLinks();
    continueStoredInvite();
  }, 0);
})();
