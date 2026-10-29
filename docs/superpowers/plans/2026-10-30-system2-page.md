# 9a: 2. sistemin kurulum sayfası — Uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sitenin kökü, karta bağlanmayan sade bir 2. sistem sayfası (`web/deck.html`) olsun; bugünkü sayfa yalnızca `/radar` olarak kalsın.

**Architecture:** Yeni statik sayfa: üç kart (Kartı hazırla, Programı indir, Kurtarma), esp-web-tools düğmesi, Web Serial kodu yok. CI kökü bu dosyadan, `/radar`'ı `web/index.html`'den üretir.

**Tech Stack:** Düz HTML/CSS, esp-web-tools 10.1.1 (unpkg), Node test (`web/tests/`), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-30-system2-page-design.md`

## Global Constraints

- `web/index.html`'in davranışı değişmez; yalnızca 2. sisteme geçiş cümlesi (`p.small.or`, "Deck'li sisteme dönmek için") yeni metne döner.
- esp-web-tools sürümü ve adresi `web/index.html` ile aynı: `https://unpkg.com/esp-web-tools@10.1.1/dist/web/install-button.js?module`.
- Manifest: `manifest-full-ws5.json`. İndirmeler: `PlaneRadarDeck.exe`, `PlaneRadarDeck-mac.dmg`, `PlaneRadarDeck-mac-intel.dmg`. 1. sistem bağlantısı: `radar/`.
- `deck.html` içinde `navigator.serial` ve `mapdata.js` yok.
- Görünüş bugünkü sayfayla aynı: renk değişkenleri, `.card`, `.btn`, `.msg`, `.step`, `.small`, `.hint` kuralları `web/index.html`'den kopyalanır (yalnız kullanılanlar).
- Metinler Türkçe; kod yorumları ve commit mesajları İngilizce.

## Review Focus

- Safari / Firefox / telefon: esp-web-tools'un `unsupported` yuvası Türkçe uyarı göstermeli, indirme düğmeleri çalışmalı.
- Dar ekran (telefon, 375 px): yatay kaydırma olmamalı, düğmeler alt alta.
- `/radar` sayfası: CI'daki `sed` hâlâ `<head>` ve `<body>` satırlarını bulmalı (grep kontrolü).
- Program açıkken "Kart yazılımını yükle": metin "programdan Bağlantıyı bırak" demeli.
- Koyu/açık tema: bugünkü sayfa yalnız koyu; yeni sayfa da aynı.

---

### Task 1: Sayfa ve testi

**Files:**
- Create: `web/deck.html`
- Create: `web/tests/deck.test.js`
- Modify: `web/index.html` (2. sisteme geçiş cümlesi)

- [ ] **Step 1: Write the failing test** (`node web/tests/deck.test.js`, `mapdata.test.js` gibi `assert` ile, sonda "deck page ok")

```js
const fs = require('fs'), path = require('path'), assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, '..', 'deck.html'), 'utf8');
const idx = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8');
for (const s of ['manifest-full-ws5.json', 'href="PlaneRadarDeck.exe"', 'href="PlaneRadarDeck-mac.dmg"',
  'href="PlaneRadarDeck-mac-intel.dmg"', 'href="radar/"', 'esp-web-tools@10.1.1', 'slot="unsupported"',
  'Bağlantıyı bırak', 'BOOT'])
  assert(html.includes(s), 'deck.html: missing ' + s);
for (const s of ['navigator.serial', 'mapdata.js']) assert(!html.includes(s), 'deck.html: has ' + s);
assert(/^<head>$/m.test(idx) && /^<body>$/m.test(idx), 'index.html: CI sed needs <head> and <body> lines');
assert(idx.includes('Kartı hazırla'), 'index.html: the system 2 switch must point to "Kartı hazırla"');
console.log('deck page ok');
```

- [ ] **Step 2: Run** — `node web/tests/deck.test.js` → `ENOENT ... deck.html`
- [ ] **Step 3: Write `web/deck.html`** (tasarımdaki üç bölüm, alt bağlantı, kısa footer) ve `web/index.html`'deki `.or` geçiş cümlesini tasarımdaki metne çevir.
- [ ] **Step 4: Run** — `node web/tests/deck.test.js` → `deck page ok`; `node web/tests/mapdata.test.js` hâlâ geçer.
- [ ] **Step 5: Tarayıcıda bak** — yerel sunucu (`py -m http.server` `web/` içinde) ile `deck.html` geniş ve 375 px genişlikte; konsolda hata yok. `index.html`'e `data-sys="radar"` verilmiş kopyada geçiş cümlesi doğru.
- [ ] **Step 6: Commit** — `Setup page: own small page for system 2`

### Task 2: Yayın ve plan

**Files:**
- Modify: `.github/workflows/build.yml` (site adımı; test adımı)
- Modify: `docs/PLAN.md` (Kurulum satırı ve "Kurulum sayfaları" bölümü: kök `web/deck.html`, `/radar` `web/index.html`; 9. satır durumu)

- [ ] **Step 1:** Site adımı: `cp web/deck.html site/index.html`; `web/index.html` köke kopyalanmaz (yalnız `sed` ile `site/radar/index.html`); `mapdata.js`, `tz.json`, `tz-LICENSE.txt` kökte kalır. Kontrol satırı: `grep -q manifest-full-ws5.json site/index.html`. "Map tile tests" adımının yanına `node web/tests/deck.test.js`.
- [ ] **Step 2:** `docs/PLAN.md` güncelle.
- [ ] **Step 3: Commit + push (ws5) + PR** — `Site: system 2 page at the root, system 1 page at /radar`; CI yeşil, `site` listesinde iki sayfa.
- [ ] **Step 4: Yayından sonra (Birleştir ile):** kök sayfa ve `/radar` curl ile; kullanıcıyla kartta "Kartı hazırla" denemesi isteğe bağlı (kartı silmeden: yalnızca düğmenin açıldığını görmek yeterli).
