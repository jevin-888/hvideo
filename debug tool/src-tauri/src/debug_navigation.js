(() => {
  'use strict';
  // This bridge only supplies navigation chrome inside the player's embedded
  // document. It never reads tokens, credentials, or device API responses.
  if (window.parent === window || location.protocol !== 'http:') return;
  const HOME = 'hvideo-debug-home';
  let enabled = false;
  let observer;

  function notify(type) {
    window.parent.postMessage({ type }, '*');
  }

  function install() {
    const title = document.getElementById('app-title');
    const container = title?.closest('.header-left');
    if (!container) return false;
    if (!document.getElementById('hvideo-debug-home-button')) {
      const style = document.createElement('style');
      style.textContent = `
        .header-left.hvideo-debug-title-row { display:flex; align-items:center; gap:12px; }
        #hvideo-debug-home-button { display:grid; place-items:center; flex:0 0 32px;
          width:32px; height:32px; padding:0; margin:0; border:1px solid transparent;
          border-radius:6px; background:transparent; color:#c3c4d7; cursor:pointer;
          transition:background .15s,color .15s,border-color .15s; }
        #hvideo-debug-home-button:hover { background:rgba(124,114,232,.16); color:#fff;
          border-color:rgba(151,141,246,.22); }
        #hvideo-debug-home-button:focus-visible { outline:2px solid #a69af8; outline-offset:2px; }
        #hvideo-debug-home-button svg { display:block; width:20px; height:20px; }
      `;
      (document.head || document.documentElement).append(style);
      const button = document.createElement('button');
      button.id = 'hvideo-debug-home-button';
      button.type = 'button';
      button.title = '返回设备列表';
      button.setAttribute('aria-label', '返回设备列表');
      button.innerHTML = '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="m3 10 9-7 9 7v10a1 1 0 0 1-1 1h-5v-7H9v7H4a1 1 0 0 1-1-1z"/></svg>';
      button.addEventListener('click', () => notify(HOME));
      container.classList.add('hvideo-debug-title-row');
      container.insertBefore(button, title);
    }
    notify('hvideo-debug-home-ready');
    return true;
  }

  window.addEventListener('message', (event) => {
    if (event.source !== window.parent || event.data?.type !== 'hvideo-debug-navigation-attach') return;
    if (enabled) {
      if (document.getElementById('hvideo-debug-home-button')) notify('hvideo-debug-home-ready');
      return;
    }
    enabled = true;
    if (install()) return;
    observer = new MutationObserver(() => { if (install()) observer.disconnect(); });
    observer.observe(document, { childList:true, subtree:true });
  });
  notify('hvideo-debug-navigation-request');
})();
