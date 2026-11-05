# Spotify sırası kartta — uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** open.spotify.com'daki sıra panelini karta getirmek; YouTube Music sırası aynen çalışmaya devam ederken iki sekme açıksa çalan şarkıya uyan sıra seçilir.

**Architecture:** Eklentiye yeni `spotify.js` içerik betiği eklenir (ytmusic.js ile aynı mesaj biçimi + `src`). `background.js` sırayı sekme başına tutar ve mesajlara `tab` ekler. `browser_link.py` kaynakları `(bağlantı, sekme)` başına tutar, çalan şarkı adına göre birini "geçerli" seçer; dışarıya bugünkü `queue` / `art` / `queue_conn` alanlarıyla sunar, böylece `queue_card` ve `core` neredeyse değişmez.

**Tech Stack:** Chrome MV3 eklentisi (düz JS), Python 3 (helper, `websockets`), pytest.

**Spec:** `docs/superpowers/specs/2026-11-05-spotify-queue-design.md`

## Global Constraints

- Kart yazılımına (firmware, `src/`) dokunulmaz; kart protokolü (`QBEGIN/QITEM/QEND/QART/QPLAY`) aynı kalır.
- YouTube Music davranışı değişmez; `ytmusic.js` yalnızca `src` alanı eklenebilecek kadar değişir (gerekmiyorsa hiç).
- Eski eklenti (mesajda `tab` yok) yeni programla bugünkü gibi çalışır.
- Eklenti sürümü `1.2.0`; program sürümü `helper/app.py:17` `VERSION = "1.12.0"`.
- Seçiciler sayfa diline bağlı olmaz (`aria-label` metnine değil `role` / `data-testid`'e dayanır).
- Kod yorumları ve commit mesajları İngilizce; kullanıcıya görünen metinler Türkçe. Commit'ler `ws5` dalına; `main`'e birleştirme yok.
- Testler: `cd helper && python -m pytest tests -q`.

## Review Focus

- Spotify'da sıra paneli kapalı → `spotify.js` boş liste gönderir, kart listeyi kaldırır, büyük kapak yine gelir (Task 1 testi `test_source_art_only`).
- İki sekme açık, hiçbirinin şarkısı çalmıyor (ör. VLC çalıyor) → kartta liste yok, iki kaynak arasında gidip gelme yok (Task 1 `test_no_match_keeps_newest_and_card_rows_none`).
- Sekme kapanınca yalnız o sekmenin sırası düşer, öbürü kalır (Task 1 `test_tab_closed_drops_only_that_source`).
- Tarayıcı bağlantısı koparsa o bağlantının bütün kaynakları düşer (Task 1 `test_connection_closed_drops_its_sources`).
- Liste kaydıktan sonra dokunma → başlıkla doğru satır bulunur (spotify.js `play`; Task 3 elle deneme adımı).

---

### Task 1: browser_link — kaynak başına sıra ve seçim

**Files:**
- Modify: `helper/deck/browser_link.py` (`BrowserLink.__init__`, `status`, `_handler` finally bloğu, `_queue`, `play`; yeni `set_now`, `_select`)
- Test: `helper/tests/test_browser_sources.py` (yeni)

**Interfaces:**
- Produces:
  - Gelen `queue` mesajında isteğe bağlı `tab: int` ve `src: str` (ör. `"Spotify"`; yoksa `"YouTube Music"`).
  - `BrowserLink.set_now(title: str) -> None` — çalan şarkı adı (boş = hiçbir şey çalmıyor); seçimi yeniler, geçerli sıra değiştiyse `on_queue(self.queue)` çağırır.
  - Geçerli kaynak alanları (bugünkü adlar korunur): `self.queue` (`{"items", "cur", "now", "src"}` | None), `self.art` (`{"now", "art"}` | None), `self.queue_conn`, yeni `self.queue_tab` (int | None).
  - `play(index, title)` → `{"t":"play","i":..,"title":..}` + `"tab": queue_tab` yalnızca `queue_tab` None değilse.
  - Saf seçim yardımcısı (test edilebilir, bağlantı gerektirmez): `_select(sources: dict, now_title: str) -> key | None`. `sources` değeri `{"q": dict|None, "art": dict|None, "seq": int}`; kural: `q.items[q.cur].t` ya da `art.now` `queue_card.same_song(..., now_title)` olan kaynaklar içinden en büyük `seq`; yoksa en büyük `seq`.

- [ ] **Step 1: Failing testleri yaz** (`helper/tests/test_browser_sources.py`). `BrowserLink`'i ağ açmadan sınamak için `_queue(conn, msg)` ve bağlantı kapanışını çağıran küçük bir yardımcı kullan (sahte `conn = object()`; `__init__`'teki thread'leri açmamak için `BrowserLink.__new__` + alanları elle kur ya da `port` testlerdeki gibi 47900+). Testler:
  - `test_two_sources_pick_the_playing_one`: tab 1 YT (`now="Gece Yolu"`), tab 2 Spotify (`now="Bak Sen Keyfine"`, `src="Spotify"`, sonra geldi). `set_now("Gece Yolu")` → `link.queue["now"] == "Gece Yolu"`, `link.queue_tab == 1`; `set_now("Bak Sen Keyfine")` → `queue_tab == 2`, `link.queue["src"] == "Spotify"`.
  - `test_no_match_keeps_newest_and_card_rows_none`: iki kaynak, `set_now("Başka")` → `queue_tab == 2` (en yeni) ve `queue_card.card_rows(link.queue, {"t": "Başka"}) is None`; art arda 5 kez `set_now("Başka")` → `on_queue` en çok 1 kez çağrılmış.
  - `test_tab_closed_drops_only_that_source`: tab 2 için `{"t":"queue","items":[],"cur":-1,"tab":2}` → kaynak silinir, `set_now("Gece Yolu")` → `queue_tab == 1`.
  - `test_source_art_only`: `items=[]`, `now="X"`, `art="https://i.scdn.co/image/ab"` → `link.queue is None`, `link.art == {"now":"X","art":"https://i.scdn.co/image/ab"}`, `queue_conn` dolu.
  - `test_connection_closed_drops_its_sources`: iki bağlantı (A: tab 1, B: tab 2); A kapanınca yalnız B kalır.
  - `test_old_extension_without_tab`: `tab` yok → anahtar `(conn, None)`, `play(2,"Kıyıda")` mesajı `{"t":"play","i":2,"title":"Kıyıda"}` (tab yok).
- [ ] **Step 2: Çalıştır, düştüğünü gör:** `cd helper && python -m pytest tests/test_browser_sources.py -q` → FAIL (`set_now` yok).
- [ ] **Step 3: Uygula.** `self.sources = {}` ve `self._seq` (`itertools.count`); `_queue` kaynağı günceller (boş liste + art yok → kaynağı sil), sonra `_apply()` seçimi yapıp alanları kurar ve değiştiyse `on_queue` çağırır; `finally` bloğu o `conn`'un bütün kaynaklarını siler ve `_apply()`; `self.now_title` `set_now` ile tutulur. Hepsi `self.lock` altında, `on_queue` kilit dışında çağrılır. `status()["queue"]` geçerli sıranın uzunluğu kalır.
- [ ] **Step 4: Testler geçsin:** `cd helper && python -m pytest tests -q` → hepsi PASS (eski `test_core.py::test_browser_link` dahil, değiştirilmeden).
- [ ] **Step 5: Commit:** `git add helper/deck/browser_link.py helper/tests/test_browser_sources.py && git commit -m "Deck: browser queue per tab, pick the one that plays"`

### Task 2: core + queue_card — çalan şarkıyı bildir, Spotify kapağı ve kaynak adı

**Files:**
- Modify: `helper/deck/core.py` (`_media_changed`), `helper/deck/queue_card.py` (`PIC_HOSTS`, `send_queue` çağrısındaki sabit `"YouTube Music"`, modül açıklaması)
- Test: `helper/tests/test_browser_sources.py`

**Interfaces:**
- Consumes: Task 1 `BrowserLink.set_now(title)`, `link.queue["src"]`.
- Produces: kart `QBEGIN.src` alanı `link.queue.get("src") or "YouTube Music"`.

- [ ] **Step 1: Failing testler:**
  - `test_spotify_pictures_allowed`: `queue_card.fetch_picture` / `thumb_jpeg` host kontrolü `i.scdn.co`'yu kabul eder — host kontrolünü `_allowed(url) -> bool` adlı küçük fonksiyona çıkar ve test et: `_allowed("https://i.scdn.co/image/ab67") is True`, `_allowed("https://evil.example/x.jpg") is False`, `_allowed("https://i.ytimg.com/vi/a/hqdefault.jpg") is True`.
  - `test_queue_source_name`: `test_queue_thumbs.py`'deki sahte cihaz kalıbıyla `QueueSender` bir Spotify sırası gönderince `send_queue`'nun `source` argümanı `"Spotify"`; `src` yoksa `"YouTube Music"`.
- [ ] **Step 2: Çalıştır:** `cd helper && python -m pytest tests/test_browser_sources.py -q` → FAIL.
- [ ] **Step 3: Uygula:** `PIC_HOSTS` += `"scdn.co"`; `_allowed` iki yerde kullanılır; `send_queue(card, self.gen, (self.core.link.queue or {}).get("src") or "YouTube Music")`; `core._media_changed` başında `self.link.set_now("" if state.get("none") else state.get("t") or "")`.
- [ ] **Step 4:** `cd helper && python -m pytest tests -q` → PASS.
- [ ] **Step 5: Commit:** `git commit -am "Deck: Spotify covers and source name on the card queue"` (yeni test dosyası zaten izlenir).

### Task 3: Eklenti — spotify.js, sekme başına sıra, manifest

**Files:**
- Create: `helper/browser-extension/spotify.js`
- Modify: `helper/browser-extension/background.js`, `helper/browser-extension/manifest.json`

**Interfaces:**
- Consumes: Task 1 mesaj biçimi (`tab`, `src`).
- Produces: `spotify.js` → `{t:'queue', src:'Spotify', items:[{t,a,d:'',img}], cur:0|-1, now, art}`; `{t:'play', i, title}` / `{t:'refresh'}` dinler.

- [ ] **Step 1: spotify.js yaz** (ytmusic.js iskeletiyle: `push`, `soon`, `MutationObserver`, 3 sn / 30 sn zamanlayıcılar, `play`).
  - Panel: `[data-testid="now-playing-bar"]` olmayan ve içinde `ul[role=treegrid]` bulunan ilk `aside`. Satırlar: paneldeki tüm `ul[role=treegrid]`'lerin `li[role=row]`'ları sırayla. Seçiciler dosyanın başında tek bir `SEL` nesnesinde.
  - Satır: `innerText` boş olmayan satırlarından [0] başlık, [1] sanatçı; `img` = ilk `img.src` (http ile başlıyorsa).
  - `cur = items.length ? 0 : -1`; `now` = `navigator.mediaSession.metadata.title` (yoksa ilk satır); `art` = mediaSession kapaklarından en büyüğü (`sizes`'a göre).
  - `play(i, title)`: i'inci satır, başlık tutmazsa başlıkla ara; `[data-testid=play-button]`'a `click()`; 800 ms sonra `push(true)`.
- [ ] **Step 2: background.js** — `queueTab`/`lastQueue` yerine `const queues = new Map()` (tabId → son mesaj, `tab` alanı eklenmiş) ve `lastTab`. `onMessage`: mesaja `tab: sender.tab.id` ekle, sakla, gönder. `ws.onopen`: bütün `queues` değerlerini gönder. `onRemoved`: varsa sil ve `{t:'queue', items:[], cur:-1, tab:id}` gönder. `play`/`refresh`: `msg.tab` varsa o sekmeye, yoksa `lastTab`'a (`refresh` sekmesizse hepsine). Üst açıklama yorumu Spotify'ı da ansın.
- [ ] **Step 3: manifest.json** — `version` `"1.2.0"`; `host_permissions` ve yeni `content_scripts` girdisi `https://open.spotify.com/*` → `spotify.js`; `description`: `"YouTube Music ve Spotify sırasını Plane Radar kartına gösterir; kartın tuşları açık sekmeye geçer."`
- [ ] **Step 4: Elle deneme (Chrome, kullanıcıyla):** eklentiyi yenile; open.spotify.com'da bir liste çal, sıra panelini aç. Spotify sekmesinin konsolunda hata yok; yardımcı programın günlüğünde Spotify sırası geliyor (`status()["queue"]` > 0). Panel kapatılınca sıra 0'a düşüyor. Sayfa dili İngilizce'ye çevrilince de okunuyor (yapılabiliyorsa).
- [ ] **Step 5: Commit:** `git add helper/browser-extension && git commit -m "Extension 1.2.0: Spotify queue, one queue per tab"`

### Task 4: Ayarlar metni, sürüm, uçtan uca deneme

**Files:**
- Modify: `helper/web/index.html` (satır ~208 sıra seçeneği, ~263 eklenti açıklaması; Mac metni ~858 gerekmiyorsa dokunma), `helper/app.py:17`, `helper/deck/browser_link.py:1` ve `background.js` modül açıklamaları (Task 1/3'te yapılmadıysa)

- [ ] **Step 1: Metinler:** 208: `Sıradaki şarkıları kartta göster (YouTube Music, Spotify)`; 263 paragrafına bir cümle: `Spotify'da sıranın görünmesi için sağdaki "Sıradakiler" paneli açık olmalı.` ve "YouTube Music'te sıradaki şarkıları" → "YouTube Music ve Spotify'da sıradaki şarkıları". `VERSION = "1.12.0"`.
- [ ] **Step 2:** `cd helper && python -m pytest tests -q` → PASS.
- [ ] **Step 3: Uçtan uca deneme (kart + Chrome, kullanıcıyla):** (a) Spotify'da çal → kartta liste, kaynak "Spotify", kapaklar; satıra dokun → şarkı değişir, liste kayar. (b) YouTube Music sekmesi de açık, YT'de çal → kart YT listesine geçer; Spotify'a dön → Spotify listesi. (c) Spotify sekmesini kapat → liste kalkar. (d) Masaüstü Spotify'da çal + open.spotify.com sekmesi açık → liste görünüyor mu, not düş.
- [ ] **Step 4: Commit:** `git commit -am "Deck 1.12.0: Spotify queue on the card"`
