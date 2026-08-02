(() => {
  const AUTH_KEY = 'cff_auth';
  const CHANNEL_NAME = 'cff-auth-session-v1';
  const pendingRequests = new Map();
  let channel = null;

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

  function openChannel() {
    if (channel || typeof window.BroadcastChannel !== 'function') return channel;
    channel = new window.BroadcastChannel(CHANNEL_NAME);
    channel.addEventListener('message', (event) => {
      const message = event.data || {};
      if (message.type === 'request' && message.requestId) {
        const auth = readSessionAuth();
        if (auth) {
          channel.postMessage({ type: 'response', requestId: message.requestId, auth });
        }
        return;
      }

      if (message.type !== 'response' || !message.requestId || !message.auth?.token) return;
      const pending = pendingRequests.get(message.requestId);
      if (!pending) return;
      pendingRequests.delete(message.requestId);
      window.clearTimeout(pending.timer);
      pending.resolve(writeSessionAuth(message.auth));
    });
    return channel;
  }

  async function recover(timeoutMs = 400) {
    const existing = readSessionAuth();
    if (existing) return existing;

    const activeChannel = openChannel();
    if (!activeChannel) return null;

    const id = requestId();
    return new Promise((resolve) => {
      const timer = window.setTimeout(() => {
        pendingRequests.delete(id);
        resolve(null);
      }, timeoutMs);
      pendingRequests.set(id, { resolve, timer });
      activeChannel.postMessage({ type: 'request', requestId: id });
    });
  }

  openChannel();
  window.CFFAuthSessionSync = Object.freeze({ recover });
})();
