/* Transparent page background (injected by chatai-webwpe at document start).
 *
 * Makes the page's own background see-through so the Plasma popup (blurred
 * by KWin) shows behind the chat, while keeping everything that sits on top
 * of it (composer, messages, menus, dialogs) as the site drew it.
 *
 * Rule: the page's base colour is the opaque background of <body> or <html>.
 * Large containers (>= 8% of the viewport) painted with that same colour are
 * the "page background" and are cleared; anything with another colour is a
 * surface and stays.
 *
 * No flashes: <html>/<body> are cleared by the engine's user style sheet from
 * the first frame; new containers are cleared in the MutationObserver
 * callback and, while the page is loading or changing, again in
 * requestAnimationFrame, which runs right before every frame is painted (a
 * site's style sheet can colour existing nodes without any mutation). The
 * engine style sheet is:
 *   html:not([data-chatai-probe]), html:not([data-chatai-probe]) > body,
 *   [data-chatai-clear], [data-chatai-clear-before]::before,
 *   [data-chatai-clear-after]::after { background-color: transparent !important }
 * data-chatai-probe briefly lifts the html/body rule to read their real
 * colour; cleared elements remember theirs in data-chatai-clear. */
(() => {
  if (window.__chataiClear) {
    window.__chataiClear.start();
    return;
  }
  const ATTR = 'data-chatai-clear';
  /* pseudo-elements (e.g. a full-page ::before splash) get their own marks */
  const PSEUDO = [['::before', 'data-chatai-clear-before'], ['::after', 'data-chatai-clear-after']];
  const ALL = '[' + ATTR + '],[data-chatai-clear-before],[data-chatai-clear-after]';
  const unmark = (e) => {
    e.removeAttribute(ATTR);
    for (const [, a] of PSEUDO)
      e.removeAttribute(a);
  };
  const PROBE = 'data-chatai-probe';
  const SKIP = new Set(['SPAN', 'A', 'B', 'I', 'EM', 'STRONG', 'CODE', 'BR', 'IMG', 'SVG', 'PATH',
    'SCRIPT', 'STYLE', 'LINK', 'META', 'INPUT', 'TEXTAREA', 'BUTTON', 'LABEL', 'P', 'LI', 'TD', 'TH']);
  let on = false, base = null, timer = 0, obs = null, rootObs = null;
  let raf = 0, until = 0;

  /* Any CSS colour (rgb, color(srgb ...), oklch, ...) -> [r, g, b, a] with
   * 0-255 channels and 0-1 alpha, via a 1x1 canvas. Cached per string. */
  const cache = new Map();
  let ctx = null;
  const rgba = (c) => {
    if (!c || c === 'transparent' || c === 'rgba(0, 0, 0, 0)')
      return null;
    if (cache.has(c))
      return cache.get(c);
    let v = null;
    const m = /^rgba?\(([^)]*)\)$/.exec(c);
    if (m) {
      const p = m[1].split(/[\s,/]+/).filter(Boolean).map(parseFloat);
      if (p.length >= 3)
        v = [p[0], p[1], p[2], p.length > 3 ? p[3] : 1];
    } else {
      ctx = ctx || document.createElement('canvas').getContext('2d', {willReadFrequently: true});
      ctx.clearRect(0, 0, 1, 1);
      ctx.fillStyle = '#000';
      ctx.fillStyle = c;
      ctx.fillRect(0, 0, 1, 1);
      const d = ctx.getImageData(0, 0, 1, 1).data;
      if (d[3])
        v = [d[0] * 255 / d[3], d[1] * 255 / d[3], d[2] * 255 / d[3], d[3] / 255];
    }
    if (cache.size > 200)
      cache.clear();
    cache.set(c, v);
    return v;
  };
  const near = (a, b) => a && b && a[3] > 0.85 &&
    Math.abs(a[0] - b[0]) <= 8 && Math.abs(a[1] - b[1]) <= 8 && Math.abs(a[2] - b[2]) <= 8;

  /* real colour of <body>/<html>, with the engine rule lifted for the read */
  function readBase() {
    const root = document.documentElement;
    if (!root)
      return null;
    root.setAttribute(PROBE, '');
    let found = null;
    for (const e of [document.body, root]) {
      const c = e && rgba(getComputedStyle(e).backgroundColor);
      if (c && c[3] > 0.85) {
        found = c;
        break;
      }
    }
    root.removeAttribute(PROBE);
    return found;
  }

  let minArea = 0;
  /* Clear `e` if it is a large base-coloured container, then descend into
   * its large children (small subtrees can't hold page background). */
  function visit(e, depth) {
    if (e.nodeType !== 1 || SKIP.has(e.tagName.toUpperCase()))
      return;
    const cs = getComputedStyle(e);
    if (cs.display === 'none')
      return;
    if (cs.display !== 'contents') {
      const r = e.getBoundingClientRect();
      if (r.width * r.height < minArea)
        return;
      if (!e.hasAttribute(ATTR) && near(rgba(cs.backgroundColor), base))
        e.setAttribute(ATTR, '');
      for (const [ps, a] of PSEUDO) {
        if (e.hasAttribute(a))
          continue;
        const pcs = getComputedStyle(e, ps);
        if (pcs.content !== 'none' && near(rgba(pcs.backgroundColor), base))
          e.setAttribute(a, '');
      }
    }
    if (depth < 30)
      for (const c of e.children)
        visit(c, depth + 1);
  }

  /* Cheap check for freshly added nodes (e.g. every streamed token): colour
   * first, so layout is only queried for base-coloured candidates; bounded
   * depth. Returns true if a container that might hold page background was
   * added, so the next frames get full scans. */
  const CONTAINERS = new Set(['DIV', 'MAIN', 'SECTION', 'ARTICLE', 'ASIDE', 'NAV', 'HEADER', 'FOOTER', 'DIALOG']);
  function visitAdded(e, depth) {
    if (e.nodeType !== 1 || SKIP.has(e.tagName.toUpperCase()))
      return false;
    const cs = getComputedStyle(e);
    if (cs.display === 'none')
      return false;
    const container = CONTAINERS.has(e.tagName.toUpperCase());
    let hit = near(rgba(cs.backgroundColor), base);
    if (container)
      for (const [ps, a] of PSEUDO) {
        const pcs = getComputedStyle(e, ps);
        if (pcs.content !== 'none' && near(rgba(pcs.backgroundColor), base))
          hit = true;
      }
    if (hit)
      visit(e, 0); /* size check + marks */
    let big = container && e.childElementCount > 2;
    if (depth < 6)
      for (const c of e.children)
        big = visitAdded(c, depth + 1) || big;
    return big;
  }

  function fullScan() {
    if (!on || !document.body)
      return;
    base = readBase();
    if (!base)
      return;
    minArea = innerWidth * innerHeight * 0.08;
    for (const c of document.body.children)
      visit(c, 0);
  }

  /* One pending catch-all scan at most (only the timer clears `timer`). */
  function later(ms) {
    if (!timer)
      timer = setTimeout(() => {
        timer = 0;
        fullScan();
      }, ms);
  }

  /* Theme switch: forget what was cleared and redo it in the same task, so
   * no frame shows the page half-cleared. */
  function rescan() {
    if (!on)
      return;
    for (const e of document.querySelectorAll(ALL))
      unmark(e);
    fullScan();
  }

  /* Scan before each painted frame for the next `ms` milliseconds. */
  function tick() {
    raf = 0;
    if (!on)
      return;
    fullScan();
    if (performance.now() < until || document.readyState !== 'complete')
      raf = requestAnimationFrame(tick);
  }
  function kick(ms) {
    until = Math.max(until, performance.now() + ms);
    if (on && !raf)
      raf = requestAnimationFrame(tick);
  }

  /* New nodes: handled right away (before the next paint). */
  function onMutations(list) {
    if (!on)
      return;
    if (!base) {
      fullScan();
      return;
    }
    minArea = innerWidth * innerHeight * 0.08;
    let big = false;
    for (const m of list)
      for (const n of m.addedNodes)
        if (n.nodeType === 1)
          big = visitAdded(n, 0) || big;
    /* a new layout may be restyled by classes in the next frames */
    if (big)
      kick(400);
    /* class/style driven changes on existing nodes: a rare catch-all */
    later(1500);
  }

  function watchRoot() {
    if (rootObs || !document.body)
      return;
    rootObs = new MutationObserver(rescan);
    for (const e of [document.documentElement, document.body])
      rootObs.observe(e, {attributes: true, attributeFilter: ['class', 'style', 'data-theme', 'data-mode']});
  }

  window.__chataiClear = {
    start() {
      if (on)
        return;
      on = true;
      obs = new MutationObserver((list) => {
        watchRoot();
        onMutations(list);
      });
      obs.observe(document, {childList: true, subtree: true});
      watchRoot();
      /* style sheets arriving change colours without DOM mutations */
      document.addEventListener('load', (ev) => {
        if (ev.target && ev.target.tagName === 'LINK')
          fullScan();
      }, true);
      document.addEventListener('DOMContentLoaded', fullScan);
      addEventListener('load', () => {
        fullScan();
        kick(3000); /* hydration usually restyles right after load */
      });
      addEventListener('pageshow', () => kick(1500));
      addEventListener('resize', () => {
        later(300);
      });
      matchMedia('(prefers-color-scheme: dark)').addEventListener('change', rescan);
      fullScan();
      kick(1500);
    },
    stop() {
      on = false;
      if (obs)
        obs.disconnect();
      if (rootObs)
        rootObs.disconnect();
      rootObs = null;
      for (const e of document.querySelectorAll(ALL))
        unmark(e);
    },
  };
  window.__chataiClear.start();
})();
