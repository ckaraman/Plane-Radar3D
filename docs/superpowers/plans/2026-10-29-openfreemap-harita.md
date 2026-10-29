# OpenFreeMap Map Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `web/mapdata.js` takes map data from OpenFreeMap vector tiles first and falls back to Overpass, without changing the map blob the card receives.

**Architecture:** A small MVT decoder plus an adapter turn tiles into Overpass-shaped `elements` (`{type:'way', id, tags, geometry:[{lat,lon}]}`), so `buildMapBlob`, `buildPlaceEntries`, `buildUrbanEntry` and `buildForestEntry` stay as they are. `buildCardMap` asks OpenFreeMap first; each of the three data sets (map, places, cover) falls back to its existing Overpass fetch on its own.

**Tech Stack:** Plain browser JS (no modules, globals, no new dependency), Node for the test.

**Spec:** `docs/superpowers/specs/2026-10-29-openfreemap-harita-design.md`

## Global Constraints

- Card firmware and the map blob format do not change (31 KB / 3000 points limits stay).
- No new dependency; `mapdata.js` stays a plain script shared by `web/index.html` and `helper/web/index.html`.
- Overpass stays as fallback.
- Landmarks chosen by OpenFreeMap `poi.rank` (no wikidata requirement).
- Terrain code untouched.
- User-visible text in Turkish; code, comments and commits in English.

## Review Focus

- Tile seams: a coastline crossing a tile border must stitch into one chain (else the sea is not filled). Test: two neighbour tiles.
- Coast direction: land on the left of every coastline way. Test: ocean on the south half gives a west-to-east line.
- Polygons overlapping in the tile buffer would cancel out in the even-odd forest raster: polygons are clipped to the tile square. Test: forest ring past the tile edge is clipped to 0..extent.
- One tile failing (404 / timeout / bad data) must fall back to Overpass for that data set, not break the send. Test: fetch stub that throws → `fetchOverpass` is used.
- Inland place (no ocean): no coastline, map still built from roads and lakes. Checked by hand (Ankara).

---

### Task 1: MVT decoder and tile adapter

**Files:**
- Modify: `web/mapdata.js` (new section after `crc32`, before the Overpass code)
- Create: `web/tests/mapdata.test.js`, `web/tests/ofm-12-2375-1530.pbf` (real tile fixture, ~2.5 KB)

**Interfaces:**
- Produces:
  - `decodeTile(buf: Uint8Array) -> {[layer]: {extent, features: [{type: 1|2|3, tags: {}, geom: [[[x,y],...], ...]}]}}` (type 1 point, 2 line, 3 polygon; geom = parts / rings in tile pixels).
  - `tileElements(tiles: [{z, x, y, layers}]) -> {map: elements[], places: elements[], cover: elements[]}` with:
    - `water class=ocean` polygons → clipped to the tile square, edges lying on the tile border dropped, remaining chains reversed (MVT ring order puts water on the left; OSM coast keeps land on the left) → `tags {natural:'coastline'}`; chain ends on a tile border are snapped (within 2 px at that zoom, global pixel coords) to the matching end in the neighbour tile so the lat/lon keys are equal.
    - `water class=lake` → closed way `tags {natural:'water'}` (clipped).
    - `transportation class motorway|trunk` (lines, clipped) → `tags {highway: class}`.
    - `landcover class=wood` → closed way `tags {landuse:'forest'}` (clipped, `geometry`) into `cover`.
    - `landuse class residential|commercial|industrial|retail` → way with `bounds {minlat,minlon,maxlat,maxlon}` and no `geometry` into `cover`.
    - `poi` named, class → tags so `landmarkCategory` works (stadium→`leisure:stadium`, zoo/museum/theme_park/attraction→`tourism`, castle/monument/fort/palace→`historic`, place_of_worship→`amenity`, park→`leisure:park`), plus `rank` → node-shaped `{type:'node', lat, lon, tags}` into `places`.
    - `aerodrome_label` with 3-letter `iata` → node `tags {aeroway:'aerodrome', iata, name}`; `aeroway class=runway` lines → way `tags {aeroway:'runway'}` into `places`.
- Change in `buildPlaceEntries`: landmark `rank: t.rank != null ? t.rank : (t.wikipedia ? 0 : 1)`.

- [ ] **Step 1: Write the failing tests** in `web/tests/mapdata.test.js` (loads `web/mapdata.js` with `vm.runInThisContext`, plain `assert`):
  - `decodeTile(fixture)` has layers `water` and `transportation`, every coordinate within `-extent*0.1 .. extent*1.1`.
  - one tile with an ocean square over its south half → one coastline way, `geometry[0].lon < geometry.at(-1).lon`.
  - two neighbour tiles (x, x+1) with that ocean half each → after `stitchCoast` on `buildMapBlob`'s input, the coast is one chain (check: `buildMapBlob` result header byte 2 === 2, sea rings present).
  - a wood ring from -500 to 4600 px → all forest geometry inside the tile's lat/lon bounds.
- [ ] **Step 2: Run** `node web/tests/mapdata.test.js` → fails (`decodeTile is not defined`).
- [ ] **Step 3: Implement** `decodeTile` (protobuf varint / zigzag, MVT command integers MoveTo 1 / LineTo 2 / ClosePath 7, keys/values tables) and `tileElements` (Sutherland–Hodgman square clip for rings, Liang–Barsky style split for lines; pixel → lat/lon via Web Mercator `lon = gx/2^z/E*360-180`, `lat = atan(sinh(pi*(1-2*gy/2^z/E)))`).
- [ ] **Step 4: Run** `node web/tests/mapdata.test.js` → `ok`.
- [ ] **Step 5: Commit** `Map: OpenFreeMap tile decoder and adapter`.

### Task 2: Fetch tiles, wire into buildCardMap, fallback

**Files:**
- Modify: `web/mapdata.js` (`buildCardMap`, new `fetchOpenFreeMap`)
- Modify: `web/index.html:375-376`, `helper/web/index.html:236` (source note)
- Modify: `.github/workflows/build.yml` (step "Helper protocol tests": add `node web/tests/mapdata.test.js`)
- Test: `web/tests/mapdata.test.js`

**Interfaces:**
- Consumes: `decodeTile`, `tileElements` (Task 1).
- Produces: `async fetchOpenFreeMap(lat, lon, radiusKm, fetchFn = fetch) -> {map, places, cover}`; throws on any failed tile.
  - TileJSON `https://tiles.openfreemap.org/planet` gives the tile URL template (cached per page).
  - Map, cover, poi: zoom 12 over the same box as `fetchOverpass` (`radiusKm * 1.25`). Airports and runways: zoom 10 over the `AIRPORT_KM` box. At most 6 requests at a time; 15 s timeout per tile.
- `buildCardMap`: `ofm = await fetchOpenFreeMap(...).catch(() => null)`; map elements `ofm ? ofm.map : await fetchOverpass(...)`; places/cover the same with `fetchPlaces` / `fetchCover`. Status text while fetching: `Harita indiriliyor (OpenFreeMap)…`; on fallback the existing Overpass texts.
- Source notes: `Harita © OpenStreetMap katkıda bulunanları (ODbL), OpenFreeMap / OpenMapTiles karolarından; gerekirse Overpass servisinden.`

- [ ] **Step 1: Failing test:** `fetchOpenFreeMap` with a stub `fetchFn` returning the TileJSON and the fixture tile for every tile URL → `map.length > 0`; a stub that throws for tiles → `buildCardMap` ends up calling a stubbed `fetchOverpass` (replace the global for the test).
- [ ] **Step 2: Run** `node web/tests/mapdata.test.js` → fails.
- [ ] **Step 3: Implement** as above; update the two notes and the CI step.
- [ ] **Step 4: Run** test → `ok`; `node --check` on the scripts pulled out of both HTML files.
- [ ] **Step 5: Browser check** (setup page served at localhost:8765, `buildCardMap` run in the page): Istanbul 17 km (sea filled, Bosphorus, lakes, airports IST/SAW), Ankara 17 km (no coast), a small inland town 5 km. Draw each blob on a canvas and compare with Overpass if it answers.
- [ ] **Step 6: Commit** `Map: OpenFreeMap first, Overpass as fallback`; bump Deck to 1.8.0 in `helper/app.py` VERSION; PLAN.md row 8 note.
