const GUARD_FLAG = '__hvideoGlobalInteractionGuardsInstalled__';

function preventDefault(event) {
  event.preventDefault();
}

export function installGlobalInteractionGuards() {
  if (typeof document === 'undefined') return;
  if (document[GUARD_FLAG]) return;
  document[GUARD_FLAG] = true;

  const root = document.documentElement;
  if (root) {
    root.style.webkitTouchCallout = 'none';
    root.style.webkitUserSelect = 'none';
    root.style.userSelect = 'none';
    root.style.webkitTapHighlightColor = 'transparent';
  }

  document.addEventListener('contextmenu', preventDefault, true);
  document.addEventListener('selectstart', preventDefault, true);
}

installGlobalInteractionGuards();
