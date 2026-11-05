// Plane Radar Deck on open.spotify.com: reads the queue panel ("Queue" on the right; it
// must be open) and plays a song from it when the card asks. Reads only the panel's text
// and pictures and what the page tells the browser is playing.
'use strict';

(() => {
  // Everything that depends on Spotify's page, in one place (no language-bound labels).
  const SEL = {
    bar: '[data-testid="now-playing-bar"]',
    list: 'ul[role="treegrid"]',
    row: 'li[role="row"]',
    play: '[data-testid="play-button"]',
  };

  let lastSent = '';
  let timer = null;

  // The queue panel: the side panel (not the player bar) that holds the queue's lists.
  function panel() {
    for (const a of document.querySelectorAll('aside')) {
      if (a.matches(SEL.bar)) continue;
      if (a.querySelector(SEL.list + ' ' + SEL.row)) return a;
    }
    return null;
  }

  // "Now playing", then any songs added by hand, then the rest, in that order.
  function rows() {
    const p = panel();
    if (!p) return [];
    const out = [];
    for (const ul of p.querySelectorAll(SEL.list)) {
      for (const r of ul.querySelectorAll(SEL.row)) out.push(r);
    }
    return out;
  }

  function lines(el) {
    return (el.innerText || '').split('\n').map((s) => s.trim()).filter(Boolean);
  }

  function title(el) {
    return lines(el)[0] || '';
  }

  function artwork() {
    try {
      const a = navigator.mediaSession && navigator.mediaSession.metadata && navigator.mediaSession.metadata.artwork;
      if (a && a.length) {
        const size = (x) => parseInt(String(x.sizes || '').split('x')[0], 10) || 0;
        let best = a[0];
        for (const x of a) if (size(x) > size(best)) best = x;
        if (best.src && best.src.startsWith('https://')) return best.src;
      }
    } catch (_) { /* not readable */ }
    return '';
  }

  function read() {
    const items = rows().map((el) => {
      const l = lines(el);
      const img = el.querySelector('img');
      // The artists (or the show) are the row's links; a badge such as "E" sits in between.
      const links = [...el.querySelectorAll('a')].map((a) => (a.innerText || '').trim()).filter(Boolean);
      return {
        t: l[0] || '',
        a: links.length ? links.join(', ') : (l.length > 1 ? l[l.length - 1] : ''),
        d: '',
        img: img && img.src && img.src.startsWith('http') ? img.src : '',
      };
    }).filter((x) => x.t);
    let now = '';
    try { now = (navigator.mediaSession.metadata && navigator.mediaSession.metadata.title) || ''; } catch (_) { now = ''; }
    if (!now && items.length) now = items[0].t;
    return { t: 'queue', src: 'Spotify', items, cur: items.length ? 0 : -1, now, art: artwork() };
  }

  function push(force) {
    let q;
    try { q = read(); } catch (_) { return; }
    const key = JSON.stringify(q);
    if (!force && key === lastSent) return;
    lastSent = key;
    try { chrome.runtime.sendMessage(q).catch(() => {}); } catch (_) { /* extension reloaded */ }
  }

  function soon() {
    clearTimeout(timer);
    timer = setTimeout(() => push(false), 400);
  }

  function play(i, want) {
    const all = rows();
    let el = all[i];
    if (!el || (want && title(el) !== want)) {
      el = all.find((e) => title(e) === want) || null;  // the list moved meanwhile
    }
    if (!el) return;
    const btn = el.querySelector(SEL.play);
    if (btn) btn.click();
    setTimeout(() => push(true), 800);
  }

  chrome.runtime.onMessage.addListener((msg) => {
    if (!msg) return;
    if (msg.t === 'play') play(msg.i, msg.title);
    if (msg.t === 'refresh') push(true);
  });

  new MutationObserver(soon).observe(document.documentElement, { subtree: true, childList: true });
  setInterval(() => push(false), 3000);
  setInterval(() => push(true), 30000);  // also wakes the extension's link now and then
  push(true);
})();
