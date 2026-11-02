/* Transparent page background (injected by chatai-webwpe).
 *
 * Makes the page's own background see-through so the Plasma popup (blurred
 * by KWin) shows behind the chat, while keeping everything that sits on top
 * of it (composer, messages, menus, dialogs) as the site drew it.
 *
 * Rule: the page's base colour is the opaque background of <body> or <html>.
 * Large containers (>= 8% of the viewport) painted with that same colour are
 * the "page background" and are cleared; anything with another colour is a
 * surface and stays. Small subtrees are skipped, so a scan touches only a few
 * dozen elements even on long conversations. */
(() => {
  if (window.__chataiClear) {
    window.__chataiClear.start();
    return;
  }
  const ATTR = 'data-chatai-clear';
  let on = false, timer = 0, obs = null, rootObs = null;

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
  /* the colour an element had before we cleared it */
  const bgOf = (e) => e.hasAttribute(ATTR) ? e.getAttribute(ATTR) : getComputedStyle(e).backgroundColor;

  function base() {
    for (const e of [document.body, document.documentElement]) {
      if (!e)
        continue;
      const c = rgba(bgOf(e));
      if (c && c[3] > 0.85)
        return c;
    }
    return null;
  }

  function mark(e) {
    if (!e.hasAttribute(ATTR))
      e.setAttribute(ATTR, getComputedStyle(e).backgroundColor);
  }

  function scan() {
    timer = 0;
    if (!on || !document.body)
      return;
    const b = base();
    if (!b)
      return; /* already transparent */
    mark(document.documentElement);
    mark(document.body);
    const min = innerWidth * innerHeight * 0.08;
    const walk = (e, depth) => {
      for (const c of e.children) {
        const cs = getComputedStyle(c);
        if (cs.display === 'contents') {
          walk(c, depth + 1);
          continue;
        }
        const r = c.getBoundingClientRect();
        if (r.width * r.height < min)
          continue;
        if (near(rgba(bgOf(c)), b))
          mark(c);
        if (depth < 30)
          walk(c, depth + 1);
      }
    };
    walk(document.body, 0);
  }

  const schedule = () => {
    if (on && !timer)
      timer = setTimeout(scan, 600);
  };
  function reset() {
    for (const e of document.querySelectorAll('[' + ATTR + ']'))
      e.removeAttribute(ATTR);
  }
  /* theme switch: the remembered colours are stale */
  const rescan = () => {
    reset();
    schedule();
  };

  window.__chataiClear = {
    start() {
      if (on)
        return;
      on = true;
      obs = new MutationObserver(schedule);
      obs.observe(document.documentElement, {childList: true, subtree: true});
      rootObs = new MutationObserver(rescan);
      for (const e of [document.documentElement, document.body].filter(Boolean))
        rootObs.observe(e, {attributes: true, attributeFilter: ['class', 'style', 'data-theme', 'data-mode']});
      addEventListener('resize', schedule);
      matchMedia('(prefers-color-scheme: dark)').addEventListener('change', rescan);
      scan();
    },
    stop() {
      on = false;
      if (obs)
        obs.disconnect();
      if (rootObs)
        rootObs.disconnect();
      reset();
    },
  };
  window.__chataiClear.start();
})();
