// Plane Radar Deck on music.youtube.com: reads the queue ("Sıradaki") of the player and
// plays a song from it when the card asks. Reads only the queue panel's text and pictures.
'use strict';

(() => {
  let lastSent = '';
  let timer = null;

  function text(el, sel) {
    const e = el.querySelector(sel);
    return e ? (e.getAttribute('title') || e.textContent || '').trim().replace(/\s+/g, ' ') : '';
  }

  function queueItems() {
    // The queue panel of the player page: plain items and the "automix" wrappers.
    const root = document.querySelector('ytmusic-player-queue #contents') ||
                 document.querySelector('ytmusic-player-queue');
    if (!root) return [];
    const out = [];
    for (const el of root.querySelectorAll('ytmusic-player-queue-item')) {
      // An automix wrapper holds the item twice (primary / counterpart): the visible one.
      if (el.closest('[hidden]')) continue;
      out.push(el);
    }
    return out;
  }

  // The cover in a large size: YouTube's picture servers make any size from the address.
  function hiRes(u) {
    if (!u || !u.startsWith('http')) return '';
    const v = u.match(/^https:\/\/i\d?\.ytimg\.com\/vi(?:_webp)?\/([^/]+)\//);
    if (v) return 'https://i.ytimg.com/vi/' + v[1] + '/maxresdefault.jpg';
    if (/googleusercontent\.com|ggpht\.com/.test(u)) {
      return u.replace(/=w\d+-h\d+[^?#]*$/, '=w1200-h1200-l90-rj').replace(/=s\d+[^?#]*$/, '=s1200');
    }
    return u;
  }

  function artwork() {
    try {
      const a = navigator.mediaSession && navigator.mediaSession.metadata && navigator.mediaSession.metadata.artwork;
      if (a && a.length) {
        let best = a[0];
        const size = (x) => parseInt(String(x.sizes || '').split('x')[0], 10) || 0;
        for (const x of a) if (size(x) > size(best)) best = x;
        if (best.src) return hiRes(best.src);
      }
    } catch (_) { /* not readable here: the player bar picture */ }
    const img = document.querySelector('ytmusic-player-bar img.image') ||
                document.querySelector('ytmusic-player-bar .thumbnail img');
    return img ? hiRes(img.src) : '';
  }

  function read() {
    const els = queueItems();
    let cur = -1;
    const items = els.map((el, i) => {
      if (el.hasAttribute('selected') || el.getAttribute('play-button-state') === 'playing' ||
          el.getAttribute('play-button-state') === 'paused') {
        if (cur < 0) cur = i;
      }
      const img = el.querySelector('img');
      return {
        t: text(el, '.song-title'),
        a: text(el, '.byline'),
        d: text(el, '.duration'),
        img: img && img.src && img.src.startsWith('http') ? img.src : '',
      };
    });
    const bar = document.querySelector('ytmusic-player-bar .title');
    return { t: 'queue', items, cur, now: bar ? bar.textContent.trim() : '', art: artwork() };
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

  function play(i, title) {
    const els = queueItems();
    let el = els[i];
    if (!el || (title && text(el, '.song-title') !== title)) {
      el = els.find((e) => text(e, '.song-title') === title) || el;  // the list moved meanwhile
    }
    if (!el) return;
    const btn = el.querySelector('ytmusic-play-button-renderer') || el.querySelector('.song-info') || el;
    btn.click();
    setTimeout(() => push(true), 800);
  }

  chrome.runtime.onMessage.addListener((msg) => {
    if (!msg) return;
    if (msg.t === 'play') play(msg.i, msg.title);
    if (msg.t === 'refresh') push(true);
  });

  new MutationObserver(soon).observe(document.documentElement, {
    subtree: true, childList: true, attributes: true,
    attributeFilter: ['selected', 'play-button-state'],
  });
  setInterval(() => push(false), 3000);
  setInterval(() => push(true), 30000);  // also wakes the extension's link now and then
  push(true);
})();
