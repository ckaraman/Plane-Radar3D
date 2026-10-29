// Tests for the OpenFreeMap part of web/mapdata.js: node web/tests/mapdata.test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'mapdata.js'), 'utf8');
const M = vm.runInThisContext(src + '\n;({ decodeTile, tileElements, buildMapBlob, KM_PER_DEG });');

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

// Roads, lakes, built-up land, airports and landmarks get the tags the map builders read.
{
  const out = M.tileElements([tile(X, {
    transportation: { extent: E, features: [{ type: 2, tags: { class: 'motorway' }, geom: [[[100, 100], [3000, 3000]]] },
                                            { type: 2, tags: { class: 'primary' }, geom: [[[100, 100], [3000, 100]]] }] },
    water: { extent: E, features: [poly('lake', [[1000, 1000], [2000, 1000], [2000, 2000], [1000, 2000]]), poly('river', [[0, 0], [10, 0], [10, 10]])] },
    landuse: { extent: E, features: [poly('residential', [[100, 100], [200, 100], [200, 200], [100, 200]])] },
    aerodrome_label: { extent: E, features: [{ type: 1, tags: { iata: 'IST', name: 'İstanbul' }, geom: [[[2000, 2000]]] }] },
    aeroway: { extent: E, features: [{ type: 2, tags: { class: 'runway' }, geom: [[[1500, 1500], [1900, 1500]]] }] },
    poi: { extent: E, features: [{ type: 1, tags: { class: 'stadium', subclass: 'stadium', name: 'Arena', rank: 3 }, geom: [[[500, 500]]] },
                                 { type: 1, tags: { class: 'shop', subclass: 'bakery', name: 'Fırın', rank: 9 }, geom: [[[600, 600]]] }] },
  })]);
  assert.deepStrictEqual(out.map.filter((e) => e.tags.highway).map((e) => e.tags.highway), ['motorway']);
  assert.strictEqual(out.map.filter((e) => e.tags.natural === 'water').length, 1, 'lake kept, river left out');
  const urban = out.cover.filter((e) => !e.geometry);
  assert.strictEqual(urban.length, 1, 'built-up land as a bounding box');
  assert.ok(urban[0].bounds.minlat < urban[0].bounds.maxlat && urban[0].bounds.minlon < urban[0].bounds.maxlon, 'bounds');
  const ap = out.places.find((e) => e.tags.aeroway === 'aerodrome');
  assert.ok(ap && ap.type === 'node' && ap.tags.iata === 'IST', 'airport');
  assert.ok(out.places.some((e) => e.tags.aeroway === 'runway' && e.geometry.length === 2), 'runway');
  const lm = out.places.filter((e) => e.type === 'node' && !e.tags.aeroway);
  assert.strictEqual(lm.length, 1, 'only landmarks with a symbol');
  assert.strictEqual(lm[0].tags.leisure, 'stadium');
  assert.strictEqual(lm[0].tags.rank, 3);
}

console.log('ok');
