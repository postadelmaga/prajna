/* Transparent page background (injected by prajna-engine at document start).
 *
 * Makes the page's own background see-through so the Plasma popup (blurred
 * by KWin) shows behind the chat, while keeping everything that sits on top
 * of it (composer, messages, menus, dialogs) as the site drew it.
 *
 * Rule: the page's base colour is the opaque background of <body> or <html>.
 * An element painted with that same colour while sitting directly on the page
 * background is invisible in the site's own design, so clearing it changes
 * nothing but the backdrop: containers, sticky bars, "Share"-style buttons,
 * fades (gradients from transparent to the base colour). Anything with another
 * colour is a surface (composer, message bubbles, menus): it and everything
 * inside it stay as the site drew them, and the walk does not descend there.
 * Buttons and links keep their hover/pressed colours.
 *
 * No flashes: <html>/<body> are cleared by the engine's user style sheet from
 * the first frame; new containers are cleared in the MutationObserver
 * callback and, while the page is loading or changing, again in
 * requestAnimationFrame, which runs right before every frame is painted (a
 * site's style sheet can colour existing nodes without any mutation). The
 * engine style sheet is:
 *   html:not([data-prajna-probe]), html:not([data-prajna-probe]) > body,
 *   [data-prajna-clear], [data-prajna-clear-before]::before,
 *   [data-prajna-clear-after]::after { background-color: transparent !important }
 *   [data-prajna-clear-i]:not(:hover):not(:active):not(:focus-visible)
 *     { background-color: transparent !important }
 *   [data-prajna-clear-img] { background-image: none !important }
 * data-prajna-probe briefly lifts the html/body rule to read their real
 * colour; cleared elements remember theirs in data-prajna-clear. */
(() => {
  if (window.__prajnaClear) {
    window.__prajnaClear.start();
    return;
  }
  const ATTR = 'data-prajna-clear';
  /* pseudo-elements (e.g. a full-page ::before splash) get their own marks */
  const PSEUDO = [['::before', 'data-prajna-clear-before'], ['::after', 'data-prajna-clear-after']];
  const ATTR_I = 'data-prajna-clear-i';     /* interactive: keeps hover colour */
  const ATTR_IMG = 'data-prajna-clear-img';  /* base-coloured fade */
  const MARKS = [ATTR, ATTR_I, ATTR_IMG, 'data-prajna-clear-before', 'data-prajna-clear-after'];
  const ALL = MARKS.map(a => '[' + a + ']').join(',');
  const unmark = (e) => {
    for (const a of MARKS)
      e.removeAttribute(a);
  };
  const cleared = (e) => e.hasAttribute(ATTR) || e.hasAttribute(ATTR_I);
  const PROBE = 'data-prajna-probe';
  /* never painted page background */
  const IGNORE = new Set(['SCRIPT', 'STYLE', 'LINK', 'META', 'BR', 'IMG', 'SVG', 'PATH', 'VIDEO',
    'CANVAS', 'IFRAME', 'TEMPLATE', 'NOSCRIPT', 'INPUT', 'TEXTAREA', 'SELECT', 'OPTION']);
  /* checked themselves, not descended into (text runs, buttons) */
  const LEAF = new Set(['SPAN', 'A', 'B', 'I', 'EM', 'STRONG', 'CODE', 'PRE', 'P', 'LI', 'TD', 'TH',
    'LABEL', 'BUTTON', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'BLOCKQUOTE']);
  const CONTAINERS = new Set(['DIV', 'MAIN', 'SECTION', 'ARTICLE', 'ASIDE', 'NAV', 'HEADER', 'FOOTER', 'DIALOG', 'FORM']);
  let on = false, base = null, timer = 0, obs = null, rootObs = null;
  /* Reading the base colour toggles an attribute on <html>, which restyles
   * the whole document: only do it when html/body or the style sheets may
   * have changed. */
  let baseDirty = true;
  /* Marks are collected while reading styles and written afterwards in one
   * go: interleaving would force a style recalculation per element. */
  let pending = [];
  const pend = (e, a) => pending.push(e, a);
  function flush() {
    for (let i = 0; i < pending.length; i += 2)
      pending[i].setAttribute(pending[i + 1], '');
    pending = [];
  }
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
  const sameRgb = (a, b) => a && b &&
    Math.abs(a[0] - b[0]) <= 8 && Math.abs(a[1] - b[1]) <= 8 && Math.abs(a[2] - b[2]) <= 8;
  const near = (a, b) => a && a[3] > 0.85 && sameRgb(a, b);

  /* a gradient made only of the base colour (any alpha) and transparency */
  const COLOR_TOKEN = /(?:rgba?|hsla?|color|oklch|oklab|lab|lch)\([^)]*\)|#[0-9a-f]{3,8}\b|transparent/gi;
  function baseFade(img) {
    if (!img || img === 'none' || !img.includes('gradient(') || img.includes('url('))
      return false;
    const stops = img.match(COLOR_TOKEN);
    if (!stops)
      return false;
    return stops.every(t => {
      const c = rgba(t);
      return !c || c[3] < 0.05 || sameRgb(c, base);
    });
  }

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

  const isLarge = (e) => {
    const r = e.getBoundingClientRect();
    return r.width * r.height >= innerWidth * innerHeight * 0.08;
  };

  function mark(e) {
    const t = e.tagName;
    pend(e, t === 'BUTTON' || t === 'A' || e.getAttribute('role') === 'button' ? ATTR_I : ATTR);
  }

  /* Clears `e` if it is base-coloured on the page background. Returns
   * whether its children still sit on the page background. */
  function check(e, cs) {
    if (cleared(e))
      return true;
    const c = rgba(cs.backgroundColor);
    if (near(c, base))
      mark(e);
    else if (c && c[3] > 0.1)
      return false; /* a surface */
    if (!e.hasAttribute(ATTR_IMG) && baseFade(cs.backgroundImage))
      pend(e, ATTR_IMG);
    /* pseudo-element styles aren't cached by the engine: only look at
     * large containers (full-page splashes are what this is for) */
    if (CONTAINERS.has(e.tagName) && isLarge(e))
      for (const [ps, a] of PSEUDO) {
        if (e.hasAttribute(a))
          continue;
        const pcs = getComputedStyle(e, ps);
        if (pcs.content !== 'none' && near(rgba(pcs.backgroundColor), base))
          pend(e, a);
      }
    return true;
  }

  function visit(e, depth) {
    if (e.nodeType !== 1 || IGNORE.has(e.tagName))
      return;
    const cs = getComputedStyle(e);
    if (cs.display === 'none')
      return;
    if (cs.display !== 'contents' && !check(e, cs))
      return; /* inside a surface: nothing there is page background */
    if (LEAF.has(e.tagName) || depth > 40)
      return;
    for (const c of e.children)
      visit(c, depth + 1);
  }

  /* does `e` sit on the page background (no painted ancestor surface)? */
  function onBase(e) {
    for (let p = e.parentElement; p && p !== document.body && p !== document.documentElement; p = p.parentElement) {
      if (cleared(p))
        continue;
      const c = rgba(getComputedStyle(p).backgroundColor);
      if (c && c[3] > 0.1)
        return false;
    }
    return true;
  }

  function fullScan() {
    if (!on || !document.body)
      return;
    if (baseDirty || !base) {
      const b = readBase();
      baseDirty = false;
      /* the site switched background (e.g. home -> conversation): restart */
      if (base && (!b || !sameRgb(base, b)))
        for (const e of document.querySelectorAll(ALL))
          unmark(e);
      base = b;
    }
    if (!base)
      return;
    for (const c of document.body.children)
      visit(c, 0);
    flush();
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
    baseDirty = true;
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
    let big = false;
    for (const m of list)
      for (const n of m.addedNodes) {
        if (n.nodeType !== 1 || !n.isConnected)
          continue;
        if (n.tagName === 'STYLE' || n.tagName === 'LINK')
          baseDirty = true;
        if (onBase(n))
          visit(n, 0);
        if (CONTAINERS.has(n.tagName) && n.childElementCount > 2)
          big = true;
      }
    flush();
    if (baseDirty)
      later(50);
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

  window.__prajnaClear = {
    /* one full pass (also used by tests and benchmarks) */
    scan: () => fullScan(),
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
        if (ev.target && ev.target.tagName === 'LINK') {
          baseDirty = true;
          fullScan();
        }
      }, true);
      document.addEventListener('DOMContentLoaded', () => {
        baseDirty = true;
        fullScan();
      });
      addEventListener('load', () => {
        baseDirty = true;
        fullScan();
        kick(3000); /* hydration usually restyles right after load */
      });
      addEventListener('pageshow', () => kick(1500));
      addEventListener('resize', () => later(300));
      /* a button seen while hovered had its hover colour: look again */
      addEventListener('pointerout', (ev) => {
        const t = ev.target && ev.target.closest && ev.target.closest('button, a, [role=button]');
        if (t && !cleared(t))
          later(250);
      }, {capture: true, passive: true});
      matchMedia('(prefers-color-scheme: dark)').addEventListener('change', rescan);
      baseDirty = true;
      fullScan();
      kick(1500);
    },
    stop() {
      on = false;
      pending = [];
      base = null;
      if (obs)
        obs.disconnect();
      if (rootObs)
        rootObs.disconnect();
      rootObs = null;
      for (const e of document.querySelectorAll(ALL))
        unmark(e);
    },
  };
  window.__prajnaClear.start();
})();
