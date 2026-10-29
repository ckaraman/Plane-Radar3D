// Map and terrain data for the card, shared by the setup page (web/index.html) and the
// Radar page of Plane Radar Deck (helper/web/index.html; the builds copy this file there).
// A plain script: its names are globals that the two pages' own scripts use.
'use strict';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ------------------------------------------------------------------ map
// Coast lines and main roads around the chosen location, from OpenStreetMap.
// Binary format and limits: src/core/services/map_store.h in the firmware.
// Device range presets: label km (shown on ring 3) -> outer ring = label x 4/3. The map must also fill
// the screen beyond the outer ring (the round screen reaches ~1.12 x the outer ring), so it is
// downloaded to 1.13 x the outer ring of the widest range the person picks.
const MAP_RANGES = { 5: 5 * 4 / 3, 10: 10 * 4 / 3, 15: 15 * 4 / 3, 17: 17 * 4 / 3 };
const mapRadiusFor = (label) => MAP_RANGES[label] ? Math.ceil(MAP_RANGES[label] * 1.13 * 10) / 10 : 0;
const MAP_MAX_KM = 26, MAP_MAX_POINTS = 3000, MAP_MAX_RING_POINTS = 2500, MAP_MAX_BYTES = 31000, KM_PER_DEG = 111;
// Public Overpass servers are often busy or rate-limited, so several mirrors are tried in rounds.
const OVERPASS = ['https://overpass-api.de/api/interpreter', 'https://overpass.kumi.systems/api/interpreter',
                  'https://overpass.private.coffee/api/interpreter', 'https://maps.mail.ru/osm/tools/overpass/api/interpreter'];
const overpassCache = new Map();       // survives retries on the same page (same place and size)

function crc32(bytes) {
  let table = crc32.table;
  if (!table) {
    table = crc32.table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1); table[n] = c >>> 0; }
  }
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

// Sends the query to all mirrors at once and takes the first good answer (the others
// are cancelled): a busy server no longer costs a 30 s wait before the next is tried.
async function overpassRace(q, timeoutMs, reasons) {
  const ctls = OVERPASS.map(() => new AbortController());
  const timer = setTimeout(() => ctls.forEach((c) => c.abort()), timeoutMs);
  try {
    return await Promise.any(OVERPASS.map(async (url, i) => {
      const host = new URL(url).hostname;
      try {
        const r = await fetch(url, { method: 'POST', body: 'data=' + encodeURIComponent(q), signal: ctls[i].signal,
                                     headers: { 'Content-Type': 'application/x-www-form-urlencoded' } });
        if (!r.ok) throw new Error(r.status === 429 ? 'sunucu meşgul (429)' : 'HTTP ' + r.status);
        const j = JSON.parse(await r.text());                // an HTML error page throws here
        if (!j || !Array.isArray(j.elements)) throw new Error('geçersiz yanıt');
        if (j.remark && /runtime error|timed out|out of memory/i.test(j.remark)) throw new Error('sunucu meşgul');
        ctls.forEach((c, k) => { if (k !== i) c.abort(); });
        return j.elements;
      } catch (e) {
        if (reasons) reasons.push(host + ': ' + (e && e.name === 'AbortError' ? 'zaman aşımı' : (e && e.message) || 'bağlanılamadı'));
        throw e;
      }
    }));
  } finally { clearTimeout(timer); }
}

async function fetchOverpass(lat, lon, radiusKm, onStatus) {
  const key = lat.toFixed(4) + ',' + lon.toFixed(4) + ',' + radiusKm;
  if (overpassCache.has(key)) return overpassCache.get(key);
  const fetchKm = radiusKm * 1.25;               // a little wider than the circle so its edge is cut exactly
  const dLat = fetchKm / KM_PER_DEG, dLon = fetchKm / (KM_PER_DEG * Math.cos(lat * Math.PI / 180));
  const bbox = [lat - dLat, lon - dLon, lat + dLat, lon + dLon].map((v) => v.toFixed(5)).join(',');
  const notRiver = '["water"!~"^(river|riverbank|canal|stream|ditch|drain|wastewater|pool|fountain|moat|basin)$"]';
  const q = '[out:json][timeout:40];(way["natural"="coastline"](' + bbox + ');' +
    'way["highway"~"^(motorway|trunk)$"](' + bbox + ');' +
    'way["natural"="water"]' + notRiver + '(' + bbox + ');' +
    'relation["natural"="water"]' + notRiver + '(' + bbox + ');' +
    'way["landuse"="reservoir"](' + bbox + '););out geom(' + bbox + ');';
  const reasons = [];
  for (let round = 0; round < 2; round++) {
    if (onStatus) onStatus('Harita sunucularına bağlanılıyor' + (round ? ' (yeniden deneniyor)' : '') + '…');
    try {
      const els = await overpassRace(q, 35000, reasons);
      overpassCache.set(key, els);
      return els;
    } catch (_) { /* all mirrors failed this round */ }
    if (round < 1) await sleep(2000);
  }
  const err = new Error('network'); err.reasons = reasons;
  throw err;
}

// ---- places for the 5" board: airports (IATA code + runways), landmarks, towns.
// Map blob entries (firmware radar3d.cpp loadPois): type 4 = runway centre line
// (2 points); type 5 = place: word 0 position, word 1 [kind, name length, 0, 0],
// then the UTF-8 name padded to 4 bytes. Older firmware skips both types.
const AIRPORT_KM = 60;   // airports are shown at every range, beyond the map's area too
const bboxFor = (lat, lon, km) => {
  const dLat = km / KM_PER_DEG, dLon = km / (KM_PER_DEG * Math.cos(lat * Math.PI / 180));
  return '(' + [lat - dLat, lon - dLon, lat + dLat, lon + dLon].map((v) => v.toFixed(5)).join(',') + ')';
};
async function fetchPlaces(lat, lon, radiusKm) {
  const b = bboxFor(lat, lon, radiusKm), ab = bboxFor(lat, lon, Math.max(radiusKm, AIRPORT_KM));
  const q = '[out:json][timeout:25];way["aeroway"="runway"]' + ab + ';out geom;(' +
    'nwr["aeroway"="aerodrome"]["iata"]' + ab + ';' +
    'nwr["tourism"="attraction"]["wikidata"]["name"]' + b + ';' +
    'nwr["man_made"~"^(tower|lighthouse)$"]["wikidata"]["name"]' + b + ';' +
    'nwr["leisure"="stadium"]["wikidata"]["name"]' + b + ';' +
    'nwr["historic"~"^(castle|monument|fort|palace)$"]["wikidata"]["name"]' + b + ';' +
    'nwr["tourism"~"^(zoo|museum|theme_park)$"]["wikidata"]["name"]' + b + ';' +
    'nwr["leisure"="park"]["wikipedia"]["name"]' + b + ';' +
    'nwr["amenity"="place_of_worship"]["wikipedia"]["name"]' + b + ';' +
    'way["bridge"="yes"]["highway"~"^(motorway|trunk)$"]["wikidata"]["name"]' + b + ';' +
    ');out center tags;';
  for (let round = 0; round < 2; round++) {
    try { return await overpassRace(q, 30000, null); } catch (_) { /* busy: once more */ }
  }
  return null;   // places are optional
}

// Landmark symbol on the 5" map: 1 stadium, 2 zoo, 3 tower, 4 castle / historic,
// 5 bridge, 6 museum, 7 park, 8 place of worship, 0 other (plain diamond).
function landmarkCategory(t) {
  if (t.leisure === 'stadium') return 1;
  if (t.tourism === 'zoo') return 2;
  if (t.man_made === 'tower' || t.man_made === 'lighthouse') return 3;
  if (t.historic) return 4;
  if (t.bridge) return 5;
  if (t.tourism === 'museum') return 6;
  if (t.leisure === 'park' || t.tourism === 'theme_park') return 7;
  if (t.amenity === 'place_of_worship') return 8;
  return 0;
}

function buildPlaceEntries(elements, lat, lon, radiusKm) {
  const cosL = Math.cos(lat * Math.PI / 180);
  const xy = (la, lo) => [(lo - lon) * KM_PER_DEG * cosL, (la - lat) * KM_PER_DEG];
  const inside = ([x, y]) => x * x + y * y <= radiusKm * radiusKm;
  const insideAp = ([x, y]) => x * x + y * y <= Math.max(radiusKm, AIRPORT_KM) ** 2;
  const pos = (el) => el.type === 'node' ? xy(el.lat, el.lon) : (el.center ? xy(el.center.lat, el.center.lon) : null);
  const runways = [], airports = [], landmarks = [], towns = [];
  for (const el of elements) {
    const t = el.tags || {};
    if (t.aeroway === 'runway' && el.geometry && el.geometry.length >= 2) {
      const a = xy(el.geometry[0].lat, el.geometry[0].lon), z = xy(el.geometry[el.geometry.length - 1].lat, el.geometry[el.geometry.length - 1].lon);
      if (Math.hypot(z[0] - a[0], z[1] - a[1]) >= 0.8 && (insideAp(a) || insideAp(z))) runways.push([a, z]);
      continue;
    }
    const p = pos(el);
    if (!p) continue;
    if (t.aeroway === 'aerodrome') {
      if (/^[A-Z]{3}$/.test(t.iata || '') && insideAp(p)) airports.push({ p, name: t.iata, kind: 0 });
      continue;
    }
    if (!inside(p)) continue;
    else if (t.place) towns.push({ p, name: t.name, kind: 2, rank: t.place === 'city' ? 0 : t.place === 'town' ? 1 : 2 });
    else if (t.name) {
      const cat = landmarkCategory(t);
      // Places with their own symbol first, then the better known ones.
      if (cat) landmarks.push({ p, name: t.name, kind: 1, cat, rank: t.wikipedia ? 0 : 1 });   // no symbol: left out
    }
  }
  // Spread out: a place closer than `gap` km to one already taken is skipped.
  const pick = (list, max, gap, taken) => {
    const out = [];
    for (const it of list) {
      if (out.length >= max) break;
      if (taken.concat(out).some((o) => Math.hypot(o.p[0] - it.p[0], o.p[1] - it.p[1]) < gap)) continue;
      if (Math.hypot(it.p[0], it.p[1]) < 0.6) continue;     // home has its own marker
      out.push(it);
    }
    return out;
  };
  const near = (a, b) => Math.hypot(a.p[0], a.p[1]) - Math.hypot(b.p[0], b.p[1]);
  const ap = pick(airports.sort(near), 4, 2, []);
  // Landmarks: one of each kind first, in this order of kinds (bridge, tower, stadium,
  // place of worship, historic, museum, zoo, park), then a second of each, and so on,
  // so two stadiums never push out the only tower.
  const CAT_ORDER = [5, 3, 1, 8, 4, 6, 2, 7];
  const byCat = CAT_ORDER.map((c) => landmarks.filter((l) => l.cat === c).sort((a, b) => a.rank - b.rank || near(a, b)));
  const lm = [];
  const gap = radiusKm * 0.12;
  for (let round = 0; lm.length < 6 && byCat.some((l) => l.length); round++) {
    for (const list of byCat) {
      if (lm.length >= 6) break;
      while (list.length) {   // the best one of this kind that is not too close to one taken
        const it = list.shift();
        if (Math.hypot(it.p[0], it.p[1]) < 0.6) continue;
        if (ap.concat(lm).some((o) => Math.hypot(o.p[0] - it.p[0], o.p[1] - it.p[1]) < gap)) continue;
        lm.push(it);
        break;
      }
    }
  }
  const tw = [];   // town / district names: left out, they crowded the screen
  const enc = new TextEncoder();
  const entries = [];
  const q = (v) => Math.max(-32767, Math.min(32767, Math.round(v * 100)));   // 10 m units
  // Runways of the airports kept (within 5 km of one), nearest airports first.
  const rwys = runways.filter(([a, z]) => ap.some((o) => Math.hypot(o.p[0] - (a[0] + z[0]) / 2, o.p[1] - (a[1] + z[1]) / 2) < 5));
  for (const [a, z] of rwys.slice(0, 16)) {
    const raw = new Uint8Array(8), dv = new DataView(raw.buffer);
    dv.setInt16(0, q(a[0]), true); dv.setInt16(2, q(a[1]), true); dv.setInt16(4, q(z[0]), true); dv.setInt16(6, q(z[1]), true);
    entries.push({ type: 4, n: 2, raw });
  }
  for (const it of ap.concat(lm, tw)) {
    let name = enc.encode(it.name);
    if (name.length > 23) {                                   // cut on a character boundary
      let s = it.name;
      while (enc.encode(s + '.').length > 23) s = s.slice(0, -1);
      name = enc.encode(s.trimEnd() + '.');
    }
    const words = 2 + Math.ceil(name.length / 4);
    const raw = new Uint8Array(words * 4), dv = new DataView(raw.buffer);
    dv.setInt16(0, q(it.p[0]), true); dv.setInt16(2, q(it.p[1]), true);
    raw[4] = it.kind; raw[5] = name.length; raw[6] = it.cat || 0;
    raw.set(name, 8);
    entries.push({ type: 5, n: words, raw });
  }
  return entries;
}

// Built-up areas for the 5" map: OSM residential / commercial / industrial land use,
// only their bounding boxes, rasterised into a coarse grid of how much of each cell
// is town (0..15). Map entry type 6: word 0 = [N, 0, radius x10 (u16)], then N*N
// nibbles, low nibble first, rows south to north, cells west to east.
// Forest and woodland (map entry type 7, same grid as type 6): the polygons are
// rasterised properly (even-odd, 4x4 samples per cell), forests are large and their
// bounding boxes would paint whole valleys green.
// Built-up land use (bounding boxes only) and forests (full outlines) in one request:
// Overpass allows each address only a couple of requests at a time, and separate
// requests next to the map's were being turned away.
async function fetchCover(lat, lon, radiusKm) {
  const b = bboxFor(lat, lon, radiusKm);
  const q = '[out:json][timeout:50];way["landuse"~"^(residential|commercial|industrial|retail)$"]' + b + ';out ids bb;' +
    '(way["landuse"="forest"]' + b + ';way["natural"="wood"]' + b +
    ';relation["landuse"="forest"]' + b + ';relation["natural"="wood"]' + b + ';);out skel geom qt;';
  for (let round = 0; round < 2; round++) {
    try {
      const els = await overpassRace(q, 55000, null);
      return {
        urban: els.filter((e) => e.type === 'way' && !e.geometry),
        forest: els.filter((e) => e.type === 'relation' || (e.type === 'way' && e.geometry)),
      };
    } catch (_) { /* busy: once more */ }
  }
  return null;
}
function buildForestEntry(elements, lat, lon, radiusKm) {
  const N = 48, SS = 4, M = N * SS, R = radiusKm, cosL = Math.cos(lat * Math.PI / 180);
  const toX = (lo) => ((lo - lon) * KM_PER_DEG * cosL + R) / (2 * R) * M;
  const toY = (la) => ((la - lat) * KM_PER_DEG + R) / (2 * R) * M;
  const edges = [];   // [x0, y0, x1, y1] in sample units
  const addLine = (g) => {
    if (!g || g.length < 2) return;
    for (let i = 1; i < g.length; i++) {
      const a = g[i - 1], z = g[i];
      if (!a || !z) continue;
      const y0 = toY(a.lat), y1 = toY(z.lat);
      if (y0 === y1) continue;
      edges.push([toX(a.lon), y0, toX(z.lon), y1]);
    }
  };
  // A way that is also a member of a forest relation would cancel itself out (even-odd).
  const inRel = new Set();
  for (const el of elements) if (el.type === 'relation' && el.members) for (const m of el.members) if (m.type === 'way') inRel.add(m.ref);
  for (const el of elements) {
    if (el.type === 'way') {
      const g = el.geometry;
      if (inRel.has(el.id)) continue;
      if (g && g.length > 2 && g[0].lat === g[g.length - 1].lat && g[0].lon === g[g.length - 1].lon) addLine(g);
    } else if (el.type === 'relation' && el.members) {
      for (const m of el.members) if (m.type === 'way') addLine(m.geometry);
    }
  }
  if (!edges.length) return null;
  // Bucket edges by sample row so each row only looks at the edges crossing it.
  const rows = Array.from({ length: M }, () => []);
  for (const e of edges) {
    const lo = Math.max(0, Math.ceil(Math.min(e[1], e[3]) - 0.5)), hi = Math.min(M - 1, Math.ceil(Math.max(e[1], e[3]) - 0.5) - 1);
    for (let r = lo; r <= hi; r++) rows[r].push(e);
  }
  const cov = new Float32Array(N * N);
  for (let r = 0; r < M; r++) {
    const yc = r + 0.5, xs = [];
    for (const [x0, y0, x1, y1] of rows[r]) {
      if ((y0 <= yc) !== (y1 <= yc)) xs.push(x0 + (yc - y0) * (x1 - x0) / (y1 - y0));
    }
    xs.sort((a, b) => a - b);
    for (let k = 0; k + 1 < xs.length; k += 2) {
      const a = Math.max(0, Math.ceil(xs[k] - 0.5)), z = Math.min(M - 1, Math.ceil(xs[k + 1] - 0.5) - 1);
      for (let c = a; c <= z; c++) cov[((r / SS) | 0) * N + ((c / SS) | 0)] += 1 / (SS * SS);
    }
  }
  const raw = new Uint8Array(4 + N * N / 2), dv = new DataView(raw.buffer);
  raw[0] = N; dv.setUint16(2, Math.round(R * 10), true);
  let any = false;
  for (let k = 0; k < N * N; k++) {
    const v = Math.max(0, Math.min(15, Math.round(cov[k] / 0.7 * 15)));   // 70 % wooded = full forest
    if (v) any = true;
    raw[4 + (k >> 1)] |= v << ((k & 1) * 4);
  }
  return any ? { type: 7, n: raw.length / 4, raw } : null;
}

function buildUrbanEntry(elements, lat, lon, radiusKm) {
  const N = 48, R = radiusKm, cell = 2 * R / N, cosL = Math.cos(lat * Math.PI / 180);
  const cov = new Float32Array(N * N);
  for (const el of elements) {
    const bb = el.bounds;
    if (!bb) continue;
    const x0 = (bb.minlon - lon) * KM_PER_DEG * cosL, x1 = (bb.maxlon - lon) * KM_PER_DEG * cosL;
    const y0 = (bb.minlat - lat) * KM_PER_DEG, y1 = (bb.maxlat - lat) * KM_PER_DEG;
    if ((x1 - x0) * (y1 - y0) > 25) continue;              // huge boxes (whole districts) are not town density
    const i0 = Math.max(0, Math.floor((x0 + R) / cell)), i1 = Math.min(N - 1, Math.floor((x1 + R) / cell));
    const j0 = Math.max(0, Math.floor((y0 + R) / cell)), j1 = Math.min(N - 1, Math.floor((y1 + R) / cell));
    for (let j = j0; j <= j1; j++) for (let i = i0; i <= i1; i++) {
      const cx0 = -R + i * cell, cy0 = -R + j * cell;
      const ox = Math.min(x1, cx0 + cell) - Math.max(x0, cx0), oy = Math.min(y1, cy0 + cell) - Math.max(y0, cy0);
      if (ox > 0 && oy > 0) cov[j * N + i] += (ox * oy) / (cell * cell);
    }
  }
  const raw = new Uint8Array(4 + N * N / 2), dv = new DataView(raw.buffer);
  raw[0] = N; dv.setUint16(2, Math.round(R * 10), true);
  let any = false;
  for (let k = 0; k < N * N; k++) {
    const v = Math.max(0, Math.min(15, Math.round(cov[k] / 0.6 * 15)));   // 60 % covered = fully town
    if (v) any = true;
    raw[4 + (k >> 1)] |= v << ((k & 1) * 4);
  }
  return any ? { type: 6, n: raw.length / 4, raw } : null;
}

// Appends extra entries to a map blob (count at offset 12).
function appendEntries(blob, entries) {
  const extra = entries.reduce((s, e) => s + 3 + e.raw.length, 0);
  if (!entries.length || blob.length + extra > MAP_MAX_BYTES) return blob;
  const out = new Uint8Array(blob.length + extra);
  out.set(blob);
  const dv = new DataView(out.buffer);
  dv.setUint16(12, dv.getUint16(12, true) + entries.length, true);
  let o = blob.length;
  for (const e of entries) { out[o] = e.type; dv.setUint16(o + 1, e.n, true); out.set(e.raw, o + 3); o += 3 + e.raw.length; }
  out.lakes = blob.lakes;
  out.places = entries.filter((e) => e.type === 5).length;
  return out;
}

function simplify(pts, tol) {          // Douglas-Peucker, iterative
  const n = pts.length;
  if (n < 3) return pts;
  const keep = new Uint8Array(n); keep[0] = keep[n - 1] = 1;
  const stack = [[0, n - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    const [ax, ay] = pts[a], [bx, by] = pts[b];
    const dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    let maxd = 0, idx = -1;
    for (let i = a + 1; i < b; i++) {
      const [px, py] = pts[i];
      let d;
      if (len2 === 0) d = Math.hypot(px - ax, py - ay);
      else { const t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / len2)); d = Math.hypot(px - (ax + t * dx), py - (ay + t * dy)); }
      if (d > maxd) { maxd = d; idx = i; }
    }
    if (maxd > tol && idx > 0) { keep[idx] = 1; stack.push([a, idx], [idx, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}

// ---- Geometry helpers (all in km, x = east, y = north, origin = chosen location) ----
const TAU = Math.PI * 2;

// Splits a polyline into the parts that lie inside the circle, cutting exactly at the circle.
function clipCircle(pts, R) {
  const out = [];
  let cur = null;
  const inside = (p) => Math.hypot(p[0], p[1]) <= R;
  let prev = pts[0];
  if (inside(prev)) cur = { pts: [prev], entry: false, exit: false };
  for (let i = 1; i < pts.length; i++) {
    const a = prev, b = pts[i];
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const A = dx * dx + dy * dy, B = 2 * (a[0] * dx + a[1] * dy), C = a[0] * a[0] + a[1] * a[1] - R * R;
    const ts = [];
    if (A > 1e-12) {
      const D = B * B - 4 * A * C;
      if (D > 0) {
        const sq = Math.sqrt(D), t1 = (-B - sq) / (2 * A), t2 = (-B + sq) / (2 * A);
        if (t1 > 0 && t1 < 1) ts.push(t1);
        if (t2 > 0 && t2 < 1) ts.push(t2);
      }
    }
    let state = cur !== null;
    for (const t of ts) {
      const ip = [a[0] + dx * t, a[1] + dy * t];
      if (state) { cur.pts.push(ip); cur.exit = true; out.push(cur); cur = null; state = false; }
      else { cur = { pts: [ip], entry: true, exit: false }; state = true; }
    }
    if (state && cur) cur.pts.push(b);
    prev = b;
  }
  if (cur) out.push(cur);
  return out;
}

// Joins coastline ways that share an end node (direction is preserved: land on the left).
function stitchCoast(parts) {
  const byStart = new Map();
  parts.forEach((p, i) => byStart.set(p.k0, i));
  const hasPred = new Set();
  parts.forEach((p) => { if (byStart.has(p.k1)) hasPred.add(byStart.get(p.k1)); });
  const used = new Array(parts.length).fill(false), chains = [];
  const follow = (i) => {
    const pts = parts[i].pts.slice();
    used[i] = true;
    let k = parts[i].k1;
    while (byStart.has(k)) {
      const j = byStart.get(k);
      if (used[j]) break;
      used[j] = true;
      for (let q = 1; q < parts[j].pts.length; q++) pts.push(parts[j].pts[q]);
      k = parts[j].k1;
    }
    return pts;
  };
  parts.forEach((p, i) => { if (!used[i] && !hasPred.has(i)) chains.push(follow(i)); });
  parts.forEach((p, i) => { if (!used[i]) chains.push(follow(i)); });
  return chains;
}

const polyLen = (s) => { let len = 0; for (let i = 1; i < s.length; i++) len += Math.hypot(s[i][0] - s[i - 1][0], s[i][1] - s[i - 1][1]); return len; };
const signedArea = (r) => { let a = 0; for (let i = 0; i < r.length; i++) { const p = r[i], q = r[(i + 1) % r.length]; a += p[0] * q[1] - q[0] * p[1]; } return a / 2; };

// Sea polygons: the coastline keeps the water on its right, so each open piece is closed by
// following the circle clockwise to the next piece. Closed coastlines (islands) become holes.
function buildSeaRings(open, loops, R) {
  const ang = (p) => Math.atan2(p[1], p[0]);
  const rings = [];
  const used = new Array(open.length).fill(false);
  for (let s = 0; s < open.length; s++) {
    if (used[s]) continue;
    const ring = [];
    let i = s;
    for (let guard = 0; guard <= open.length; guard++) {
      used[i] = true;
      const p = open[i];
      for (const q of p) ring.push(q);
      const ea = ang(p[p.length - 1]);
      let best = -1, bestD = Infinity;
      for (let j = 0; j < open.length; j++) {
        if (used[j] && j !== s) continue;
        const d = (((ea - ang(open[j][0])) % TAU) + TAU) % TAU;
        if (d < bestD) { bestD = d; best = j; }
      }
      const steps = Math.ceil(bestD / (5 * Math.PI / 180));
      for (let k = 1; k < steps; k++) { const a = ea - bestD * k / steps; ring.push([R * Math.cos(a), R * Math.sin(a)]); }
      if (best === s || best < 0) break;
      i = best;
    }
    if (ring.length >= 3) rings.push(ring);
  }
  const ccw = loops.filter((l) => signedArea(l) > 0), cw = loops.filter((l) => signedArea(l) <= 0);
  if (open.length) {
    for (const l of loops) rings.push(l);
  } else if (ccw.length) {
    const disk = []; for (let k = 0; k < 72; k++) { const a = -TAU * k / 72; disk.push([R * Math.cos(a), R * Math.sin(a)]); }
    rings.push(disk); for (const l of ccw) rings.push(l);
  } else {
    for (const l of cw) rings.push(l);
  }
  return rings;
}

// Lakes and reservoirs: closed ways, or multipolygon relations whose member ways are
// joined end to end into rings. Points outside the downloaded box are skipped (the
// device clips the fill to the radar disc anyway).
const isWater = (t) => t && (t.natural === 'water' || t.landuse === 'reservoir');
function waterRings(elements, toKm) {
  const rings = [];
  const geomPts = (geom) => geom.filter((g) => g).map(toKm);
  const joinRings = (segs) => {
    const out = [], used = new Array(segs.length).fill(false);
    const same = (a, b) => Math.abs(a[0] - b[0]) < 1e-6 && Math.abs(a[1] - b[1]) < 1e-6;
    for (let s = 0; s < segs.length; s++) {
      if (used[s] || segs[s].length < 2) continue;
      used[s] = true;
      let ring = segs[s].slice();
      for (let guard = 0; guard < segs.length && !same(ring[0], ring[ring.length - 1]); guard++) {
        const end = ring[ring.length - 1];
        let found = false;
        for (let j = 0; j < segs.length; j++) {
          if (used[j] || segs[j].length < 2) continue;
          const sj = segs[j];
          if (same(sj[0], end)) { ring = ring.concat(sj.slice(1)); }
          else if (same(sj[sj.length - 1], end)) { ring = ring.concat(sj.slice(0, -1).reverse()); }
          else continue;
          used[j] = true; found = true; break;
        }
        if (!found) break;   // open (cut by the download box): close it straight
      }
      out.push(ring);
    }
    return out;
  };
  for (const el of elements) {
    if (!isWater(el.tags)) continue;
    if (el.type === 'way' && Array.isArray(el.geometry)) {
      const pts = geomPts(el.geometry);
      if (pts.length >= 4) rings.push(pts);
    } else if (el.type === 'relation' && Array.isArray(el.members)) {
      const segs = el.members.filter((m) => m.type === 'way' && Array.isArray(m.geometry))
        .map((m) => geomPts(m.geometry));
      for (const r of joinRings(segs)) if (r.length >= 4) rings.push(r);
    }
  }
  return rings;
}

function buildMapBlob(elements, lat, lon, withWater, radiusKm) {
  const cosL = Math.cos(lat * Math.PI / 180), R = Math.min(radiusKm, MAP_MAX_KM);
  const roads = [];                    // {type:1, pts}
  const coastParts = [];               // raw ways split at points outside the downloaded box
  for (const el of elements) {
    if (el.type !== 'way' || !Array.isArray(el.geometry) || isWater(el.tags)) continue;
    const coastline = el.tags && el.tags.natural === 'coastline';
    let cur = [], keys = [];
    const flush = () => {
      if (cur.length >= 2) {
        if (coastline) coastParts.push({ pts: cur, k0: keys[0], k1: keys[keys.length - 1] });
        else for (const c of clipCircle(cur, R)) if (c.pts.length >= 2) roads.push({ type: 1, hw: el.tags && el.tags.highway, pts: c.pts });
      }
      cur = []; keys = [];
    };
    for (const g of el.geometry) {
      if (!g) { flush(); continue; }   // point outside the requested box
      cur.push([(g.lon - lon) * KM_PER_DEG * cosL, (g.lat - lat) * KM_PER_DEG]);
      keys.push(g.lat.toFixed(7) + ',' + g.lon.toFixed(7));
    }
    flush();
  }

  // Coast: stitch, clip to the circle, separate open pieces from closed islands.
  const openPieces = [], loops = [], coastLines = [];
  for (const chain of stitchCoast(coastParts)) {
    const closed = chain.length > 3 && Math.hypot(chain[0][0] - chain[chain.length - 1][0], chain[0][1] - chain[chain.length - 1][1]) < 1e-6;
    const pieces = clipCircle(chain, R);
    if (closed && pieces.length > 1 && !pieces[0].entry && !pieces[pieces.length - 1].exit) {
      const last = pieces.pop(), first = pieces.shift();
      pieces.push({ pts: last.pts.concat(first.pts.slice(1)), entry: last.entry, exit: first.exit });
    }
    for (const c of pieces) {
      if (c.pts.length < 2) continue;
      if (closed && pieces.length === 1 && !c.entry && !c.exit) loops.push(c.pts);
      else if (c.entry && c.exit) openPieces.push(c.pts);
      else coastLines.push(c.pts);     // data ends inside the circle: outline only
    }
  }

  const COAST_PTS = 1800;              // share of the line budget reserved for the coast
  const roadPrio = (r) => r.hw === 'motorway' ? 1 : 2;

  // 1) Coast + sea: simplify just enough that the outline and the sea polygons fit.
  let tol = 0.03, coast = null;
  for (let iter = 0; iter < 10; iter++, tol *= 1.4) {
    const lines = [], simpleOpen = [], simpleLoops = [];
    let pts = 0;
    const add = (s) => { lines.push({ type: 0, pts: s }); pts += s.length; };
    for (const c of openPieces) { const s = simplify(c, tol); if (s.length >= 2) simpleOpen.push(s); if (polyLen(s) >= 0.15) add(s); }
    for (const c of loops) { const s = simplify(c, tol); if (s.length >= 4 && polyLen(s) >= 0.6) { simpleLoops.push(s.slice(0, -1)); add(s); } }
    for (const c of coastLines) { const s = simplify(c, tol); if (s.length >= 2 && polyLen(s) >= 0.15) add(s); }
    simpleLoops.sort((x, y) => Math.abs(signedArea(y)) - Math.abs(signedArea(x)));
    const rings = withWater ? buildSeaRings(simpleOpen, simpleLoops.slice(0, 50), R) : [];
    const ringPts = rings.reduce((n, r) => n + r.length, 0);
    coast = { lines, pts, rings, ringPts };
    if (pts <= COAST_PTS && ringPts <= MAP_MAX_RING_POINTS) break;
  }
  if (coast.ringPts > MAP_MAX_RING_POINTS) { coast.rings = []; coast.ringPts = 0; }   // sea fill does not fit: outline only
  while (coast.pts > MAP_MAX_POINTS && coast.lines.length) {                             // extreme coastline: drop the shortest pieces
    coast.lines.sort((x, y) => polyLen(y.pts) - polyLen(x.pts));
    coast.pts -= coast.lines.pop().pts.length;
  }

  // 2) Lakes: largest first, as many as fit in their share of the budget.
  const LAKE_PTS = 900;
  const lakes = [];
  let lakePts = 0;
  if (withWater) {
    const toKm = (g) => [(g.lon - lon) * KM_PER_DEG * cosL, (g.lat - lat) * KM_PER_DEG];
    const cand = [];
    for (const r of waterRings(elements, toKm)) {
      const area = Math.abs(signedArea(r));
      if (area < 0.03) continue;                                        // smaller than ~170 x 170 m
      if (!r.some((p) => Math.hypot(p[0], p[1]) <= R * 1.05)) continue;  // not on the disc
      cand.push({ area, pts: r });
    }
    cand.sort((a, b) => b.area - a.area);
    for (const c of cand) {
      let s = simplify(c.pts.concat([c.pts[0]]), Math.max(tol, 0.03)).slice(0, -1);
      if (s.length < 3) continue;
      if (lakePts + s.length > LAKE_PTS) continue;
      lakes.push(s); lakePts += s.length;
    }
  }

  // 3) Roads: motorways first, then trunk roads, longest first, as many as fit.
  const roadLines = [];
  for (const r of roads) {
    const s = simplify(r.pts, Math.max(tol, 0.05));
    if (s.length >= 2 && polyLen(s) >= 0.4) roadLines.push({ type: 1, p: roadPrio(r), len: polyLen(s), pts: s });
  }
  roadLines.sort((x, y) => x.p - y.p || y.len - x.len);
  const lines = coast.lines.slice();
  let linePts = coast.pts;
  const bytesFor = (nl, lp) => 14 + (nl + coast.rings.length + lakes.length) * 3 + (lp + coast.ringPts + lakePts) * 4;
  for (const l of roadLines) {
    if (linePts + l.pts.length > MAP_MAX_POINTS || bytesFor(lines.length + 1, linePts + l.pts.length) > MAP_MAX_BYTES) continue;
    lines.push(l); linePts += l.pts.length;
  }
  const built = { lines, rings: coast.rings, linePts, ringPts: coast.ringPts };
  if (!built.lines.length && !lakes.length) return null;    // nothing to draw here
  // Draw order on the device: roads, then coast lines (on top of the sea fill), rings are masked separately.
  built.lines.sort((a, b) => b.type - a.type);
  const all = built.lines.map((l) => ({ type: l.type, pts: l.pts }))
    .concat(built.rings.map((r) => ({ type: 2, pts: r })))
    .concat(lakes.map((r) => ({ type: 3, pts: r })));   // type 3: lake (filled + outlined, older firmware ignores it)
  const bytes = 14 + all.length * 3 + (built.linePts + built.ringPts + lakePts) * 4;
  const buf = new ArrayBuffer(bytes), dv = new DataView(buf);
  dv.setUint8(0, 0x50); dv.setUint8(1, 0x4D); dv.setUint8(2, built.rings.length ? 2 : 1); dv.setUint8(3, 0);
  dv.setInt32(4, Math.round(lat * 1e7), true); dv.setInt32(8, Math.round(lon * 1e7), true);
  dv.setUint16(12, all.length, true);
  let o = 14;
  const q = (v) => Math.max(-32767, Math.min(32767, Math.round(v * 100)));   // 10 m units
  for (const l of all) {
    dv.setUint8(o, l.type); dv.setUint16(o + 1, l.pts.length, true); o += 3;
    for (const [x, y] of l.pts) { dv.setInt16(o, q(x), true); dv.setInt16(o + 2, q(y), true); o += 4; }
  }
  const out = new Uint8Array(buf);
  out.lakes = lakes.length;
  return out;
}

// Ground heights from the AWS Open Data terrain tiles (Terrarium PNG, no rate limit):
// height = R*256 + G + B/256 - 32768 metres. Zoom 10 is ~115 m per pixel.
async function elevationFromTiles(pts, onProgress) {
  const Z = 10, n = 2 ** Z;
  const px = (lat, lon) => {
    const s = Math.sin(lat * Math.PI / 180);
    return [((lon + 180) / 360) * n * 256, (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n * 256];
  };
  const need = new Map();
  for (const [la, lo] of pts) {
    const [x, y] = px(la, lo);
    const key = Math.floor(x / 256) + '/' + Math.floor(y / 256);
    need.set(key, null);
  }
  if (need.size > 16) throw new Error('alan çok geniş');
  let done = 0;
  await Promise.all([...need.keys()].map(async (key) => {
    const [tx, ty] = key.split('/');
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.src = 'https://s3.amazonaws.com/elevation-tiles-prod/terrarium/' + Z + '/' + tx + '/' + ty + '.png';
    await img.decode();
    const cv = document.createElement('canvas');
    cv.width = cv.height = 256;
    const g = cv.getContext('2d', { willReadFrequently: true });
    g.drawImage(img, 0, 0);
    need.set(key, g.getImageData(0, 0, 256, 256).data);
    onProgress('Arazi yükseklikleri indiriliyor… %' + Math.round((++done / need.size) * 100));
  }));
  return pts.map(([la, lo]) => {
    const [x, y] = px(la, lo);
    const tx = Math.floor(x / 256), ty = Math.floor(y / 256);
    const d = need.get(tx + '/' + ty);
    const ix = Math.min(255, Math.floor(x - tx * 256)), iy = Math.min(255, Math.floor(y - ty * 256));
    const i = (iy * 256 + ix) * 4;
    return Math.round(d[i] * 256 + d[i + 1] + d[i + 2] / 256 - 32768);
  });
}

// Fallback: Open-Meteo elevation API. Its free tier counts every coordinate, so ask
// slowly in small batches and wait when it answers 429 (too many requests).
async function elevationFromOpenMeteo(pts, onProgress) {
  const out = [];
  const BATCH = 50;
  for (let i = 0; i < pts.length; i += BATCH) {
    const part = pts.slice(i, i + BATCH);
    const url = 'https://api.open-meteo.com/v1/elevation?latitude=' + part.map((p) => p[0].toFixed(5)).join(',') +
      '&longitude=' + part.map((p) => p[1].toFixed(5)).join(',');
    let j = null;
    for (let attempt = 0; attempt < 6 && !j; attempt++) {
      const r = await fetch(url);
      if (r.status === 429) {
        onProgress('Arazi sunucusu yoğun, bekleniyor… (%' + Math.round((out.length / pts.length) * 100) + ')');
        await sleep(10000 * (attempt + 1));
        continue;
      }
      if (!r.ok) throw new Error('sunucu ' + r.status);
      j = await r.json();
    }
    if (!j) throw new Error('sunucu çok yoğun (429), birkaç dakika sonra tekrar deneyin');
    for (const v of j.elevation || []) out.push(Math.round(Number.isFinite(v) ? v : 0));
    onProgress('Arazi yükseklikleri indiriliyor… %' + Math.round((out.length / pts.length) * 100));
    await sleep(1500);
  }
  return out;
}

// The map blob for a card (its HELLO fields hw / mapv): null when there is nothing to draw
// at this place. Throws on network errors (with err.reasons) or a map that is too big.
async function buildCardMap(lat, lon, label, dev, onStatus) {
  const radius = mapRadiusFor(label);
  onStatus('Harita indiriliyor… (birkaç saniye sürebilir)');
  // 5" board: places are fetched at the same time as the map.
  const wantPlaces = dev.hw === 'ws5' || dev.hw === 'ws5b';
  let placesMissing = false;
  const placesP = wantPlaces ? fetchPlaces(lat, lon, radius) : Promise.resolve(null);
  const elements = await fetchOverpass(lat, lon, radius, onStatus);
  // After the map, so no more than two requests run at once.
  const coverP = wantPlaces ? fetchCover(lat, lon, radius) : Promise.resolve(null);
  const water = (dev.mapv || 1) >= 2;
  let blob = buildMapBlob(elements, lat, lon, water, radius);
  if (!blob) return null;
  // 5" board: airports, landmarks and town names on the map too.
  if (wantPlaces) {
    onStatus('Önemli yerler, yerleşim ve ormanlar ekleniyor…');
    const [places, cover] = await Promise.all([placesP, coverP]);
    const urban = cover && cover.urban, forest = cover && cover.forest;
    placesMissing = !places;
    const extra = places ? buildPlaceEntries(places, lat, lon, radius) : [];
    const town = urban ? buildUrbanEntry(urban, lat, lon, radius) : null;
    if (town) extra.unshift(town);
    let woods = null;
    try { woods = forest ? buildForestEntry(forest, lat, lon, radius) : null; } catch (_) { /* optional */ }
    if (woods) extra.unshift(woods);
    if (extra.length) blob = appendEntries(blob, extra);
  }
  return { blob, water, placesMissing };
}

// 3D terrain: an N x N grid of ground heights (terrain tiles, else Open-Meteo) over the
// same square as the map, for ELEVBEGIN / ELEVROW (firmware terrain.h).
async function terrainHeights(lat, lon, label, onStatus, onNote) {
  const half = mapRadiusFor(label), N = 25;
  onStatus('Arazi yükseklikleri indiriliyor…');
  const cosl = Math.cos(lat * Math.PI / 180);
  const pts = [];
  for (let r = 0; r < N; r++) {
    for (let c = 0; c < N; c++) {
      const y = -half + (2 * half * r) / (N - 1), x = -half + (2 * half * c) / (N - 1);
      pts.push([lat + y / 111, lon + x / (111 * cosl)]);
    }
  }
  let h;
  try {
    h = await elevationFromTiles(pts, onStatus);
  } catch (tileErr) {
    onNote('Arazi resimleri alınamadı (' + (tileErr && tileErr.message) + '), Open-Meteo deneniyor');
    h = await elevationFromOpenMeteo(pts, onStatus);
  }
  if (h.length !== N * N) throw new Error('eksik veri');
  return { n: N, half: Math.round(half * 100) / 100, h };
}
