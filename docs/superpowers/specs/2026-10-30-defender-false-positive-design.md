# 9b: Defender yanlış alarmı (Windows exe)

## Amaç

Defender, sitedeki `PlaneRadarDeck.exe`'yi **indirirken** `Trojan:Win32/Sabsik.TE.A!ml` diye
engelliyor (yapay zekâ tabanlı, dosyaya bakarak verilen karar; yanlış alarm). Kullanıcı programı
bugünkü gibi tek exe olarak indirip çalıştırabilmeli.

- **Hedef:** indirmede alarmın kalkması ya da belirgin azalması; kullanıcı için hiçbir şey değişmez.
- **Hedef değil:** imzalama (SignPath kaldırıldı), Mac sürümü, programın davranışı.
- **Olmaması gereken:** Defender'ı kapatmayı ya da istisna eklemeyi önermek; exe'nin
  "Windows ile başlat" yolunu değiştirmek.

## Yaklaşımlar

1. **Tek dosya kalır; PyInstaller başlatıcısı kaynaktan derlenir, exe'ye sürüm bilgisi eklenir
   (seçilen).** Exe'nin başındaki başlatıcı, zararlı yazılımlarla paylaşılan hazır başlatıcıdan
   farklı olur; dosya özelliklerinde ürün adı, sürüm, açıklama görünür.
2. Klasör + zip: "sonuna büyük veri eklenmiş exe" yapısı kalkar ama kullanıcı zip açar, başlangıç
   yolu değişir, eski kurulumdan geçiş gerekir. 1. yol yetmezse sıradaki.
3. Microsoft'a yanlış alarm bildirimi: kullanıcı yapar (hesap gerekir), yalnız o dosyayı kapsar;
   1. yola destek.

## Nasıl

CI, `deckapp` işi (`.github/workflows/build.yml`):

- PyInstaller sürümü sabitlenir: `6.22.3` (bugün CI'ın kullandığı). Kurulum
  `PYINSTALLER_COMPILE_BOOTLOADER=1` ile `pip install pyinstaller==6.22.3 --no-binary pyinstaller`;
  başlatıcı runner'daki Visual Studio derleyicisiyle derlenir.
- Derleme günlüğündeki "Bootloader …" satırı artık hazır `runw.exe` yerine kaynaktan derleneni
  göstermeli; adım bunu kontrol eder (hazır dosyanın SHA-256'sı ile karşılaştırma ya da
  derleme çıktısının varlığı).
- Sürüm bilgisi: `helper/app.py`'deki `APP_NAME` ve `VERSION`'dan üretilen bir sürüm dosyası,
  `pyinstaller --version-file` ile. Alanlar: ProductName "Plane Radar Deck",
  FileDescription "Plane Radar Deck", CompanyName "Plane Radar 3D", ProductVersion ve
  FileVersion `VERSION` (ör. 1.9.1.0), OriginalFilename "PlaneRadarDeck.exe",
  LegalCopyright depo lisansına göre.
- Mevcut öz-testler (`--selftest --deep`, `--smoke`) değişmeden çalışır.

Program sürümü 1.9.2 olur (yeni exe; içerik aynı).

## Deneme

1. Birleştirmeden önce: PR'ın CI'ından yeni exe indirilir (izin istenir), kullanıcının
   bilgisayarında Defender komut satırı taramasıyla (`MpCmdRun -Scan -ScanType 3 -File …`)
   taranır; karşılaştırma için sitedeki bugünkü exe de taranır. Dosya özelliklerinde sürüm
   bilgisi görünmeli, program açılmalı.
2. Birleştirince: siteden Edge ile indirme denenir (asıl şikâyet).

Tarama alarm verirse: 2. yolu (klasör + zip) konuşuruz; kullanıcı istersen Microsoft'a bildirim
yapar.

## Kısıtlar ve riskler

- Sonuç garanti değil: yapay zekâ tabanlı tespit yeni exe'yi de işaretleyebilir.
- Windows derlemesi birkaç dakika uzar.
- Defender ayarlarına dokunulmaz; tarama yalnızca okuma.

## Yapıcıya kalanlar

- Sürüm dosyasının üretim yeri (CI içinde tek satır ya da `helper/` altında küçük bir betik).
- Başlatıcının gerçekten kaynaktan derlendiğini gösteren kontrolün tam biçimi.
