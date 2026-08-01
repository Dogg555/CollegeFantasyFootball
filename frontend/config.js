// Default same-origin API config for local development.
// Render prepends deployment-specific values before this file.
window.CFF_API_BASE = window.CFF_API_BASE || '/api';
window.CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO !== false;

(() => {
  const scripts = ['polish-core.js', 'polish-forms.js', 'polish-state.js'];

  if (document.readyState === 'loading') {
    document.write('<link rel="stylesheet" href="polish.css" data-cff-polish="true">');
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
