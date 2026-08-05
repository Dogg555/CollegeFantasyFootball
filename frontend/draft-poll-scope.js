(() => {
  'use strict';

  const pageName = window.location.pathname.split('/').pop() || '';
  if (pageName !== 'draft.html') return;

  const nativeSetInterval = window.setInterval.bind(window);
  let draftPollIntercepted = false;

  window.setInterval = function scopedDraftSetInterval(callback, delay, ...args) {
    const isDraftPoll = !draftPollIntercepted
      && Number(delay) === 2000
      && typeof callback === 'function'
      && callback.toString().includes('refreshDraftFromApi');

    if (!isDraftPoll) {
      return nativeSetInterval(callback, delay, ...args);
    }

    draftPollIntercepted = true;
    window.setInterval = nativeSetInterval;

    return nativeSetInterval(async () => {
      if (document.visibilityState !== 'visible' || !window.getAuthState?.()?.token) return;
      try {
        await window.syncDraftFromApi?.();
      } catch {
        // Keep the last authoritative draft snapshot visible during an outage.
      }
      window.renderAll?.();
    }, delay, ...args);
  };
})();
