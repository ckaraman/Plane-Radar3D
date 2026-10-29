// Checks for the system 2 setup page (stage 9a): node web/tests/deck.test.js
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const html = fs.readFileSync(path.join(__dirname, '..', 'deck.html'), 'utf8');
const idx = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');

for (const s of ['manifest-full-ws5.json', 'href="PlaneRadarDeck.exe"', 'href="PlaneRadarDeck-mac.dmg"',
  'href="PlaneRadarDeck-mac-intel.dmg"', 'href="radar/"', 'esp-web-tools@10.1.1', 'slot="unsupported"',
  'Bağlantıyı bırak', 'BOOT']) {
  assert(html.includes(s), 'deck.html: missing ' + s);
}
// The page never talks to the card itself: no Web Serial, no map code.
for (const s of ['navigator.serial', 'mapdata.js']) assert(!html.includes(s), 'deck.html: has ' + s);
// CI builds /radar/ from index.html with sed on these two lines.
assert(/^<head>$/m.test(idx) && /^<body>$/m.test(idx), 'index.html: CI sed needs <head> and <body> lines');
assert(idx.includes('Kartı hazırla'), 'index.html: the system 2 switch must point to "Kartı hazırla"');
// Review fixes: the program is taken back after the install, the Mac has a menu bar, the map is sent by a button.
for (const s of ['geri alın', 'menü çubuğundaki', 'Haritayı gönder']) assert(html.includes(s), 'deck.html: missing ' + s);
for (const s of ['tepsi menüsünden', 'listede görünmez']) assert(!html.includes(s), 'deck.html: has ' + s);
// The program window may only send people to what the site still has.
const app = fs.readFileSync(path.join(__dirname, '..', '..', 'helper', 'web', 'index.html'), 'utf8');
assert(!app.includes('güncelleme ya da harita göndermeden'), 'helper/web/index.html: points to removed page features');
console.log('deck page ok');
