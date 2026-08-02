// Default same-origin API config for local development.
// Render prepends deployment-specific values before this file.
window.CFF_API_BASE = window.CFF_API_BASE || '/api';
window.CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO !== false;
window.CFF_ALLOWED_LEAGUE_SIZES = Object.freeze([4, 6, 8, 10, 12, 14, 16]);

(() => {
  const scripts = ['polish-core.js', 'polish-forms.js', 'polish-state.js', 'beta-ui.js', 'league-nav.js', 'footer-links.js'];
  const leagueSizeSelectIds = ['league-size', 'settings-teams'];

  function ensureBranding() {
    if (!document.querySelector('link[rel~="icon"]')) {
      const icon = document.createElement('link');
      icon.rel = 'icon';
      icon.type = 'image/svg+xml';
      icon.href = 'assets/favicon.svg';
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
    if (document.querySelector(`link[href="${href}"]`)) return;
    document.write(`<link rel="stylesheet" href="${href}" ${attribute}>`);
  }

  function appendStylesheet(href, datasetKey) {
    if (document.querySelector(`link[href="${href}"]`)) return;
    const stylesheet = document.createElement('link');
    stylesheet.rel = 'stylesheet';
    stylesheet.href = href;
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
    scripts.forEach((source) => {
      document.write(`<script src="${source}"><\/script>`);
    });
    return;
  }

  ensureLeagueSizeOptions();
  appendStylesheet('polish.css', 'cffPolish');
  appendStylesheet('alpha-ui.css', 'cffModern');
  appendStylesheet('beta-ui.css', 'cffBeta');
  appendStylesheet('league-nav.css', 'cffLeagueNav');

  scripts.reduce((chain, source) => chain.then(() => new Promise((resolve, reject) => {
    if (document.querySelector(`script[src="${source}"]`)) {
      resolve();
      return;
    }
    const script = document.createElement('script');
    script.src = source;
    script.onload = resolve;
    script.onerror = reject;
    document.head.appendChild(script);
  })), Promise.resolve()).catch((error) => {
    console.error('Unable to load the shared UI polish layer.', error);
  });
})();
