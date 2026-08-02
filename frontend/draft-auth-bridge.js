(() => {
  const originalValidateAuthSession = window.validateAuthSession;
  if (typeof originalValidateAuthSession !== 'function') return;

  window.validateAuthSession = async function validateDraftAuthSession() {
    try {
      await window.CFFAuthSessionSync?.recover();
    } catch {
      // Continue with the normal validator when cross-tab recovery is unavailable.
    }
    return originalValidateAuthSession();
  };
})();
