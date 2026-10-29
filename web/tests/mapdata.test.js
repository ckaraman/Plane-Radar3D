// Tests for the OpenFreeMap part of web/mapdata.js: node web/tests/mapdata.test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'mapdata.js'), 'utf8');
const M = vm.runInThisContext(src + '\n;({ decodeTile, tileElements, buildMapBlob, KM_PER_DEG, fetchOpenFreeMap, buildCardMap, fetchLandmarks });');

const E = 4096, Z = 12, X = 2375, Y = 1530;
const tileLon = (x) => x / 2 ** Z * 360 - 180;
const tileLat = (y) => Math.atan(Math.sinh(Math.PI * (1 - 2 * y / 2 ** Z))) * 180 / Math.PI;
const poly = (cls, ring) => ({ type: 3, tags: { class: cls }, geom: [ring] });
const tile = (x, layers) => ({ z: Z, x, y: Y, layers });
// Ocean over the south half of a tile, reaching into the buffer like real tiles (clockwise, y down).
const southSea = () => poly('ocean', [[-64, 2048], [E + 64, 2048], [E + 64, E + 64], [-64, E + 64]]);

// decodeTile reads a real OpenFreeMap tile.
{
  const t = M.decodeTile(new Uint8Array(fs.readFileSync(path.join(__dirname, 'ofm-12-2375-1530.pbf'))));
  assert.ok(t.water && t.water.features.length > 0, 'water layer');
  for (const name of Object.keys(t)) {
    const ext = t[name].extent;
    for (const f of t[name].features) {
      assert.ok([1, 2, 3].includes(f.type), name + ' feature type');
      if (f.type === 1) continue;   // labels reach far into the buffer
      for (const part of f.geom) for (const [x, y] of part) {
        assert.ok(x > -ext * 0.1 && x < ext * 1.1 && y > -ext * 0.1 && y < ext * 1.1, name + ' coordinate in range');
      }
    }
  }
  assert.ok(t.water.features.some((f) => f.type === 3 && f.tags.class), 'water polygons carry a class');
}

// Sea to the south: one coastline, land (north) on its left, so it runs west to east.
{
  const out = M.tileElements([tile(X, { water: { extent: E, features: [southSea()] } })]);
  const coast = out.map.filter((e) => e.tags.natural === 'coastline');
  assert.strictEqual(coast.length, 1, 'one coastline way');
  const g = coast[0].geometry;
  assert.ok(g[0].lon < g[g.length - 1].lon, 'coast runs west to east');
  assert.ok(Math.abs(g[0].lat - tileLat(Y + 0.5)) < 1e-6, 'coast on the sea edge, not the tile border');
}

// The same sea over two neighbour tiles stitches into one coast that crosses the circle: sea is filled.
{
  const out = M.tileElements([tile(X, { water: { extent: E, features: [southSea()] } }),
                              // the neighbour's copy of the coast is quantised a pixel off at the border
                              tile(X + 1, { water: { extent: E, features: [poly('ocean', [[-64, 2049], [E + 64, 2048], [E + 64, E + 64], [-64, E + 64]])] } })]);
  const blob = M.buildMapBlob(out.map, tileLat(Y + 0.5) + 0.005, tileLon(X + 1), true, 3);
  assert.ok(blob, 'map built');
  assert.strictEqual(blob[2], 2, 'sea rings present (coast stitched across the tile border)');
}

// A coast crossing a tile border at a shallow angle: the neighbour's copy, a pixel off across
// the line, crosses the border far from this tile's crossing point; still one coast.
{
  // sea south of a line falling 0.097 px per px towards the east, crossing the south border near x 2000
  const a = tile(X, { water: { extent: E, features: [poly('ocean', [[-64, 3896], [E + 64, 4306], [E + 64, E + 64], [-64, E + 64]])] } });
  // the tile below: the same line a pixel lower, so it reaches its north border about 10 px away
  const b = { z: Z, x: X, y: Y + 1, layers: { water: { extent: E, features: [poly('ocean', [[-64, -199], [E + 64, 211], [E + 64, E + 64], [-64, E + 64]])] } } };
  const out = M.tileElements([a, b]);
  const blob = M.buildMapBlob(out.map, tileLat(Y + 1), tileLon(X + 0.5), true, 3);
  assert.ok(blob, 'map built');
  assert.strictEqual(blob[2], 2, 'sea rings present (shallow seam joined)');
}

// A forest reaching past the tile edge is clipped to the tile (overlaps would cancel in even-odd).
{
  const out = M.tileElements([tile(X, { landcover: { extent: E, features: [poly('wood', [[-500, -500], [4600, -500], [4600, 4600], [-500, 4600]])] } })]);
  const woods = out.cover.filter((e) => e.tags.landuse === 'forest');
  assert.strictEqual(woods.length, 1, 'one forest');
  const g = woods[0].geometry;
  const eps = 1e-9;
  for (const p of g) {
    assert.ok(p.lon >= tileLon(X) - eps && p.lon <= tileLon(X + 1) + eps, 'forest lon inside tile');
    assert.ok(p.lat <= tileLat(Y) + eps && p.lat >= tileLat(Y + 1) - eps, 'forest lat inside tile');
  }
  assert.ok(g[0].lat === g[g.length - 1].lat && g[0].lon === g[g.length - 1].lon, 'forest ring closed');
}

// Roads, lakes, built-up land and airports get the tags the map builders read.
{
  const out = M.tileElements([tile(X, {
    transportation: { extent: E, features: [{ type: 2, tags: { class: 'motorway' }, geom: [[[100, 100], [3000, 3000]]] },
                                            { type: 2, tags: { class: 'primary' }, geom: [[[100, 100], [3000, 100]]] },
                                            { type: 2, tags: { class: 'motorway', ramp: 1 }, geom: [[[200, 200], [900, 900]]] }] },
    water: { extent: E, features: [poly('lake', [[1000, 1000], [2000, 1000], [2000, 2000], [1000, 2000]]), poly('river', [[0, 0], [10, 0], [10, 10]])] },
    landuse: { extent: E, features: [poly('residential', [[100, 100], [200, 100], [200, 200], [100, 200]])] },
    aerodrome_label: { extent: E, features: [{ type: 1, tags: { iata: 'IST', name: 'İstanbul' }, geom: [[[2000, 2000]]] }] },
    aeroway: { extent: E, features: [{ type: 2, tags: { class: 'runway' }, geom: [[[1500, 1500], [1900, 1500]]] }] },
  })]);
  assert.deepStrictEqual(out.map.filter((e) => e.tags.highway).map((e) => e.tags.highway), ['motorway']);
  assert.strictEqual(out.map.filter((e) => e.tags.natural === 'water').length, 1, 'lake kept, river left out');
  const urban = out.cover.filter((e) => !e.geometry);
  assert.strictEqual(urban.length, 1, 'built-up land as a bounding box');
  assert.ok(urban[0].bounds.minlat < urban[0].bounds.maxlat && urban[0].bounds.minlon < urban[0].bounds.maxlon, 'bounds');
  const ap = out.places.find((e) => e.tags.aeroway === 'aerodrome');
  assert.ok(ap && ap.type === 'node' && ap.tags.iata === 'IST', 'airport');
  assert.ok(out.places.some((e) => e.tags.aeroway === 'runway' && e.geometry.length === 2), 'runway');
}


(async () => {
  const fixture = fs.readFileSync(path.join(__dirname, 'ofm-12-2375-1530.pbf'));
  const urls = [];
  const okFetch = async (url) => {
    urls.push(url);
    if (url === 'https://tiles.openfreemap.org/planet') return { ok: true, json: async () => ({ tiles: ['https://t.test/{z}/{x}/{y}.pbf'] }) };
    return { ok: true, arrayBuffer: async () => fixture.buffer.slice(fixture.byteOffset, fixture.byteOffset + fixture.length) };
  };
  // Tiles for the map box at zoom 12 and for the airport box at zoom 10, all decoded.
  const lat = tileLat(Y + 0.5), lon = tileLon(X + 0.5);
  const ofm = await M.fetchOpenFreeMap(lat, lon, 5, okFetch);
  assert.ok(ofm.map.some((e) => e.tags.natural === 'coastline'), 'coast from the tiles');
  assert.ok(urls.some((u) => u.startsWith('https://t.test/12/2375/1530')), 'map tile at zoom 12');
  assert.ok(urls.some((u) => u.startsWith('https://t.test/10/')), 'airport tiles at zoom 10');

  // buildCardMap: tiles first, Overpass only when a tile fails.
  let overpassCalls = 0;
  globalThis.fetchOverpass = async () => { overpassCalls++; return [{ type: 'way', tags: { highway: 'motorway' }, geometry: [{ lat, lon: lon - 0.03 }, { lat, lon: lon + 0.03 }] }]; };
  // Landmarks come from Wikidata, famous ones first, one row per item.
  const row = (q, name, lo, la, links, type) => ({ item: { value: 'http://www.wikidata.org/entity/' + q }, itemLabel: { value: name },
    loc: { value: 'Point(' + lo + ' ' + la + ')' }, links: { value: String(links) }, type: { value: 'http://www.wikidata.org/entity/' + type } });
  let sparql = null;
  const wd = async (url) => { sparql = decodeURIComponent(url); return { ok: true, json: async () => ({ results: { bindings: [
    row('Q12506', 'Ayasofya', 28.98, 41.008, 152, 'Q32815'), row('Q12506', 'Ayasofya', 28.98, 41.008, 152, 'Q33506'),
    row('Q1', 'Rams Park', 28.99, 41.10, 71, 'Q1154710'), row('Q2', 'Bir şey', 28.9, 41.0, 90, 'Q5'), row('Q3', 'Q3', 28.9, 41.0, 40, 'Q22698')] } }) }; };
  const lms = await M.fetchLandmarks(41.0082, 28.9784, 25.7, wd);
  assert.ok(sparql.includes('wikibase:radius "25.7"') && sparql.includes('Point(28.9784 41.0082)'), 'asks around the place');
  assert.deepStrictEqual(lms.map((e) => e.tags.name), ['Ayasofya', 'Rams Park'], 'one per item, known kinds, with a name');
  assert.strictEqual(lms[0].tags.amenity, 'place_of_worship');
  assert.strictEqual(lms[0].lat, 41.008);
  assert.strictEqual(lms[1].tags.leisure, 'stadium');
  assert.ok(lms[0].tags.rank < lms[1].tags.rank, 'more Wikipedia articles: better rank');
  assert.strictEqual(await M.fetchLandmarks(41, 29, 5, async () => { throw new Error('down'); }), null, 'Wikidata down: null');

  let placesCalls = 0;
  globalThis.fetchLandmarks = async () => { placesCalls++; return [{ type: 'node', lat: lat + 0.02, lon, tags: { leisure: 'stadium', name: 'Arena', rank: -10 } }]; };
  globalThis.fetchPlaces = async () => { throw new Error('Overpass not used for places when the tiles answer'); };
  globalThis.fetchCover = async () => null;
  globalThis.fetch = okFetch;
  let built = await M.buildCardMap(lat, lon, 5, { hw: 'ws5', mapv: 2 }, () => {});
  assert.ok(built && built.blob.length > 14, 'map from the tiles');
  assert.strictEqual(overpassCalls, 0, 'Overpass not asked when the tiles answer');
  assert.strictEqual(placesCalls, 1, 'landmarks asked from Wikidata');
  assert.strictEqual(built.placesMissing, false, 'landmarks arrived');
  globalThis.fetchLandmarks = async () => null;
  built = await M.buildCardMap(lat, lon, 5, { hw: 'ws5', mapv: 2 }, () => {});
  assert.strictEqual(built.placesMissing, true, 'Wikidata down: landmarks missing, map still built');
  globalThis.fetchPlaces = async () => null;
  globalThis.fetch = async (url) => { if (url.includes('/10/')) throw new Error('down'); return okFetch(url); };
  built = await M.buildCardMap(lat, lon, 5, { hw: 'ws5', mapv: 2 }, () => {});
  assert.ok(built && built.blob.length > 14, 'map from Overpass');
  assert.strictEqual(overpassCalls, 1, 'one failed tile: Overpass is used');
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
