// Default same-origin API config for local development.
// Render prepends deployment-specific values before this file.
const CFF_DIRECT_API_BASE = 'https://college-ff-api.onrender.com/api';
const CFF_STALE_API_BASES = new Set([
  'https://api.college-fantasy-football.com/api'
]);

if (CFF_STALE_API_BASES.has(String(window.CFF_API_BASE || '').replace(/\/+$/, ''))) {
  console.warn('Replacing stale API configuration with the direct Render backend.');
  window.CFF_API_BASE = CFF_DIRECT_API_BASE;
} else {
  window.CFF_API_BASE = window.CFF_API_BASE || '/api';
}

window.CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO === true;
window.CFF_ALLOWED_LEAGUE_SIZES = Object.freeze([4, 6, 8, 10, 12, 14, 16]);

// Keep the league page functional if a browser briefly receives state.js and
// league.js from different static deployments. Newer state.js replaces these.
if (typeof window.apiCacheMeta !== 'function') {
  window.apiCacheMeta = (scope = 'league') => {
    try {
      const store = JSON.parse(window.localStorage.getItem('cff_api_cache_meta') || '{}');
      return store?.[scope] || null;
    } catch {
      return null;
    }
  };
}

if (typeof window.mutationControlsDisabled !== 'function') {
  window.mutationControlsDisabled = () => false;
}

if (typeof window.mutationErrorMessage !== 'function') {
  window.mutationErrorMessage = (error, fallback = 'Request failed. No local changes were made.') => {
    if (error?.status === 429) {
      const retry = error.retryAfter ? ` Retry after ${error.retryAfter} seconds.` : ' Try again later.';
      return `Too many requests.${retry}`;
    }
    if (error?.status === 503 || error?.unavailable) {
      return 'Service is temporarily unavailable. No local changes were made.';
    }
    return error?.data?.error || error?.message || fallback;
  };
}

(() => {
  const scripts = ['api-client.js', 'authoritative-data.js', 'auth-session-sync.js', 'draft-poll-scope.js', 'polish-core.js', 'polish-forms.js', 'polish-state.js', 'beta-ui.js', 'league-nav.js', 'workspace-ui.js', 'invite-fix.js', 'footer-links.js'];
  const leagueSizeSelectIds = ['league-size', 'settings-teams'];
  const assetVersion = String(window.CFF_BUILD_COMMIT || '').replace(/[^A-Za-z0-9._-]/g, '');

  function assetUrl(source) {
    return assetVersion ? `${source}?v=${encodeURIComponent(assetVersion)}` : source;
  }

  function ensureBranding() {
    if (!document.querySelector('link[rel~="icon"]')) {
      const icon = document.createElement('link');
      icon.rel = 'icon';
      icon.type = 'image/svg+xml';
      icon.href = assetUrl('assets/favicon.svg');
      document.head.appendChild(icon);
    }
    if (!document.querySelector('meta[name="theme-color"]')) {
      const theme = document.createElement('meta');
      theme.name = 'theme-color';
      theme.content = '#080d12';
      document.head.appendChild(theme);
    }
  }

  function ensureLeagueSizeOptions() {
    leagueSizeSelectIds.forEach((selectId) => {
      const select = document.getElementById(selectId);
      if (!select) return;

      const currentValue = select.value;
      const existingValues = new Set(Array.from(select.options, (option) => option.value));

      [4, 6].forEach((size) => {
        const value = String(size);
        if (existingValues.has(value)) return;
        const option = document.createElement('option');
        option.value = value;
        option.textContent = `${size} teams`;
        select.appendChild(option);
      });

      Array.from(select.options)
        .sort((left, right) => Number(left.value) - Number(right.value))
        .forEach((option) => select.appendChild(option));

      if (Array.from(select.options).some((option) => option.value === currentValue)) {
        select.value = currentValue;
      }
    });
  }

  function writeStylesheet(href, attribute) {
    const versionedHref = assetUrl(href);
    if (document.querySelector(`link[href="${versionedHref}"]`)) return;
    document.write(`<link rel="stylesheet" href="${versionedHref}" ${attribute}>`);
  }

  function appendStylesheet(href, datasetKey) {
    const versionedHref = assetUrl(href);
    if (document.querySelector(`link[href="${versionedHref}"]`)) return;
    const stylesheet = document.createElement('link');
    stylesheet.rel = 'stylesheet';
    stylesheet.href = versionedHref;
    if (datasetKey) stylesheet.dataset[datasetKey] = 'true';
    document.head.appendChild(stylesheet);
  }

  ensureBranding();

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', ensureLeagueSizeOptions, { once: true });
    writeStylesheet('polish.css', 'data-cff-polish="true"');
    writeStylesheet('alpha-ui.css', 'data-cff-modern="true"');
    writeStylesheet('beta-ui.css', 'data-cff-beta="true"');
    writeStylesheet('league-nav.css', 'data-cff-league-nav="true"');
    writeStylesheet('workspace-ui.css', 'data-cff-workspace="true"');
    scripts.forEach((source) => {
      document.write(`<script src="${assetUrl(source)}"><\/script>`);
    });
    return;
  }

  ensureLeagueSizeOptions();
  appendStylesheet('polish.css', 'cffPolish');
  appendStylesheet('alpha-ui.css', 'cffModern');
  appendStylesheet('beta-ui.css', 'cffBeta');
  appendStylesheet('league-nav.css', 'cffLeagueNav');
  appendStylesheet('workspace-ui.css', 'cffWorkspace');

  scripts.reduce((chain, source) => chain.then(() => new Promise((resolve, reject) => {
    const versionedSource = assetUrl(source);
    if (document.querySelector(`script[src="${versionedSource}"]`)) {
      resolve();
      return;
    }
    const script = document.createElement('script');
    script.src = versionedSource;
    script.onload = resolve;
    script.onerror = reject;
    document.head.appendChild(script);
  })), Promise.resolve()).catch((error) => {
    console.error('Unable to load the shared UI polish layer.', error);
  });
})();