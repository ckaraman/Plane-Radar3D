# 9b: Defender yanlış alarmı — Uygulama planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Windows exe tek dosya kalsın ama kaynaktan derlenmiş PyInstaller başlatıcısı ve sürüm bilgisiyle üretilsin.

**Architecture:** `helper/version_info.py` `app.py`'deki `APP_NAME`/`VERSION`'dan PyInstaller sürüm dosyasını üretir. CI `deckapp` işi PyInstaller 6.22.3'ü kaynaktan (başlatıcıyla) kurar, başlatıcının hazır olandan farklı olduğunu kontrol eder ve `--version-file` ile derler.

**Tech Stack:** Python 3.12, PyInstaller 6.22.3, GitHub Actions `windows-latest` (MSVC), PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-30-defender-false-positive-design.md`

## Global Constraints

- PyInstaller `6.22.3`, `PYINSTALLER_COMPILE_BOOTLOADER=1`, `pip install pyinstaller==6.22.3 --no-binary pyinstaller`.
- Exe hâlâ `--onefile --windowed --name PlaneRadarDeck`; diğer bayraklar aynı.
- Sürüm bilgisi: ProductName ve FileDescription "Plane Radar Deck", CompanyName "Plane Radar 3D", OriginalFilename "PlaneRadarDeck.exe", InternalName "PlaneRadarDeck", LegalCopyright "Copyright (c) 2026 ckaraman", FileVersion/ProductVersion `VERSION` + ".0" (1.9.2 → "1.9.2.0", sayısal (1, 9, 2, 0)).
- `helper/app.py` `VERSION = "1.9.2"`.
- Defender ayarlarına dokunulmaz; yalnızca tarama.

## Review Focus

- Başlatıcı derlemesi CI'da sessizce hazır dosyaya düşerse: adım hata vermeli (Task 2 hash kontrolü).
- `VERSION` "1.10.0" gibi iki haneli: sayısal sürüm (1, 10, 0, 0) olmalı (Task 1 testi).
- Öz-testler (`--selftest --deep`, `--smoke`) kaynaktan derlenmiş başlatıcıyla da geçmeli.
- Mac derlemesi (`build_mac.sh`) etkilenmemeli.
- Exe boyutu bugünküne yakın kalmalı (~25 MB).

---

### Task 1: Sürüm dosyası üreticisi

**Files:**
- Create: `helper/version_info.py`
- Create: `helper/tests/test_build.py` (CI'da `test_firmware.py`'nin yanında çalışır)
- Modify: `.github/workflows/build.yml` (test satırı)

**Interfaces:**
- Produces: `version_info.text(name: str, version: str) -> str` — PyInstaller'ın `--version-file` biçimi (`VSVersionInfo(...)`); `python version_info.py` `app.py`'yi import etmeden (`ast` ile `APP_NAME`, `VERSION` okuyarak) metni stdout'a yazar.

- [ ] **Step 1: Failing test**

```python
def test_version_text():
    t = version_info.text("Plane Radar Deck", "1.10.0")
    ast.parse(t)  # PyInstaller eval()s it
    assert "filevers=(1, 10, 0, 0)" in t and "prodvers=(1, 10, 0, 0)" in t
    for s in ["'ProductName', 'Plane Radar Deck'", "'FileDescription', 'Plane Radar Deck'",
              "'CompanyName', 'Plane Radar 3D'", "'OriginalFilename', 'PlaneRadarDeck.exe'",
              "'FileVersion', '1.10.0.0'", "'ProductVersion', '1.10.0.0'"]:
        assert s in t, s

def test_reads_app():
    out = subprocess.run([sys.executable, "version_info.py"], cwd=HELPER, capture_output=True, text=True, check=True).stdout
    assert "'ProductVersion', '%s.0'" % VERSION_FROM_APP in out
```

- [ ] **Step 2: Run** — `py helper/tests/test_build.py` → `ModuleNotFoundError: version_info`
- [ ] **Step 3: Implement** `helper/version_info.py`; `VERSION`'ı 1.9.2 yap.
- [ ] **Step 4: Run** — `py helper/tests/test_build.py` → `tests ok`
- [ ] **Step 5: Commit** — `Deck: version info for the Windows exe`

### Task 2: CI — kaynaktan başlatıcı ve sürüm bilgisi

**Files:**
- Modify: `.github/workflows/build.yml` (`deckapp`: Install, yeni "Bootloader check", Build)

- [ ] **Step 1:** Install: `pip install -r requirements.txt` ve ayrı adımda `env: PYINSTALLER_COMPILE_BOOTLOADER: 1` ile `pip install pyinstaller==6.22.3 --no-binary pyinstaller`.
- [ ] **Step 2:** "Bootloader check" (pwsh): `pip download pyinstaller==6.22.3 --no-deps --only-binary :all: -d wheel`, wheel'den `PyInstaller/bootloader/Windows-64bit-intel/runw.exe` çıkar; kurulu `runw.exe` ile SHA-256 aynıysa `::error::` ve çık. Farklıysa `::notice::` ile iki hash.
- [ ] **Step 3:** Build: `python version_info.py > version.txt`; pyinstaller satırına `--version-file version.txt`. Öz-test adımına `(Get-Item dist\PlaneRadarDeck.exe).VersionInfo | Format-List ProductName, FileVersion, CompanyName` ekle.
- [ ] **Step 4:** `docs/PLAN.md` 9. satır: "9b kodlandı (kaynaktan başlatıcı + sürüm bilgisi, Deck 1.9.2), denenecek".
- [ ] **Step 5: Commit + push + PR** — `Deck 1.9.2: own PyInstaller bootloader and version info`; CI yeşil, `Bootloader check` notice'i ve VersionInfo satırları görünür.

### Task 3: Deneme (kullanıcıyla)

- [ ] **Step 1:** İzin isteyerek PR CI'ının `deck-windows` artifact'ını ve sitedeki bugünkü exe'yi scratchpad'e indir.
- [ ] **Step 2:** İkisini `"%ProgramFiles%\Windows Defender\MpCmdRun.exe" -Scan -ScanType 3 -File <exe> -DisableRemediation` ile tara; sonuçları yan yana yaz.
- [ ] **Step 3:** Sonuca göre: temizse "Birleştir"i öner; alarm sürerse 2. yolu (klasör + zip) konuş.
