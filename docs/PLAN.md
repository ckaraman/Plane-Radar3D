# Plan: iki sistem, tek repo

Tek kart (Waveshare ESP32-S3-Touch-LCD-5, 800×480), iki ayrı firmware. Biri yüklüyken diğerine
"Tam kurulum" ile geçilir; iki firmware aynı bellek düzenini (`partitions/plane_radar_16mb.csv`) kullanır.

| | 1. Sistem: WiFi radar | 2. Sistem: PC destekli radar + deck |
|---|---|---|
| Veri | Kart WiFi'den indirir (adsb.fi, Open-Meteo, adsbdb) | PC programı indirir, ayrıştırır, USB'den hazır gönderir |
| Kartta WiFi / TLS | Var | Yok (RAM ve işlemci çizime kalır) |
| Ekranlar | Radar, hava, saat, su | Radar, hava, saat, su, deck |
| Kurulum | `…/Plane-Radar3D/radar/` (Wi-Fi, konum, harita, güncelleme) | Ana sayfa (`web/deck.html`): kartı hazırla, PC programını indir, kurtarma; gerisi programdan |
| PC kapalıyken | — | Saat ekranı (kart ayrıca besleniyorsa) |

## Kod düzeni

```
src/core/   iki firmware'in ortak kodu: ekran, dokunmatik, 3D radar, harita/arazi,
            hava/saat/su ekranları, fontlar, ayarlar, USB güncelleme
src/net/    sadece 1. sistem: WiFi bağlantısı, HTTPS (TLS)
src/deck/   sadece 2. sistem: deck ekranı, tuşlar, USB klavye, PC programıyla haberleşme
src/app/    ana döngü (main.cpp) ve USB kurulum komutları (serial_setup)
include/    config.h (pinler, zamanlamalar, sürüm) ve derlemede yazılan setup_url.h
helper/     PC programı (Windows, Mac M, Mac Intel)
web/        kurulum sayfası
```

Başlıklar (.h) kendi .cpp dosyalarının yanında. `platformio.ini` dört klasörü de include yoluna
ekler, bu yüzden `#include "services/adsb_client.h"` gibi satırlar değişmedi.

## İki tarafa bağlı dosyalar (ayrılacak)

Şimdilik `src/core` (veya `src/app`) içinde duruyorlar; ilgili aşamada bölünecekler.

| Dosya | Bağ | Ayrılacağı aşama |
|---|---|---|
| `app/services/serial_setup` | WiFi, deck tuşları, USB köprüsü komutları bir arada | 3 ve 6 |
| `core/hardware/console` | USB klavye (deck) modu ile kurulum modu iç içe | 3 (tamam: `WITH_DECK` yoksa hep kurulum modu) |
| `core/services/adsb_client` | Uçak listesi (ortak) + HTTPS indirme (1. sistem) | 6 |
| `core/services/weather_service` | Hava verisi (ortak) + indirme (1. sistem) | 6 |
| `core/services/route_service` | Rota verisi (ortak) + indirme (1. sistem) | 6 |
| `core/ui/settings_menu` | Ayarlar menüsü `wifi_setup`'a bağlı | 6 |

4" ve 5B (1024×600) fontları Aşama 3'te silindi. Koddaki `BOARD_WS5B` / `!BOARD_WS5` blokları
derlenmiyor; Aşama 10'da temizlenecek.

## Derleme ortamları (`platformio.ini`)

| Ortam | Ne | Sürüm |
|---|---|---|
| `ws5` | 2. sistem: radar + deck (`-DWITH_DECK`), Wi-Fi yok, `src/net` derlenmez; veri USB'den | `5in-…` (0.40.0'dan itibaren Wi-Fi'siz) |
| `radar` | 1. sistem: `-DWITH_WIFI`, `src/deck` derlenmez, USB hep kurulum (USB-Serial/JTAG) modunda | `radar-…` |

Deck'e ait her satır `#if defined(WITH_DECK)`, Wi-Fi'ye ait her satır `#if defined(WITH_WIFI)`
içinde. Sürümler `include/config.h` içinde.

## Aşamalar

| # | Adım | Durum |
|---|---|---|
| 0 | Git, GitHub girişi, yeni repo | Tamam |
| 1 | Mevcut kod yeni repoda derleniyor ve yayında | Tamam |
| 2 | Tek dal, `core/net/deck/app` klasörleri, sadece 800×480 derlemesi (davranış aynı) | Tamam |
| 3 | 1. sistem firmware'i: deck / USB klavye çıkar | Tamam (kartta denendi) |
| 4 | 1. sistem kurulum sayfası + kurtarma rehberi | Tamam (kartta denendi) |
| 5 | 2. sistem USB protokolünün yazılı tasarımı ([PROTOCOL.md](PROTOCOL.md)) | Tamam |
| 6 | 2. sistem firmware'i: WiFi'siz, veri USB'den (6a indirme kodu `src/net`'e, 6b ws5'ten Wi-Fi çıkar, 6c PREFS / sys / PC yok → saat, 6d sayfa) | Tamam (kartta ve sayfada denendi) |
| 7 | PC programı: veri motoru (7a uçak + rota `helper/deck/feed.py`, 7b hava + PREFS, 7c pencere ve bağlama) | Tamam (kartta denendi, Deck 1.5.0; kurulum sayfasından köprü adımı çıktı) |
| 8 | PC programı: 8a ayarlar (konum, saat dilimi, ekran) `SET`, 8b harita + arazi (sayfayla ortak JS dosyası), 8c yazılım güncelleme (OTA). Tam kurulum / kurtarma sayfada kalır (kullanıcı kararı) | Tamam (kartta denendi; 8a: ayarlar + adres araması, Deck 1.6.0; 8b: `web/mapdata.js`, Deck 1.7.0; harita verisi OpenFreeMap karolarından, önemli yerler Wikidata'dan, Overpass yalnızca yedek, Deck 1.8.0; 8c: program yeni kart yazılımını kendisi bulur, onayla USB'den yükler, Deck 1.9.0, firmware 5in-0.41.1 ile denendi) |
| 9 | 9a 2. sistem kurulum sayfası (`web/deck.html`; tamam, sitede doğrulandı; Deck 1.9.1 pencere metni); 9b Defender yanlış alarmı (`PlaneRadarDeck.exe` indirirken `Trojan:Win32/Sabsik.TE.A!ml`): kodlandı (kaynaktan derlenmiş PyInstaller başlatıcısı + exe sürüm bilgisi, Deck 1.9.2; tamam: sitede, Edge ile indirmede alarm yok. Kullanıcı daha önce bu dosya için karantinadan izin vermişti, sonucu etkilemiş olabilir; alarm geri gelirse klasör + zip) | Tamam |
| 10 | Akıcı deck ve radar (2. sistem): 10a ölçüm tamam (kart 5in-0.42.0 `PR:PERF`, Deck 1.10.0 `perf.log`; asıl yavaşlık programın kablo okumasında: 72 KB kapak 82 sn, bu sırada uçak partileri atlanıyor, radar 4 kare/sn); 10b-1 program okuması tamam (Deck 1.10.1; kartta: 72 KB kapak 2,8 sn, uçak partisi atlanmıyor, radarda kopma yok); 10b-2 kartta denendi (5in-0.43.0: radar 39-40 kare/sn, sayfa kurulumu 0,5-1 sn; sıradan şarkı seçince bağlantı takıldı); 10b-3 kodlandı (5in-0.44.0, Deck 1.11.0: takılma kaydı, sıra tıklaması ayrı iş parçacığında, küçük kapaklar şarkıya bağlı, USB çıkışı kurtarması, küçük sıra kapağı), kartta denendi (takılma, card gone, usbfix yok; şarkı geçişinde çoğunlukla 1 küçük kapak; ama sıra açıkken boş PSRAM yine 0,67 MB, kapak açılışı ~1 sn); 10b-4 kodlandı (5in-0.45.0: sıra ayrı 768 KB'lık ekran yerine müzik ekranını ödünç alır, beklenen boş PSRAM ~1,9 MB; sıra kapanınca müzik zemini yeniden çizilir), denenecek; tamam (kartta denendi: sıra açıkken boş PSRAM 1,39-1,53 MB, kopma ve ekran bozulması yok; kapak gelişinde hâlâ ~1 sn); B-0 ölçüm kartta denendi (5in-0.46.0, `dec_ms`: büyük kapak ~950 ms'nin ~200 ms'si JPEG açma, ~750 ms'si sayfaya işleme; sıra zemini 500-600 ms, JPEG açmasız); karartmayı kaldırmak yalnız %5-12 kazandırır (asıl iş arka plan); 10b-5 hızlandırma kodlandı (5in-0.47.0: arka plan satır satır, renk sıkıştırma 8 KB tablodan; pikseller aynı; bilgisayarda kapak sayfası %52, arka plan %39 süre), kartta denendi: JPEG dışı kurulum ~750 → ~530 ms (%30 kısa), JPEG ~160–215 ms, kapak başına toplam ~950 → ~700–750 ms; turn_max hâlâ ~800–880 ms; iç bellek 120 → 100 KB, PSRAM 1,43–1,49 MB, hata yok; B-1 kodlandı (5in-0.48.0: kapaklı sayfa kurulumu core 0'daki `cover` görevinde, ek tampon yok; kurulum sürerken müzik/sıra sayfası çizilmez, ekran son kareyi tutar; tuşlar sayfası açık kalır ama zemini yeniden kurulacaksa bekler; yeni kapak gelirse eskisi kurulum bitince silinir), tamam (kartta denendi: kapak gelirken `turn_max` ortanca 632 → 110 ms, en çok 884 → 175 ms; kalan ~80–110 ms sayfanın her karesi, 10c'nin işi; büyük kapak kurulumu core 0'da biraz yavaş: ~700–750 → ~770–820 ms, JPEG ~185–237 ms; sıra zemini ~280–390 → ~310–440 ms; iç bellek 100 → 87 KB (en az 80 KB), kurulum sırasında PSRAM en az 1,03 MB; takılma / hata kaydı yok); 10c temizlik (sıra açıkken 11 kare/sn de bakılacak); 10c-0 ölçüm kodlandı (5in-0.48.1, yalnız kayıt: `q` sıra açık kare sayısı, `draw` en yavaş karenin parçaları ms [zemin kopyası, üst parçalar, ekrana aktarma], `qpanel` [hazır kopyadan, baştan çizilen], `psram_blk` en büyük boş PSRAM parçası), kartta denendi: A yanlış (`psram_blk` 1,34–1,41 MB, panel çoğunlukla hazır kopyadan). `fps` aslında ekran turu sayısı; gerçek çizim sıra açıkken saniyede ~1 (`q` 5 / 5 sn). Her çizim pahalı: zemin kopyası (768 KB PSRAM→PSRAM) ~37 ms, üst parçalar müzik sayfasında ~57 ms, sırada ~44 ms (panel yeniden çizilirse ~88 ms), ekrana aktarma ~0; `turn_max` ~80–95 ms bundan. Sürekli değişen bir şey (kaydırma, ses çubuğu) olunca bu ~11 kare/sn demek. Kapak gelişi 2,3–3 sn (80 KB), o sırada müzik sayfası 4 kare/sn sınırında; 10c-1 kodlandı (5in-0.48.2: kapak gelirken sınır 250 → 66 ms, yani 15 kare/sn; `front` üst parçaları türüne göre ms [sekmeler, yazılar, çubuklar, düğmeler, sıradaki kartı / sıra paneli]), kartta denendi: sıra kapalıyken kapak 59–80 KB 2,2–3 sn (0.48.1 ile aynı, 66 ms zararsız); müzik sayfası karesi zemin 37 + sekmeler 16 + yazı 3 + çubuk 10 + düğme 9 + sıradaki kartı 16 ms; sıra açıkken (panel hazır) sekmeler 16 + düğme 8 + panel 14 ms. 11 kare/sn'nin sebebi bulundu: sıra açıkken yeni kapak gelince `tick()` müzik zemininin kapak numarasına bakıyor, o güncellenmediği için her tur tam kare çiziliyor (~85 ms); döngü yavaşlayınca kapak da yavaş geliyor (35 KB 15,6 sn) ve uçak partileri atlanıyor | Bu adım |

## Sıradaki oturum (10. aşama devamı)

- Dal: ws5. 10b-5 kartta denendi (sonuçlar yukarıda 10. aşama satırında).
- B-1 kartta denendi (sonuçlar yukarıda). Sıradaki: 10c. Sıra açıkken müzik sayfası 11–12 kare/sn, her tur ~80 ms (B-1'den bağımsız, 0.47'de de var).
- Sıra paneli şarkı seçince bazen kapanıyordu: program, tarayıcının sırası ile Windows'un "şu an çalan" bilgisi bir an uyuşmayınca karta QCLEAR gönderiyordu. Deck 1.11.1: uyuşmazlık 3 sn sürmeden sıra silinmez; silinince `perf.log`'a `queue clear` yazılır. Tamam (kartta denendi: ~2 dk'da 12 şarkı geçişinde silme yok, her geçişte 1 küçük kapak; başka uygulama çalınca 1 kez `queue clear ok`, YouTube Music'e dönünce sıra 12 sn sonra geri geldi).
- Bir kez takılma (5in-0.48.0, 15:24:53): kart tuşlar sayfasındayken müzik başladı, sıra gönderildi, sonra kart 15 sn hiç satır göndermedi; program ~30 sn'de kendiliğinden yeniden bağlandı, kart yeniden başlamadı. Kayıtlarda tek takılma bu. Sebep belli değil: B-1'in ilk kapak kurulumuyla aynı ana denk geliyor. Tekrarlarsa kartta takılma anını yazan bir kayıt eklenecek.
- 10c: sıra açıkken 11 fps, büyük kapak aktarılırken fps düşüşü. Tahminler: (A) sıra paneli hazır kopyası için ~265 KB tek parça PSRAM bulunamıyor, panel her karede baştan çiziliyor; (B) kapak gelirken müzik sayfası bilerek 4 kare/sn (`main.cpp`, 250 ms; 10b-1'den önceki yavaş okuma için konmuştu).
- 10c-2 önerisi (karar bekliyor): sıra açıkken `tick()` sıra zemininin kapak numarasına baksın (tek koşul). Sonra: sekmeler ve radar düğmesi (16 ms) zemine alınabilir.
- İkinci takılma (5in-0.48.2, 22:16:26–22:16:48): müzik sayfası, kapak ve "queue 30 rows" gönderiminden kısa süre sonra kart 15 sn satır göndermedi, `card gone`, 2 sn sonra yine bağlandı; kart yeniden başlamadı. 15:24 takılmasında da hemen önce "queue 30 rows" vardı. Kartta takılma anını yazan kayıt sırada.
- 10c-1 denemesi (yapıldı): kapak gelirken `rx_ms` ve "cover ... B ... s" süresi 0.48.1 ile karşılaştırılacak (80 KB 2,3–3 sn); `front` hangi parçanın 44–57 ms'yi doldurduğunu gösterir.
- 10c-1 adayları (kullanıcı 1 ve 3'ü seçti, 2 ölçüme göre): kapak gelirken 250 ms sınırını gevşetmek; yalnız değişen bölgeyi kopyalayıp çizmek (iki ekran tamponu sırayla gösterildiği için her bölge iki tampona da yazılmalı); üst parçaların süresini parça parça ölçmek.
- 10c-0 denemesi (yapıldı): sıra açıkken `qpanel` ikinci sayı büyükse ve `psram_blk` < ~265 KB ise A doğru; `draw` hangi parçanın uzun sürdüğünü gösterir. Kapak gelirken `rx_ms` ve `fps` not edilecek (B için karşılaştırma).
- Bilgisayarda `pio run -e ws5` bağlanma (link) hatası veriyor; kodla ilgisi yok, CI'a güven.
- Ölçüm: `%APPDATA%\PlaneRadarDeck\perf.log`; bakılacaklar `cover_ms`, `dec_ms`, `turn_max`, `heap`, `psram`.

## Kurulum sayfaları

- **Kök (2. sistem):** `web/deck.html` → `site/index.html`. Karta bağlanmaz (Web Serial yok):
  "Kartı hazırla" (esp-web-tools, `manifest-full-ws5.json`), PC programını indir, kurtarma
  (BOOT + RESET). Konum, harita, tuşlar ve güncelleme programda. Test: `web/tests/deck.test.js`.
- **`/radar` (1. sistem):** `web/index.html` → `site/radar/index.html`; kopyaya `<base href="../">`
  (dosyalar kökten okunur) ve `<body data-sys="radar">` eklenir. Sayfada `.or` sınıflı parçalar
  sadece 1. sistemde görünür; `.nr` parçaları artık hiç yayınlanmıyor (10. aşamada silinecek).
  1. sistem kartı ekranda `/radar` adresini gösterir (`include/config.h`).

## Kurtarma (kart açılmıyorsa)

ESP32-S3'ün silinemeyen yükleme modu var: BOOT'u basılı tut, RESET'e bas-bırak, BOOT'u bırak.
Sonra kurulum sayfasının sonundaki kurtarma kartından "Tam kurulum" (ayarlar silinir, harita
kalır; "Erase device" işaretlenirse harita da silinir).
