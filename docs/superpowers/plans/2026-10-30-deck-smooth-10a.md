# 10a: Ölçüm (akıcı deck ve radar) — Uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Kart beş saniyede bir ölçüm satırı (`PR:PERF`) göndersin; program bunu ve kendi gönderim sürelerini pencerede gösterip `perf.log`'a yazsın. Kartın sıra listesinden şarkı seçme mesajı (`QPLAY`) programa kesin ulaşsın. Başka davranış değişmez.

**Architecture:** Kartta yeni `src/deck/services/perf.{h,cpp}` sayaçları tutar; `main.cpp`, `serial_setup.cpp` ve `deck_screen.cpp` `WITH_DECK` içinde sayaçları besler. Programda yeni `helper/deck/perf.py` (`PerfLog`) dosyaya yazar; `device._line` `PERF` ve `QPLAY`'i `on_event`'e iletir; `core` ve `device` gönderim sürelerini `PerfLog`'a da yazar.

**Tech Stack:** ESP32-S3 Arduino/PlatformIO (C++17, ArduinoJson), Python 3.12 (pyserial), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-30-deck-smooth-design.md` (10a; QPLAY eklemesi kullanıcının "bazen çalmıyor" bildirimiyle)

## Global Constraints

- Kart yazılımı `5in-0.42.0` (`include/config.h`, `ws5`); `radar` sürümü değişmez (`radar-1.1.0`). Program `VERSION = "1.10.0"`.
- Kart değişiklikleri `#if defined(WITH_DECK)` içinde ya da `src/deck/` altında (`radar` ortamı `WITH_DECK`'i kaldırır, `src/deck`'i derlemez).
- Ölçüm satırı: `PR:PERF {"scr","page","fps","turn_max","turns","lines","cover_n","cover_ms","thumb_n","thumb_ms","rx_ms","heap"}`, 5000 ms'de bir; değerler o aralığa ait (her raporda sıfırlanır).
  - `scr`: `"radar" | "clock" | "weather" | "water" | "deck"`; `page`: deck açıkken `"music" | "keys"`, değilse `""`.
  - `fps`: aralıkta `updateScreen` sayısı / sn (bir ondalık); `turn_max`: en uzun `loop()` turu (ms); `turns`: tur sayısı; `lines`: işlenen kablo satırı; `cover_n/cover_ms`: büyük kapak `drawJpg` sayısı / toplam ms; `thumb_n/thumb_ms`: küçük kapak; `rx_ms`: `host::coverReceiving()` doğruyken geçen ms; `heap`: `ESP.getFreeHeap()`.
- Program `perf.log`: `config.APP_DIR/perf.log`, satır biçimi `YYYY-MM-DD HH:MM:SS <metin>`; 1 000 000 baytı geçince `perf.log.1`'e taşınır (eskisinin üzerine). Yazma hatası programı durdurmaz.
- Program `perf.log`'a yazdıkları: `card <PERF json>`, `cover <bayt> B <sn> s`, `thumb <k> <bayt> B <sn> s`, `queue <satır> rows <sn> s`.
- `docs/PROTOCOL.md` Deck bölümüne `PERF` satırı yazılır.
- Merge yalnızca kullanıcı "Birleştir" deyince.

## Review Focus

- Eski program + yeni kart: eski `device._line` bilinmeyen `PERF`'i sessizce atar (değişiklik yok, kontrol).
- `perf.log` yazılamıyor (klasör yok / izin yok): program çalışmaya devam eder (Task 1 testi).
- `PERF` satırı bir kapak/OTA aktarımı sırasında gelir: aktarım bozulmaz (`_line` önce aktarımın etiketlerine bakar; `PERF` onlarda yok) — Task 2 testi aktarım sırasında `PERF` söyler.
- `radar` derlemesi: CI'da `radar` ortamı da derlenir; `perf` çağrıları orada yok.
- Kartta ölçümün kendisi yük olmamalı: tur başına yalnızca `millis/micros` ve sayaç artışı; JSON yalnızca 5 sn'de bir.

---

### Task 1: Program — `PerfLog`

**Files:**
- Create: `helper/deck/perf.py`
- Create: `helper/tests/test_perf.py` (yerelde `py` ile ve CI'da `test_firmware.py` yanında)
- Modify: `.github/workflows/build.yml` (test satırı)

**Interfaces:**
- Produces: `perf.PerfLog(path: str, limit: int = 1_000_000)`; `.write(text: str) -> None` (zaman damgalı satır ekler, sınırı geçince döndürür, `OSError`'u yutar; iş parçacığına güvenli).

- [ ] **Step 1: Failing test**

```python
def test_writes_and_rotates():
    d = tempfile.mkdtemp(); p = os.path.join(d, "perf.log")
    log = perf.PerfLog(p, limit=200)
    log.write("card {\"fps\":30}")
    line = open(p, encoding="utf-8").read()
    assert re.match(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d card \{\"fps\":30\}\n$", line), line
    for i in range(20):
        log.write("x" * 20)
    assert os.path.exists(p + ".1") and os.path.getsize(p) <= 200

def test_unwritable_is_quiet():
    perf.PerfLog(os.path.join(tempfile.mkdtemp(), "yok", "yok", "perf.log")).write("a")  # no exception
```

- [ ] **Step 2: Run** `py helper/tests/test_perf.py` → `ModuleNotFoundError: deck.perf` (ya da `perf`)
- [ ] **Step 3: Implement** `helper/deck/perf.py`; build.yml'de `python helper/tests/test_build.py` altına `python helper/tests/test_perf.py`.
- [ ] **Step 4: Run** `py helper/tests/test_perf.py` → `tests ok`
- [ ] **Step 5: Commit** — `Deck: perf.log for card timing reports`

### Task 2: Program — `PERF` ve `QPLAY` iletimi, gönderim süreleri

**Files:**
- Modify: `helper/deck/device.py` (`_line` etiket listesi; `Device.__init__(..., perf=None)`; `send_cover`, `send_thumb`, `send_queue` süreleri)
- Modify: `helper/deck/core.py` (`PerfLog` kurulumu, `_card_event` `PERF`)
- Modify: `helper/app.py` (`VERSION = "1.10.0"`)
- Test: `helper/tests/test_core.py` (`test_all` içine; pty gerektirir, yalnız CI)

**Interfaces:**
- Consumes: `perf.PerfLog.write(text)` (Task 1).
- Produces: `Device(on_event, log=print, perf=None)` — `perf` bir `callable(str)` ya da `None`; `Core.perf: PerfLog` (`config.APP_DIR/perf.log`). `on_event` artık `QPLAY` ve `PERF` de alır.

- [ ] **Step 1: Failing test** (`test_all`, mevcut "Card -> helper events" bölümünün ardından)

```python
    card.say('PR:QPLAY {"x":3,"t":"Gece Yolu"}')
    card.say('PR:PERF {"scr":"deck","page":"music","fps":12.5,"turn_max":340}')
    assert wait(lambda: len(events) == 4), events
    assert events[2] == ("QPLAY", {"x": 3, "t": "Gece Yolu"})
    assert events[3][0] == "PERF" and events[3][1]["turn_max"] == 340
```

ve kapak aktarımı testinde aktarım sırasında bir `PERF` satırı (FakeCard `COVER` parçasını cevaplamadan önce `self.say('PR:PERF {"fps":4}')`, bir kez) — `send_cover` yine başarılı, `card.cover == jpeg`. Device'a `perf=perf_lines.append` verilir; `send_cover` sonrası `any(l.startswith("cover ") for l in perf_lines)`, `send_thumb` sonrası `thumb `, `send_queue` sonrası `queue ` satırı.

- [ ] **Step 2: Run** — CI (`python helper/tests/test_core.py`); yerelde yalnız `py -m py_compile helper/deck/device.py helper/deck/core.py`. Beklenen ilk CI: `AssertionError` (QPLAY olayı yok).
- [ ] **Step 3: Implement** — `_line`: `("KEY", "SETVOL", "MUTE", "QPLAY", "PERF")`. Süreler `time.time()` ile, başarılı gönderimde `self.perf(...)` (biçim Global Constraints'te). `Core`: `self.perf = perf.PerfLog(os.path.join(config.APP_DIR, "perf.log"))`, `Device(..., perf=self.perf.write)`; `_card_event` `PERF` → `self.log("perf: %s" % json.dumps(data))` ve `self.perf.write("card " + json.dumps(data, separators=(",", ":")))`.
- [ ] **Step 4: Run** `py helper/tests/test_perf.py; py helper/tests/test_firmware.py; py helper/tests/test_build.py` → hepsi `tests ok` (test_build `VERSION` 1.10.0'ı okur)
- [ ] **Step 5: Commit** — `Deck 1.10.0: card PERF reports and QPLAY reach the program; send timings`

### Task 3: Kart — `perf` sayaçları ve `PR:PERF`

**Files:**
- Create: `src/deck/services/perf.h`, `src/deck/services/perf.cpp`
- Modify: `src/app/main.cpp` (`loop()` tur süresi, `updateScreen` sayımı, `tick`), `src/app/services/serial_setup.cpp` (`handleLine` sayımı), `src/deck/ui/deck_screen.cpp` (üç `drawJpg` çevresi; `pageName()`), `src/deck/ui/deck_screen.h`, `include/config.h` (`5in-0.42.0`), `docs/PROTOCOL.md`

**Interfaces:**
- Produces (namespace `services::perf`): `void turn(uint32_t ms, bool cover_rx)`, `void frame()`, `void line()`, `void jpeg(bool thumb, uint32_t us)`, `void tick(const char* scr, const char* page)` — 5000 ms dolunca `PR:PERF` yazar ve sayaçları sıfırlar. `ui::deck::pageName() -> const char*` (`"music"`/`"keys"`).

- [ ] **Step 1:** `perf.{h,cpp}` (sayaçlar `static`, JSON `ArduinoJson` ile, `Console.printf("PR:PERF %s\n", ...)`).
- [ ] **Step 2:** `main.cpp` `loop()` başında `const unsigned long t_turn = millis();`; `BOARD_WS5` dalında `return`'den önce `WITH_DECK` içinde `perf::turn(millis() - t_turn, host::coverReceiving())` ve `perf::tick(<g_screen adı>, g_screen == Screen::Deck ? ui::deck::pageName() : "")`; `updateScreen()` çağrısından sonra `perf::frame()`. OTA dalı ölçülmez.
- [ ] **Step 3:** `serial_setup.cpp` `handleLine` başında `WITH_DECK` içinde `perf::line()`; `deck_screen.cpp` üç `drawJpg` çağrısı `micros()` ile çevrilir → `perf::jpeg(false|true, us)`.
- [ ] **Step 4:** `config.h` `5in-0.42.0`; `PROTOCOL.md` Deck bölümüne `PERF` (kart → PC, 5 sn, alanlar Global Constraints'teki gibi; program günlüğe ve `perf.log`'a yazar, eski program yok sayar).
- [ ] **Step 5: Run** — yerelde PlatformIO yok: derleme CI'da (`ws5` ve `radar` iki ortam). Push öncesi `git diff` ile `radar` derlemesine giren dosyalarda (`main.cpp`, `serial_setup.cpp`) her `perf::` çağrısının `WITH_DECK` içinde olduğu okunarak kontrol edilir.
- [ ] **Step 6: Commit** — `Card 5in-0.42.0: PR:PERF timing report every 5 s`

### Task 4: PLAN, push, PR

- [ ] **Step 1:** `docs/PLAN.md` 10. satır: "10a ölçüm kodlandı (kart 5in-0.42.0 `PR:PERF`, Deck 1.10.0 `perf.log`, QPLAY iletimi), kartta denenecek; 10b gereksiz yük, 10c temizlik" — durum "Bu adım".
- [ ] **Step 2:** Commit + push ws5 + PR (base main) — `Stage 10a: timing reports from the card, perf.log`. CI yeşil: `ws5` ve `radar` derlenir, `test_core.py` geçer.
- [ ] **Step 3 (kullanıcıyla, birleştirince):** program 1.10.0 indirilir, kart programdan 0.42.0'a güncellenir; radar açıkken ve deck müzik sayfası açıkken birkaç şarkı geçişi; `perf.log` okunur, "önceki" sayılar yazılır.
