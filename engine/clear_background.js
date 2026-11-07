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
 * Cost. Reading computed styles is the expensive part and a long chat has
 * tens of thousands of elements, so the whole document is never walked in
 * one go while the page is in use:
 *  - added nodes, and elements whose class/style/state attributes changed,
 *    are checked in requestAnimationFrame right before the frame is painted
 *    (no flash; the style recalculation is the one that frame needs anyway),
 *    within a per-frame time budget; leftovers continue in the next frame;
 *  - a catch-all walk of the page (for what no mutation reports: style
 *    sheets, descendant selectors, :has()) is requested with a trailing
 *    debounce (QUIET_MS after the last change, at most MAX_WAIT_MS after the
 *    first) and runs in slices of SLICE_MS per frame; a request arriving
 *    while one runs re-runs it once, later;
 *  - DOM changes inside an editable field or inside a known surface (typing
 *    in the composer, menus) can't touch the page background: ignored.
 * Only a change of the page colour itself (theme switch) re-clears the page
 * synchronously, in one task, so no frame shows it half-done.
 *
 * Marks are sticky: a cleared element's own colour can't be read any more,
 * so it stays cleared until the page colour changes.
 *
 * The engine style sheet is:
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
  const IGNORE = new Set(['SCRIPT', 'STYLE', 'LINK', 'META', 'BR', 'IMG', 'SVG', 'svg', 'math', 'VIDEO',
    'CANVAS', 'IFRAME', 'TEMPLATE', 'NOSCRIPT', 'INPUT', 'TEXTAREA', 'SELECT', 'OPTION']);
  /* checked themselves, not descended into (text runs, buttons) */
  const LEAF = new Set(['SPAN', 'A', 'B', 'I', 'EM', 'STRONG', 'CODE', 'PRE', 'P', 'LI', 'TD', 'TH',
    'LABEL', 'BUTTON', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'BLOCKQUOTE']);
  const CONTAINERS = new Set(['DIV', 'MAIN', 'SECTION', 'ARTICLE', 'ASIDE', 'NAV', 'HEADER', 'FOOTER', 'DIALOG', 'FORM']);
  const HTML = 'http://www.w3.org/1999/xhtml';
  /* attributes that restyle an element (none of them ours) */
  const WATCHED = ['class', 'style', 'hidden', 'open', 'data-state', 'data-theme', 'data-mode'];

  /* main-thread budgets (ms per frame) and catch-all debounce */
  const FRESH_MS = 8;        /* added / restyled elements, before their first paint */
  const SLICE_MS = 5;        /* catch-all walk */
  const LOAD_MS = 12;        /* either, while the page loads or hydrates */
  const QUIET_MS = 1500;     /* catch-all: this long after the last change... */
  const MAX_WAIT_MS = 5000;  /* ...but never later than this after the first */

  let on = false, listening = false, base = null, obs = null;
  /* Reading the base colour toggles an attribute on <html>, which restyles
   * the whole document: only do it when html/body or the style sheets may
   * have changed. */
  let baseDirty = true, rootChanged = false, lastBaseRead = -1e9;
  /* bumped whenever cached pseudo-element results may be stale (page
   * colour, style sheets, viewport size) */
  let gen = 0;
  /* Marks are collected while reading styles and written afterwards in one
   * go: interleaving would force a style recalculation per element. */
  let pending = [];
  const pend = (e, a) => pending.push(e, a);
  function flush() {
    for (let i = 0; i < pending.length; i += 2)
      pending[i].setAttribute(pending[i + 1], '');
    pending = [];
  }

  /* Elements last seen painted (surfaces) or not displayed: their subtree
   * was not walked, so it must be once that changes. A hint of what to walk
   * and where changes can't matter, never a reason to skip reading a style:
   * every catch-all walk re-reads them all. */
  const surfaces = new WeakSet(), undisplayed = new WeakSet();
  /* Pseudo-element styles are computed afresh on every read (the engine
   * doesn't cache them), the slowest check here: redone only when the
   * element's class/style or `gen` changed. */
  const pseudoSeen = new WeakMap();

  /* pending work: nodes reported by the observer (checked next frame), the
   * walk of new/restyled subtrees and the catch-all walk, both stacks of
   * (element, depth) pairs that survive across frames */
  let added = new Set(), changed = new Set();
  const fresh = [];
  let full = null, again = false;
  let raf = 0, until = 0, lastScan = -1e9;
  /* the per-frame loop while the document loads stops here even if some
   * resource never finishes (the load event or later changes restart it) */
  let loadDeadline = 0;
  let fullTimer = 0, firstRequest = 0;

  /* what this script costs the page (read by tests and benchmarks) */
  const stats = {
    fullScans: 0, syncRescans: 0, freshChecks: 0, visited: 0, busyMs: 0, maxFrameMs: 0,
    baseReads: 0, baseReadMs: 0, maxBaseReadMs: 0, observerMs: 0,
    reset() {
      this.fullScans = this.syncRescans = this.freshChecks = this.visited = 0;
      this.busyMs = this.maxFrameMs = this.baseReads = this.baseReadMs = this.maxBaseReadMs = this.observerMs = 0;
    },
  };

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

  /* Re-reads the page colour if it may have changed. Another colour (the
   * site switched theme or page) forgets every mark, made against the old
   * one. Returns whether that happened. */
  function refreshBase() {
    /* no opaque page colour (yet): look again at most every 500 ms, not on
     * every frame with DOM changes (each read restyles the document) */
    if (!baseDirty && (base || performance.now() - lastBaseRead < 500))
      return false;
    baseDirty = false;
    lastBaseRead = performance.now();
    const t0 = performance.now();
    const b = readBase();
    const ms = performance.now() - t0;
    stats.baseReads++;
    stats.baseReadMs += ms;
    stats.maxBaseReadMs = Math.max(stats.maxBaseReadMs, ms);
    const switched = !!base && (!b || !sameRgb(base, b));
    if (switched)
      for (const e of document.querySelectorAll(ALL))
        unmark(e);
    if (switched || !base)
      gen++;
    base = b;
    return switched;
  }

  const isLarge = (e) => {
    const r = e.getBoundingClientRect();
    return r.width * r.height >= innerWidth * innerHeight * 0.08;
  };

  function mark(e) {
    const t = e.tagName;
    pend(e, t === 'BUTTON' || t === 'A' || e.getAttribute('role') === 'button' ? ATTR_I : ATTR);
  }

  /* only large containers: full-page splashes are what this is for */
  function checkPseudo(e) {
    const sig = gen + '|' + e.getAttribute('class') + '|' + e.getAttribute('style');
    if (pseudoSeen.get(e) === sig || !isLarge(e))
      return;
    pseudoSeen.set(e, sig);
    for (const [ps, a] of PSEUDO) {
      if (e.hasAttribute(a))
        continue;
      const pcs = getComputedStyle(e, ps);
      if (pcs.content !== 'none' && near(rgba(pcs.backgroundColor), base))
        pend(e, a);
    }
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
    if (CONTAINERS.has(e.tagName))
      checkPseudo(e);
    return true;
  }

  /* Visits (element, depth) pairs popped from `st` until it is empty or the
   * clock passes `deadline`; the rest stays for the next slice. Children
   * are pushed in reverse, so the walk is in document order. A negative
   * depth checks the element and walks its subtree only if that was hidden
   * from the last walk (it was a surface or not displayed). Returns whether
   * the stack is empty. */
  function walk(st, deadline) {
    let n = 0;
    while (st.length) {
      if ((++n & 15) === 0 && performance.now() > deadline)
        break;
      const d = st.pop(), e = st.pop();
      /* removed since it was queued */
      if (!e.isConnected || IGNORE.has(e.tagName))
        continue;
      stats.visited++;
      const cs = getComputedStyle(e);
      const display = cs.display;
      const wasHidden = surfaces.has(e) || undisplayed.has(e);
      if (display === 'none') {
        undisplayed.add(e);
        continue;
      }
      undisplayed.delete(e);
      if (display !== 'contents' && !check(e, cs)) {
        surfaces.add(e); /* nothing inside a surface is page background */
        continue;
      }
      surfaces.delete(e);
      if (LEAF.has(e.tagName) || d > 40 || (d < 0 && !wasHidden))
        continue;
      const ch = e.children, next = d < 0 ? 1 : d + 1;
      for (let i = ch.length - 1; i >= 0; i--)
        st.push(ch[i], next);
    }
    flush();
    return st.length === 0;
  }

  /* does `e` sit on the page background (no painted ancestor surface)? */
  function onBase(e, memo) {
    const seen = [];
    let result = true;
    for (let p = e.parentElement; p && p !== document.body && p !== document.documentElement; p = p.parentElement) {
      if (memo.has(p)) {
        result = memo.get(p);
        break;
      }
      seen.push(p);
      if (cleared(p))
        continue;
      const c = rgba(getComputedStyle(p).backgroundColor);
      if (c && c[3] > 0.1) {
        result = false;
        break;
      }
    }
    for (const p of seen)
      memo.set(p, result);
    return result;
  }

  /* No style reads: is `e` (with `self`) or an ancestor of it a surface as
   * of the last walk, or editable? DOM changes there (typing in the
   * composer, menus) can't affect the page background. */
  function quiet(e, self, memo) {
    if (!e || e.nodeType !== 1)
      return false;
    if (e.isContentEditable)
      return true;
    const seen = [];
    let r = false;
    for (let p = self ? e : e.parentElement; p && p !== document.body; p = p.parentElement) {
      const m = memo.get(p);
      if (m !== undefined) {
        r = m;
        break;
      }
      seen.push(p);
      if (surfaces.has(p)) {
        r = true;
        break;
      }
    }
    for (const p of seen)
      memo.set(p, r);
    return r;
  }

  /* html/body restyled but the page colour is the same (a route or state
   * class): rules keyed on it may have given cleared elements another
   * colour, which their marks hide. Lift the marks and look again, in this
   * task (no frame without them); the marked elements are few. */
  function recheckMarked() {
    const marked = [...document.querySelectorAll(ALL)];
    if (!marked.length)
      return;
    for (const e of marked)
      unmark(e);
    gen++; /* pseudo-element marks were lifted too */
    const memo = new Map(), st = [];
    for (const e of marked)
      if (e.isConnected && onBase(e, memo))
        st.push(e, -1);
    walk(st, Infinity);
  }

  /* ---- catch-all walk ---- */

  function startFull(sync) {
    full = null;
    if (!on || !document.body)
      return;
    if (refreshBase())
      sync = true; /* new page colour: redo it all before the next paint */
    if (!base)
      return;
    stats.fullScans++;
    const st = [];
    const ch = document.body.children;
    for (let i = ch.length - 1; i >= 0; i--)
      st.push(ch[i], 0);
    if (sync) {
      stats.syncRescans++;
      walk(st, Infinity);
      finishFull();
    } else {
      full = st;
      schedule();
    }
  }

  function finishFull() {
    full = null;
    lastScan = performance.now();
    if (again) {
      again = false;
      requestFull(QUIET_MS);
    }
  }

  /* Trailing debounce: `ms` after the latest request, but at most
   * MAX_WAIT_MS after the oldest one still waiting. */
  function requestFull(ms) {
    if (!on)
      return;
    const now = performance.now();
    if (!firstRequest)
      firstRequest = now;
    clearTimeout(fullTimer);
    fullTimer = setTimeout(() => {
      fullTimer = 0;
      firstRequest = 0;
      if (full)
        again = true; /* the change may be behind the running walk */
      else
        startFull(false);
    }, Math.max(0, Math.min(ms, firstRequest + MAX_WAIT_MS - now)));
  }

  /* ---- per-frame work ---- */

  function schedule() {
    if (on && !raf)
      raf = requestAnimationFrame(frame);
  }

  function frame() {
    raf = 0;
    if (!on)
      return;
    const t0 = performance.now();
    const loading = document.readyState !== 'complete' || t0 < until;
    if (rootChanged) {
      /* html/body restyled: a theme switch changes the page colour */
      rootChanged = false;
      baseDirty = true;
      if (refreshBase()) {
        startFull(true);
      } else if (base) {
        recheckMarked();
        requestFull(100);
      }
    }
    if (added.size || changed.size) {
      if (!base) {
        if (!full)
          startFull(false); /* reads the page colour first */
      } else {
        const memo = new Map();
        /* new nodes: walked whole, unless inside another new node */
        for (const n of added)
          if (n.isConnected && !(n.parentElement && added.has(n.parentElement)) && onBase(n, memo))
            fresh.push(n, 0);
        for (const e of changed)
          if (e.isConnected && !added.has(e) && onBase(e, memo))
            fresh.push(e, -1);
        stats.freshChecks += added.size + changed.size;
      }
      added.clear();
      changed.clear();
    }
    const deadline = t0 + (loading ? LOAD_MS : FRESH_MS);
    if (!fresh.length || walk(fresh, deadline)) {
      if (!full && t0 < until && t0 - lastScan >= 300)
        startFull(false); /* load / hydration: keep up with restyles */
      if (full && walk(full, Math.min(deadline, performance.now() + (loading ? LOAD_MS : SLICE_MS))))
        finishFull();
    }
    const now = performance.now();
    stats.busyMs += now - t0;
    stats.maxFrameMs = Math.max(stats.maxFrameMs, now - t0);
    if (fresh.length || full || added.size || changed.size || rootChanged || now < until ||
        (document.readyState !== 'complete' && now < loadDeadline))
      schedule();
  }

  /* For the next `ms` milliseconds (load, hydration), walk the whole page
   * again at most every 300 ms. */
  function kick(ms) {
    until = Math.max(until, performance.now() + ms);
    schedule();
  }

  function onMutations(list) {
    if (!on)
      return;
    const t0 = performance.now();
    let catchAll = false, any = false;
    const memo = new Map();
    for (const m of list) {
      const t = m.target;
      if (m.type === 'attributes') {
        if (t === document.documentElement || t === document.body) {
          rootChanged = any = true;
          continue;
        }
        if (t.namespaceURI !== HTML || quiet(t, false, memo))
          continue;
        if (!cleared(t)) {
          changed.add(t);
          any = true;
        }
        /* a container's class or state can restyle its subtree through
         * descendant selectors (inline styles barely ever do) */
        if (m.attributeName !== 'style' && t.firstElementChild)
          catchAll = true;
        continue;
      }
      const inQuiet = quiet(t, true, memo);
      for (const n of m.addedNodes) {
        if (n.nodeType !== 1)
          continue;
        if (n.tagName === 'STYLE' || n.tagName === 'LINK') {
          /* a style sheet restyles the whole page, wherever it is */
          baseDirty = true;
          gen++;
          catchAll = true;
        } else if (!inQuiet) {
          added.add(n);
          any = true;
        }
      }
      if (inQuiet)
        continue;
      /* new layouts get restyled by classes and style sheets afterwards */
      catchAll = true;
    }
    if (catchAll)
      requestFull(QUIET_MS);
    if (any)
      schedule();
    stats.observerMs += performance.now() - t0;
  }

  /* re-check an element before the next paint (hover gone, transition end) */
  function recheck(e) {
    if (!on || !e || e.nodeType !== 1 || cleared(e) || quiet(e, false, new Map()))
      return;
    changed.add(e);
    schedule();
  }

  function listen() {
    if (listening)
      return;
    listening = true;
    /* style sheets arriving change colours without DOM mutations */
    document.addEventListener('load', (ev) => {
      if (on && ev.target && ev.target.tagName === 'LINK') {
        baseDirty = true;
        gen++;
        if (!full)
          startFull(false);
        kick(300);
      }
    }, true);
    document.addEventListener('DOMContentLoaded', () => {
      baseDirty = true;
      if (on && !full)
        startFull(false);
      kick(1000);
    });
    addEventListener('load', () => {
      baseDirty = true;
      if (on && !full)
        startFull(false);
      kick(3000); /* hydration usually restyles right after load */
    });
    addEventListener('pageshow', () => kick(1500));
    addEventListener('resize', () => {
      gen++;
      requestFull(300);
    });
    /* a button seen while hovered had its hover colour: look again */
    addEventListener('pointerout', (ev) => {
      const t = ev.target && ev.target.closest && ev.target.closest('button, a, [role=button]');
      if (t)
        recheck(t);
    }, {capture: true, passive: true});
    /* a colour read mid-transition: look again at its end */
    addEventListener('transitionend', (ev) => {
      if (ev.propertyName && ev.propertyName.startsWith('background'))
        recheck(ev.target);
    }, {capture: true, passive: true});
    addEventListener('animationend', (ev) => recheck(ev.target), {capture: true, passive: true});
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
      if (!on)
        return;
      rootChanged = true;
      schedule();
    });
  }

  window.__prajnaClear = {
    /* one complete synchronous pass (tests and benchmarks) */
    scan() {
      if (!on || !document.body)
        return;
      refreshBase();
      if (!base)
        return;
      const st = [];
      for (let i = document.body.children.length - 1; i >= 0; i--)
        st.push(document.body.children[i], 0);
      walk(st, Infinity);
      lastScan = performance.now();
    },
    stats,
    start() {
      if (on)
        return;
      on = true;
      listen();
      obs = new MutationObserver(onMutations);
      obs.observe(document, {childList: true, subtree: true, attributes: true, attributeFilter: WATCHED});
      baseDirty = true;
      loadDeadline = performance.now() + 15000;
      startFull(false);
      kick(1500);
    },
    stop() {
      on = false;
      if (obs)
        obs.disconnect();
      obs = null;
      if (raf)
        cancelAnimationFrame(raf);
      raf = 0;
      clearTimeout(fullTimer);
      fullTimer = firstRequest = 0;
      pending = [];
      added = new Set();
      changed = new Set();
      fresh.length = 0;
      full = null;
      again = rootChanged = false;
      until = 0;
      base = null;
      for (const e of document.querySelectorAll(ALL))
        unmark(e);
    },
  };
  window.__prajnaClear.start();
})();
