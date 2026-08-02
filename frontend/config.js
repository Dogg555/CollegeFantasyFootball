// Default same-origin API config for local development.
// Render prepends deployment-specific values before this file.
window.CFF_API_BASE = window.CFF_API_BASE || '/api';
window.CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO !== false;

(() => {
  const scripts = ['polish-core.js', 'polish-forms.js', 'polish-state.js'];

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
      theme.content = '#0d1116';
      document.head.appendChild(theme);
    }
  }

  ensureBranding();

  if (document.readyState === 'loading') {
    if (!document.querySelector('link[data-cff-polish]')) {
      document.write('<link rel="stylesheet" href="polish.css" data-cff-polish="true">');
    }
    if (!document.querySelector('link[href="alpha-ui.css"]')) {
      document.write('<link rel="stylesheet" href="alpha-ui.css" data-cff-modern="true">');
    }
    scripts.forEach((source) => {
      document.write(`<script src="${source}"><\/script>`);
    });
    return;
  }

  if (!document.querySelector('link[data-cff-polish]')) {
    const stylesheet = document.createElement('link');
    stylesheet.rel = 'stylesheet';
    stylesheet.href = 'polish.css';
    stylesheet.dataset.cffPolish = 'true';
    document.head.appendChild(stylesheet);
  }

  if (!document.querySelector('link[href="alpha-ui.css"]')) {
    const modern = document.createElement('link');
    modern.rel = 'stylesheet';
    modern.href = 'alpha-ui.css';
    modern.dataset.cffModern = 'true';
    document.head.appendChild(modern);
  }

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
