# 8c: Kart yazılımını PC programından güncelleme — Uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** PlaneRadarDeck, 2. sistem kartı için yeni yazılımı kendisi fark edip haber versin ve kullanıcı onaylayınca kartı USB'den güncellesin.

**Architecture:** Yeni `helper/deck/firmware.py` sürüm denetimini ve indirmeyi yapar (saf fonksiyonlar). `Device.upload_firmware` kurulum sayfasının OTA akışını (`web/index.html` → `usbUpdate`) mevcut `_in_setup_mode` yolunun içinde uygular. `Core` 6 saatte bir denetler ve durumu `self.fw` sözlüğünde tutar; pencere bunu `live()` ile okur, tepsi menüsü ve bildirimi de oradan.

**Tech Stack:** Python 3 (pyserial, urllib, pystray, pywebview), pencere `helper/web/index.html` (düz JS).

**Spec:** `docs/superpowers/specs/2026-10-30-deck-card-update-design.md`

## Global Constraints

- Kart tarafında değişiklik yok; protokol `src/core/services/serial_ota.cpp` neyse o.
- Sürüm dosyası: `https://ckaraman.github.io/Plane-Radar3D/version.json`, anahtar `version_ws5`; yazılım: `https://ckaraman.github.io/Plane-Radar3D/firmware-ws5.bin?v=<sürüm>`.
- Yalnızca `HELLO`'da `sys == 2` ve `hw == "ws5"` olan karta güncelleme önerilir ve yüklenir.
- Denetim: açılışta (kart bağlanınca) ve 6 saatte bir. Ağ hatası sessiz geçilir.
- Sormadan güncelleme yok. "Şimdi değil" o sürümü `settings["fw_skip"]`'e yazar; o sürüm için bildirim bir daha çıkmaz, şerit sönük kalır.
- Kullanıcıya görünen metinler Türkçe; kod yorumları ve commit mesajları İngilizce.
- Program sürümü `helper/app.py` `VERSION` → `"1.9.0"`.
- Testler: `helper/tests/test_firmware.py` (pty yok, Windows'ta `py helper/tests/test_firmware.py` ile koşar); protokol testi `helper/tests/test_core.py` (pty, yalnızca CI / Linux).

## Review Focus

- Güncelleme sürerken kablo çıkar: hata mesajı, `fw["state"]` "error" olur, program kilitlenmez; kart sonra takılınca izleyici klavye moduna döndürür.
- Kart yeniden başladıktan sonra kurulum modunda açılır: program `USBNORMAL` ile klavye moduna döndürmeli; yeni `fw` sürümü doğrulanmalı (Task 2 testi).
- Kart `OTANAK` ile geri sarar: doğru parçadan devam edilmeli (Task 2 testi, `drop_once`).
- `version.json` erişilemez ya da `version_ws5` yok: şerit çıkmaz, hata gösterilmez (Task 1 testi).
- Güncelleme sürerken tuş/harita gönderimi istenir: `busy` nedeniyle reddedilir ("Başka bir gönderim sürüyor").

---

### Task 1: Sürüm denetimi ve indirme (`firmware.py`)

**Files:**
- Create: `helper/deck/firmware.py`
- Create: `helper/tests/test_firmware.py`
- Modify: `.github/workflows/build.yml` (Helper protocol tests adımına `python helper/tests/test_firmware.py` satırı)

**Interfaces:**
- Produces:
  - `SITE = "https://ckaraman.github.io/Plane-Radar3D/"`
  - `latest_version(get_json=feed.get_json) -> str | None` — `version.json`'dan `version_ws5`; her hata `None`.
  - `offer(hello: dict | None, latest: str | None) -> bool` — kart bağlı, `sys == 2`, `hw == "ws5"`, `latest` dolu ve `hello["fw"] != latest`.
  - `download(version: str, fetch=_fetch_bytes) -> bytes` — `firmware-ws5.bin?v=<version>`; 4096 bayttan küçükse ya da ilk bayt `0xE9` (ESP görüntü başı) değilse `RuntimeError("indirilen dosya bozuk")`; HTTP/ağ hatası `RuntimeError("indirme: <neden>")`.

- [ ] **Step 1: Write the failing tests** (`test_firmware.py`, `if __name__ == "__main__"` ile hepsini çağırıp "tests ok" yazar, `test_core.py` gibi)

```python
def test_latest_version():
    assert firmware.latest_version(lambda url: {"version_ws5": "5in-0.42.0"}) == "5in-0.42.0"
    assert firmware.latest_version(lambda url: {}) is None
    def boom(url): raise feed.HttpError("yok")
    assert firmware.latest_version(boom) is None

def test_offer():
    card = {"fw": "5in-0.41.0", "sys": 2, "hw": "ws5"}
    assert firmware.offer(card, "5in-0.42.0")
    assert not firmware.offer(card, "5in-0.41.0")
    assert not firmware.offer(card, None)
    assert not firmware.offer(None, "5in-0.42.0")
    assert not firmware.offer(dict(card, sys=1), "5in-0.42.0")
    assert not firmware.offer(dict(card, hw="ws5b"), "5in-0.42.0")

def test_download():
    good = b"\xe9" + bytes(5000)
    urls = []
    assert firmware.download("5in-0.42.0", lambda u: urls.append(u) or good) == good
    assert urls == ["https://ckaraman.github.io/Plane-Radar3D/firmware-ws5.bin?v=5in-0.42.0"]
    for bad in (b"\xe9" + bytes(100), b"<html>" + bytes(5000)):
        try:
            firmware.download("x", lambda u: bad); assert False
        except RuntimeError as e:
            assert "bozuk" in str(e)
```

- [ ] **Step 2: Run to verify it fails** — `py helper/tests/test_firmware.py` → `ModuleNotFoundError: deck.firmware`
- [ ] **Step 3: Implement `helper/deck/firmware.py`** — `_fetch_bytes(url)` urllib ile, `feed.USER_AGENT`, `cache` kırmak için `Cache-Control: no-cache` başlığı, zaman aşımı 30 sn.
- [ ] **Step 4: Run to verify it passes** — `py helper/tests/test_firmware.py` → `tests ok`
- [ ] **Step 5: CI satırını ekle, commit** — `Deck: firmware version check and download`

### Task 2: OTA gönderimi (`Device.upload_firmware`)

**Files:**
- Modify: `helper/deck/device.py` (yeni metot `_in_setup_mode`'un altına)
- Modify: `helper/tests/test_core.py` (FakeCard'a OTA, `test_all` sonuna senaryo)

**Interfaces:**
- Consumes: `_in_setup_mode(work, progress)`, `request`, `send`, `_stream` kuyruğu, `_close`, `_open(port, setup=True)`, `find_port(PID_SETUP)`.
- Produces: `Device.upload_firmware(blob: bytes, progress=lambda pct, text: None) -> str` — karttaki yeni `fw` sürümünü döner; hata `RuntimeError(<Türkçe neden>)`.

Akış (`work()` içinde, sayfadaki değerler):
1. `PR:OTABEGIN {"len":n,"crc":crc32}` → `OTAACK`/`ERR`, 20 sn; zaman aşımı ya da `bad_json` ise 5 denemeye kadar `1.5 * deneme` sn bekleyip tekrar.
2. 480 baytlık parçalar, **tek tek**: `PR:OTA <seq> <len> <base64>`, cevap 4 sn. `OTAACK seq==gönderilen` → sonraki; `OTANAK {"seq":k}` → `k`'dan devam; `ERR` → hata; cevapsız → tekrar. Üst üste 8 başarısız deneme → `RuntimeError("kart yanıt vermedi")`. Cevaplar `_stream = ({"OTAACK","OTANAK","ERR"}, q)` ile okunur (`_transfer` gibi).
3. `PR:OTAEND` → `OTADONE`/`ERR`, 30 sn.
4. Kart yeniden başlar: `_close()`, `find_port(PID_SETUP)` 30 sn beklenir, `_open(port, setup=True)`; böylece `_in_setup_mode` kendi `USBNORMAL` adımını yeni açılan portta yapar.
5. `work()` dışı: hata olursa ve port açıksa `PR:OTAABORT` gönderilir (sonra `_in_setup_mode`'un kendi temizliği). Bağlanınca `hello["fw"]` döner.
İlerleme: 2–8 geçiş, 10–90 gönderim (`"Yazılım karta gönderiliyor: %<n>. Kabloyu çıkarmayın."`), 92 `"Doğrulanıyor…"`, 95 `"Kart yeniden başlıyor…"`, 100 `"Kart güncellendi."`.

- [ ] **Step 1: FakeCard'a OTA ekle ve testi yaz** — `OTABEGIN` meta saklar, `OTAACK {"seq":0}`; `OTA` parçada `drop_once` ise seq 3'ü bir kez yutup bir sonrakinde `OTANAK {"seq":3}` der, sıradaysa ekler ve `OTAACK`; `OTAEND` CRC/len doğruysa `self.fw_written = stage`, `fw` değerini `"5in-0.42.0"` yapar, `OTADONE {}` (HELLO artık bu `fw`'yu ve `usb` "setup"'ı bildirir); değilse `ERR {"msg":"ota_crc"}`.

```python
    # Firmware over USB (stage 8c): one chunk at a time, a lost one sent again, restart.
    fw = b"\xe9" + bytes(range(256)) * 20
    card.drop_once = True
    steps = []
    assert d.upload_firmware(fw, lambda p, t: steps.append(p)) == "5in-0.42.0"
    assert card.fw_written == fw and steps[-1] == 100 and card.usb == "kb"
    assert wait(lambda: d.connected)
```

- [ ] **Step 2: Run to verify it fails** — CI'da (ya da Linux'ta) `python helper/tests/test_core.py` → `AttributeError: upload_firmware`. Windows'ta pty olmadığından bu test yerelde koşmaz; adım commit + push sonrası CI'da doğrulanır.
- [ ] **Step 3: Implement `upload_firmware`**
- [ ] **Step 4: Run** — CI "Helper protocol tests" adımı yeşil, `tests ok`.
- [ ] **Step 5: Commit** — `Deck: card firmware update over USB`

### Task 3: Denetim döngüsü ve durum (`Core`)

**Files:**
- Modify: `helper/deck/core.py`
- Modify: `helper/deck/config.py` (`DEFAULT_SETTINGS["fw_skip"] = ""`)
- Modify: `helper/tests/test_firmware.py`

**Interfaces:**
- Consumes: `firmware.latest_version`, `firmware.offer`, `firmware.download`, `Device.upload_firmware`.
- Produces:
  - `Core.fw = {"latest": str|None, "offer": bool, "skipped": bool, "running": bool, "pct": int, "text": str, "error": str, "done": str}`
  - `Core.on_fw_offer = lambda version: None` — yeni bir sürüm ilk kez önerildiğinde (atlanmamışsa) bir kez çağrılır (tepsi bildirimi için).
  - `Core.check_firmware(latest_version=firmware.latest_version) -> None` — `fw["latest"]`/`offer`/`skipped` günceller, gerekirse `on_fw_offer`.
  - `Core.update_firmware() -> bool` — iş parçacığında indir + `upload_firmware`; zaten sürüyorsa ya da `offer` yoksa `False`.
  - `Core.skip_firmware() -> None` — `settings["fw_skip"] = fw["latest"]`, kaydeder, `skipped = True`.
- Döngü: `_fw_loop` iş parçacığı; kart bağlanınca ilk denetim, sonra 6 saatte bir (21600 sn); `device.busy` iken atlar.
- Güncelleme sırasında `fw["running"]` True; bitince `done` = yeni sürüm, `offer` False. Hata: `error` = mesaj, `running` False.

- [ ] **Step 1: Failing test** — `Core` yerine küçük bir sahte ile değil, `check_firmware`'ı saf tutmak için mantığı `firmware.state(hello, latest, skip) -> {"offer","skipped"}` yardımcı fonksiyonuna koy ve onu test et:

```python
def test_state():
    card = {"fw": "5in-0.41.0", "sys": 2, "hw": "ws5"}
    assert firmware.state(card, "5in-0.42.0", "") == {"offer": True, "skipped": False}
    assert firmware.state(card, "5in-0.42.0", "5in-0.42.0") == {"offer": True, "skipped": True}
    assert firmware.state(card, "5in-0.41.0", "") == {"offer": False, "skipped": False}
```

- [ ] **Step 2: Run** — `py helper/tests/test_firmware.py` → `AttributeError: state`
- [ ] **Step 3: Implement** `firmware.state` ve `Core` eklerini; `on_fw_offer` yalnızca `offer and not skipped` iken ve aynı sürüm için bir kez.
- [ ] **Step 4: Run** — `py helper/tests/test_firmware.py` → `tests ok`
- [ ] **Step 5: Commit** — `Deck: check for card firmware every 6 hours`

### Task 4: Pencere, tepsi, sürüm, plan

**Files:**
- Modify: `helper/app.py` (`Api.live`/`state` → `"fw": dict(core.fw)`; `Api.fw_update()`, `Api.fw_skip()`; tepsi; `VERSION = "1.9.0"`)
- Modify: `helper/deck/tray_mac.py` (menü satırı)
- Modify: `helper/web/index.html` (şerit + ilerleme)
- Modify: `docs/PLAN.md` (8. satır: 8c tamam değil, "kodlandı, kartta denenecek", Deck 1.9.0)

**Interfaces:**
- Consumes: `Core.fw`, `Core.update_firmware`, `Core.skip_firmware`, `Core.on_fw_offer`.

Kararlar:
- **Şerit:** `<main>`'in en üstünde, tüm sayfalarda görünen `#fw-bar` (`.msg` sınıfı). Metinler:
  - öneri: `"Kart için yeni yazılım var: <latest> (kartta <fw>)."` + **Güncelle** + **Şimdi değil**; atlanmışsa sönük (`opacity .6`), yalnızca **Güncelle**.
  - sürerken: `<progress>` + `fw.text`; Güncelle düğmesi kapalı.
  - bitti: `"Kart güncellendi (sürüm <done>)."` (ok); hata: `"Güncellenemedi: <error>. Kart eski yazılımla çalışmaya devam eder; tekrar deneyin."` (err).
  - Güncelle'ye basınca `confirm("Kartın yazılımı güncellenecek (yaklaşık 2 dakika). Bu sırada kabloyu çıkarmayın. Devam edilsin mi?")`.
  - `applyLive` içinde `st.fw`'dan çizilir.
- **Tepsi (Windows, pystray):** `"Kart yazılımını güncelle…"` satırı, `visible=lambda _: core.fw["offer"] and not core.fw["running"]`; tıklayınca pencere açılır (`open_editor`). `core.on_fw_offer = lambda v: tray.notify("Kart için yeni yazılım var: %s" % v, APP_NAME)`.
- **Tepsi (Mac):** aynı satır `MacTray`'e `fw_offer` callable ile; `refresh` içinde `setHidden_`. Bildirim Mac'te yok (yalnızca günlüğe yazılır) — pystray bildirimi orada kullanılmıyor.

- [ ] **Step 1: Uygula** (yukarıdaki dosyalar)
- [ ] **Step 2: Doğrula** — `py helper/tests/test_firmware.py` → `tests ok`; `py -m pyflakes helper/app.py helper/deck` (kuruluysa) hata yok; pencereyi `py helper/app.py` ile aç, kart bağlıyken ve `version_ws5` farklıyken şeridin çıktığını gör.
- [ ] **Step 3: Commit + push (ws5)** — `Deck 1.9.0: update the card firmware from the program`; CI'daki `test_core.py` ve exe derlemesi yeşil.
- [ ] **Step 4: Kartta deneme** — kullanıcı ile: yeni exe'yi kur, karttaki sürümü bir eski sürümde tut (ya da CI'nın yayınladığı sürümle farklı olsun), bildirim → Güncelle → kart yeni sürümle açılır, tuşlar çalışır. Sonuç `docs/PLAN.md`'ye "8c tamam (kartta denendi)".
