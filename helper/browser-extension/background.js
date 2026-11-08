// Plane Radar Deck: the link between this browser and the Plane Radar Deck program on
// this computer (ws://127.0.0.1:47822, only this computer). The program asks it to open
// a site in the tab that already shows it, and to play a song from a music queue; the
// YouTube Music and Spotify pages (ytmusic.js, spotify.js) send their queues through here,
// each tagged with its tab so the program can show the one that plays.
'use strict';

const LINK = 'ws://127.0.0.1:47822/deck';
let ws = null;
let retryTimer = null;
const queues = new Map();  // tab id -> newest queue message from that tab
let lastTab = null;        // the tab that sent last (for an older program without tab ids)

function brand() {
  const b = (navigator.userAgentData && navigator.userAgentData.brands) || [];
  const names = b.map((x) => x.brand);
  for (const n of ['Microsoft Edge', 'Brave', 'Opera', 'Vivaldi', 'Google Chrome']) {
    if (names.some((x) => x === n || x.startsWith(n + ' '))) return n;  // "Opera GX" is Opera
  }
  return 'Chromium';
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function connect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  try {
    ws = new WebSocket(LINK);
  } catch (e) {
    ws = null;
    schedule();
    return;
  }
  ws.onopen = () => {
    send({ t: 'hello', v: chrome.runtime.getManifest().version, brand: brand() });
    for (const q of queues.values()) send(q);
  };
  ws.onmessage = (e) => {
    let msg;
    try { msg = JSON.parse(e.data); } catch (_) { return; }
    handle(msg).catch(() => {});
  };
  ws.onclose = () => { ws = null; schedule(); };
  ws.onerror = () => {};
}

function schedule() {
  clearTimeout(retryTimer);
  retryTimer = setTimeout(connect, 5000);
}

function hostOf(url) {
  try { return new URL(url).host.replace(/^www\./, ''); } catch (_) { return ''; }
}

async function handle(msg) {
  if (msg.t === 'ping') {
    send({ t: 'pong' });
  } else if (msg.t === 'open') {
    // The tab that already shows this site: to the front. None: a new tab.
    const host = hostOf(msg.url);
    const tabs = await chrome.tabs.query({});
    const hit = host && tabs.find((t) => t.url && hostOf(t.url) === host);
    if (hit) {
      await chrome.tabs.update(hit.id, { active: true });
      const w = await chrome.windows.get(hit.windowId);
      await chrome.windows.update(hit.windowId, w.state === 'minimized' ? { focused: true, state: 'normal' } : { focused: true });
      send({ t: 'opened', id: msg.id, reused: true });
    } else {
      let win = null;
      try { win = await chrome.windows.getLastFocused({ windowTypes: ['normal'] }); } catch (_) { win = null; }
      if (win && win.id !== undefined) {
        await chrome.tabs.create({ url: msg.url, windowId: win.id, active: true });
        await chrome.windows.update(win.id, win.state === 'minimized' ? { focused: true, state: 'normal' } : { focused: true });
      } else {
        await chrome.windows.create({ url: msg.url, focused: true });
      }
      send({ t: 'opened', id: msg.id, reused: false });
    }
  } else if (msg.t === 'play') {
    const tab = Number.isInteger(msg.tab) ? msg.tab : lastTab;
    if (tab !== null) {
      chrome.tabs.sendMessage(tab, { t: 'play', i: msg.i, title: msg.title }).catch(() => {});
    }
  } else if (msg.t === 'refresh') {
    const tabs = Number.isInteger(msg.tab) ? [msg.tab] : [...queues.keys()];
    for (const tab of tabs) chrome.tabs.sendMessage(tab, { t: 'refresh' }).catch(() => {});
  }
}

chrome.runtime.onMessage.addListener((msg, sender) => {
  if (msg && msg.t === 'queue' && sender.tab) {
    const q = Object.assign({}, msg, { tab: sender.tab.id });
    queues.set(sender.tab.id, q);
    lastTab = sender.tab.id;
    connect();
    send(q);
  }
});

chrome.tabs.onRemoved.addListener((id) => {
  if (queues.has(id)) {
    queues.delete(id);
    if (lastTab === id) lastTab = null;
    send({ t: 'queue', items: [], cur: -1, tab: id });
  }
});

// Stay in touch: started with the browser, woken every 30 s while the program is away.
chrome.runtime.onStartup.addListener(connect);
chrome.runtime.onInstalled.addListener(connect);
chrome.alarms.create('deck-link', { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener(connect);
connect();
