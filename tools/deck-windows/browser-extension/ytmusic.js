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
    return { t: 'queue', items, cur, now: bar ? bar.textContent.trim() : '' };
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
