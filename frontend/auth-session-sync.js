(() => {
  const AUTH_KEY = 'cff_auth';
  const CHANNEL_NAME = 'cff-auth-session-v1';
  const pendingRequests = new Map();
  let channel = null;

  function canonicalEmail(value = '') {
    return String(value).trim().toLowerCase();
  }

  function readSessionAuth() {
    try {
      const raw = window.sessionStorage.getItem(AUTH_KEY);
      const auth = raw ? JSON.parse(raw) : null;
      return auth?.token ? auth : null;
    } catch {
      return null;
    }
  }

  function writeSessionAuth(auth) {
    if (!auth?.token) return null;
    try {
      window.sessionStorage.setItem(AUTH_KEY, JSON.stringify(auth));
      return auth;
    } catch {
      return null;
    }
  }

  function requestId() {
    if (window.crypto?.randomUUID) return window.crypto.randomUUID();
    return `auth-${Date.now()}-${Math.random().toString(36).slice(2)}`;
  }

  function recoveryIdentity(auth) {
    const email = canonicalEmail(auth?.email);
    return email ? `email:${email}` : `token:${String(auth?.token || '')}`;
  }

  function selectRecoveredAuth(responses = [], expectedEmail = '') {
    const expected = canonicalEmail(expectedEmail);
    const accounts = new Map();

    responses.forEach((auth) => {
      if (!auth?.token) return;
      const email = canonicalEmail(auth.email);
      if (expected && email !== expected) return;
      const identity = recoveryIdentity(auth);
      if (!identity.endsWith(':') && !accounts.has(identity)) {
        accounts.set(identity, auth);
      }
    });

    // Fail closed when different accounts answer the same recovery request.
    // Multiple tabs for the same normalized email are treated as one account.
    if (accounts.size !== 1) return null;
    return accounts.values().next().value || null;
  }

  function finishRecovery(id) {
    const pending = pendingRequests.get(id);
    if (!pending) return;
    pendingRequests.delete(id);
    window.clearTimeout(pending.timer);
    const selected = selectRecoveredAuth(pending.responses, pending.expectedEmail);
    pending.resolve(selected ? writeSessionAuth(selected) : null);
  }

  function openChannel() {
    if (channel || typeof window.BroadcastChannel !== 'function') return channel;
    channel = new window.BroadcastChannel(CHANNEL_NAME);
    channel.addEventListener('message', (event) => {
      const message = event.data || {};
      if (message.type === 'request' && message.requestId) {
        const auth = readSessionAuth();
        const expectedEmail = canonicalEmail(message.expectedEmail);
        if (auth && (!expectedEmail || canonicalEmail(auth.email) === expectedEmail)) {
          channel.postMessage({ type: 'response', requestId: message.requestId, auth });
        }
        return;
      }

      if (message.type !== 'response' || !message.requestId || !message.auth?.token) return;
      const pending = pendingRequests.get(message.requestId);
      if (!pending) return;
      pending.responses.push(message.auth);
    });
    return channel;
  }

  async function recover(timeoutMs = 400, options = {}) {
    const existing = readSessionAuth();
    if (existing) return existing;

    const activeChannel = openChannel();
    if (!activeChannel) return null;

    const expectedEmail = canonicalEmail(
      typeof options === 'string' ? options : options?.expectedEmail
    );
    const duration = Math.max(50, Number(timeoutMs) || 400);
    const id = requestId();

    return new Promise((resolve) => {
      const pending = {
        resolve,
        timer: null,
        responses: [],
        expectedEmail
      };
      pendingRequests.set(id, pending);
      pending.timer = window.setTimeout(() => finishRecovery(id), duration);
      activeChannel.postMessage({
        type: 'request',
        requestId: id,
        ...(expectedEmail ? { expectedEmail } : {})
      });
    });
  }

  openChannel();
  window.CFFAuthSessionSync = Object.freeze({ recover });
})();
