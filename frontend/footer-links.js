(() => {
  'use strict';

  const pageName = window.location.pathname.split('/').pop() || 'index.html';

  function restoreHomeFooter() {
    if (pageName !== 'index.html') return;

    const footer = document.querySelector('footer.footer');
    if (!footer) return;

    footer.classList.add('footer--rich');
    footer.innerHTML = `
      <div class="footer__brand">
        <strong>College Fantasy Football</strong>
        <span>Built for college fantasy managers.</span>
      </div>
      <nav class="footer__links" aria-label="Footer">
        <a href="https://github.com/Dogg555/CollegeFantasyFootball" target="_blank" rel="noopener noreferrer">GitHub</a>
        <a href="contact.html">Contact</a>
        <a href="privacy.html">Privacy</a>
        <a href="terms.html">Terms</a>
      </nav>
    `;
  }

  if (document.readyState === 'complete') {
    restoreHomeFooter();
  } else {
    window.addEventListener('load', restoreHomeFooter, { once: true });
  }
})();
