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
| 10 | Akıcı deck ve radar (2. sistem): 10a ölçüm tamam (kart 5in-0.42.0 `PR:PERF`, Deck 1.10.0 `perf.log`; asıl yavaşlık programın kablo okumasında: 72 KB kapak 82 sn, bu sırada uçak partileri atlanıyor, radar 4 kare/sn); 10b-1 program okuması tamam (Deck 1.10.1; kartta: 72 KB kapak 2,8 sn, uçak partisi atlanmıyor, radarda kopma yok); 10b-2 kartta denendi (5in-0.43.0: radar 39-40 kare/sn, sayfa kurulumu 0,5-1 sn; sıradan şarkı seçince bağlantı takıldı); 10b-3 kodlandı (5in-0.44.0, Deck 1.11.0: takılma kaydı, sıra tıklaması ayrı iş parçacığında, küçük kapaklar şarkıya bağlı, USB çıkışı kurtarması, küçük sıra kapağı), kartta denendi (takılma, card gone, usbfix yok; şarkı geçişinde çoğunlukla 1 küçük kapak; ama sıra açıkken boş PSRAM yine 0,67 MB, kapak açılışı ~1 sn); 10b-4 kodlandı (5in-0.45.0: sıra ayrı 768 KB'lık ekran yerine müzik ekranını ödünç alır, beklenen boş PSRAM ~1,9 MB; sıra kapanınca müzik zemini yeniden çizilir), denenecek; tamam (kartta denendi: sıra açıkken boş PSRAM 1,39-1,53 MB, kopma ve ekran bozulması yok; kapak gelişinde hâlâ ~1 sn); B-0 ölçüm kartta denendi (5in-0.46.0, `dec_ms`: büyük kapak ~950 ms'nin ~200 ms'si JPEG açma, ~750 ms'si sayfaya işleme; sıra zemini 500-600 ms, JPEG açmasız); karartmayı kaldırmak yalnız %5-12 kazandırır (asıl iş arka plan); 10b-5 hızlandırma kodlandı (5in-0.47.0: arka plan satır satır, renk sıkıştırma 8 KB tablodan; pikseller aynı; bilgisayarda kapak sayfası %52, arka plan %39 süre), kartta denendi: JPEG dışı kurulum ~750 → ~530 ms (%30 kısa), JPEG ~160–215 ms, kapak başına toplam ~950 → ~700–750 ms; turn_max hâlâ ~800–880 ms; iç bellek 120 → 100 KB, PSRAM 1,43–1,49 MB, hata yok; B-1 kodlandı (5in-0.48.0: kapaklı sayfa kurulumu core 0'daki `cover` görevinde, ek tampon yok; kurulum sürerken müzik/sıra sayfası çizilmez, ekran son kareyi tutar; tuşlar sayfası açık kalır ama zemini yeniden kurulacaksa bekler; yeni kapak gelirse eskisi kurulum bitince silinir), tamam (kartta denendi: kapak gelirken `turn_max` ortanca 632 → 110 ms, en çok 884 → 175 ms; kalan ~80–110 ms sayfanın her karesi, 10c'nin işi; büyük kapak kurulumu core 0'da biraz yavaş: ~700–750 → ~770–820 ms, JPEG ~185–237 ms; sıra zemini ~280–390 → ~310–440 ms; iç bellek 100 → 87 KB (en az 80 KB), kurulum sırasında PSRAM en az 1,03 MB; takılma / hata kaydı yok); 10c temizlik (sıra açıkken 11 kare/sn de bakılacak); 10c-0 ölçüm kodlandı (5in-0.48.1, yalnız kayıt: `q` sıra açık kare sayısı, `draw` en yavaş karenin parçaları ms [zemin kopyası, üst parçalar, ekrana aktarma], `qpanel` [hazır kopyadan, baştan çizilen], `psram_blk` en büyük boş PSRAM parçası), kartta denendi: A yanlış (`psram_blk` 1,34–1,41 MB, panel çoğunlukla hazır kopyadan). `fps` aslında ekran turu sayısı; gerçek çizim sıra açıkken saniyede ~1 (`q` 5 / 5 sn). Her çizim pahalı: zemin kopyası (768 KB PSRAM→PSRAM) ~37 ms, üst parçalar müzik sayfasında ~57 ms, sırada ~44 ms (panel yeniden çizilirse ~88 ms), ekrana aktarma ~0; `turn_max` ~80–95 ms bundan. Sürekli değişen bir şey (kaydırma, ses çubuğu) olunca bu ~11 kare/sn demek. Kapak gelişi 2,3–3 sn (80 KB), o sırada müzik sayfası 4 kare/sn sınırında; 10c-1 kodlandı (5in-0.48.2: kapak gelirken sınır 250 → 66 ms, yani 15 kare/sn; `front` üst parçaları türüne göre ms [sekmeler, yazılar, çubuklar, düğmeler, sıradaki kartı / sıra paneli]), kartta denendi: sıra kapalıyken kapak 59–80 KB 2,2–3 sn (0.48.1 ile aynı, 66 ms zararsız); müzik sayfası karesi zemin 37 + sekmeler 16 + yazı 3 + çubuk 10 + düğme 9 + sıradaki kartı 16 ms; sıra açıkken (panel hazır) sekmeler 16 + düğme 8 + panel 14 ms. 11 kare/sn'nin sebebi bulundu: sıra açıkken yeni kapak gelince `tick()` müzik zemininin kapak numarasına bakıyor, o güncellenmediği için her tur tam kare çiziliyor (~85 ms); döngü yavaşlayınca kapak da yavaş geliyor (35 KB 15,6 sn) ve uçak partileri atlanıyor; 10c-2 kodlandı (5in-0.48.3: sıra açıkken `tick()` sıra zemininin kapak numarasına bakar; takılma kaydı: `PR:PERF` içinde sorun olunca `usb` {`txfull_ms` en uzun yazılamayan süre, `drop` atılan bayt, `rxgap_ms` PC'den en uzun sessizlik, `dtr_off`/`unmount`/`susp` port kapanma / USB gitme / askıya alma sayısı, `fix` usbfix, `before` en uzun yazılamama başlarken PC'nin son 4 satırının başı}; yerine ulaşmayan rapor sayılmaz, sonraki rapor takılmanın tamamını kapsar), tamam (kartta denendi: sıra açıkken 10 şarkı geçişinde `q` 5 sn'de 5–14 (0.48.2'de 57), kapak 35–82 KB 1,4–3 sn (0.48.2'de 35 KB 15,6 sn), `batch ... skipped` yok, sıra gönderimi 0,2 sn; takılma yok, tek `usb` kaydı bağlanırken 1,5 sn (`PR:HELLO` öncesi, zararsız); kalan: her kare ~80–100 ms (zemin 37, sekmeler 16, panel yeniden çizilirse ~70 ms)); 10c-3 kodlandı (5in-0.49.0: (1) sekmeler ve radar düğmesi müzik / sıra zemininde (tuşlar sayfasında zaten öyleydi), her karede ~16 ms az, pikseller aynı; (2) ekranın üç tamponunun her biri üstündeki zemini ve her parçanın durumunu hatırlar, bir karede yalnız o tamponda değişen parçaların dikdörtgeni zeminden kopyalanıp yeniden çizilir; parçalar: müzik sayfasında yazı, süre çubuğu, ses çubuğu, oynat düğmeleri, sıradaki kartı; sırada yazı, çubuk, düğmeler, panel; tuşlarda basılan tuş, düğme halkası, oynat düğmeleri; deck'e her girişte ve ters ekranda kayıt silinir, tam çizilir; `draw` içindeki zemin kopyası çoğu karede ~0, kopyalar artık `front` parçalarının içinde); kartta ölçüldü: müzik sayfası duruşta `turn_max` 82 → 5–7 ms, zemin kopyası 36 → 0 ms, sıra açıkken 5 ms; tamam (kartta denendi, iz / titreme yok); 10d-0 ölçüm kodlandı (5in-0.49.1, yalnız kayıt: `card` en yavaş karede sıradaki kartı / sıra panelinin parçaları ms [zeminden kopya (sırada hazır kopya dahil), küçük resim JPEG açma, cam (kart camı, satır vurgusu / çizgileri, kaydırma çubuğu), küçük resimler, yazılar]; 0.49.0 kaydında 10 küçük resmin JPEG'i toplam 15 ms, yani `front[4]`'ün 39–67 ms'si büyük ihtimalle resim yuvarlatma değil, kart camının `roundRect`'i), kartta ölçüldü: 0.49.0'daki 50–67 ms sıra açıkken panelin yeniden çizimiymiş, sıradaki kartı değil. Sıra kapalı, şarkı geçişi: `front[4]` 16–32 ms = cam 13 + JPEG 0–15 (10 küçük resim aynı karede) + resim 1 + yazı 1. Sıra açık, panel yeniden çizilince `front[4]` 52–69 ms = kopya 13 (zeminden + hazır kopyaya, her biri ~13 ms; kaydırırken ikisi 27) + JPEG 0–17 + cam 23 (çalan satırın vurgusu ~20, görünmeyince cam 3) + resim 2–11 + yazı 10–12. Sonuç: resimleri yuvarlatıp saklamak (eski (3)) yalnız 2–11 ms kazandırır; asıl yük `roundRect` camları, kopyalar ve 10 resmin tek karede açılması; 10d-1 kodlandı (5in-0.50.0: (a) `roundRect` kenardan 2 px içerideki piksellerde uzaklık hesabını atlar, yalnız dolgu doluysa karıştırır; `paint_test` eski kodla 28 şekilde 0 piksel fark, bilgisayarda ~%40 hızlı, bilerek bozulunca 5016 piksel fark yakaladı; (b) sıradaki kartının camı müzik zemininde (`s_music_card`): zemin kurulurken sıra varsa cam zemine girer; sonra sıra gelirse cam eskisi gibi karede çizilir; zeminde cam varken sıra giderse zemin yeniden kurulur), denenecek | Bu adım |

## Sıradaki oturum (10. aşama devamı)

- Dal: ws5. Kart 5in-0.50.0 (10d-1) ws5'te, main'de değil; 5in-0.49.1 (10d-0 ölçüm) main'de, kartta ölçüldü. 5in-0.49.0 (10c-3) main'de, kartta denendi: ölçüm ve gözle kontrol tamam (01.11). Deck 1.11.1.
- 10c-3 ölçümü: müzik sayfası duruşta `turn_max` 82 → 5–7 ms (`draw` [0,5,0], zemin kopyası 0); sıra açıkken 5 ms; tuşlar 1 ms. Şarkı geçişinde hâlâ ~80–120 ms: zemin yeniden kuruluyor (36 ms, beklenen) ve sıradaki kartı `front[4]` 50–67 ms; küçük resimler geldikçe sıra sürümü artıp kart her seferinde yeniden çiziliyor. Takılma yok.
- Ekranın üç tamponu var (PLAN'da önce iki yazıyordu); her biri kendi kaydını tutar (`s_fb`).
- 10d-1 (5in-0.50.0) kartta denenecek: perf.log'da sıra kapalı şarkı geçişinde `card` cam 13 → 0 (zeminde cam varsa), sıra açıkken panel camı 23 → daha az, `front[4]`; ayrıca `cover_n` şarkı başına artmadı mı (sıra şarkı geçişinde bir an boşalıyorsa zemin iki kez kurulur). Gözle: kartın camı, sekmeler, düğmeler, ses / süre çubukları, sıra paneli eskisi gibi mi, iz var mı. Kalan adaylar: panelin iki 13 ms'lik kopyası, küçük resimleri karede bir açmak.
- Eski aday (10d-0 öncesi): (3) sıra panelinin küçük resimlerini bir kez yuvarlatıp saklamak (panel yeniden çizimi ~55–70 ms, kaydırırken her kare); müzik sayfasındaki sıradaki kartı da aynı küçük resmi kullandığı için şarkı geçişindeki 50–67 ms'lik `front[4]` da düşer.
- Eskiden kalan, 10c-3'le ilgisiz: deck → radar dönüşünden sonra radar 15–20 sn boyunca 13–21 kare/sn, `turn_max` 115–170 (0.48.3'te 23:53 ve 01:22'de de var); tuşlar sayfasına ilk girişte bir kez `turn_max` ~700 ms (0.48.3'te 655).
- Takılma: 0.48.0 (15:24) ve 0.48.2 (22:16) birer kez; ikisinde de hemen önce "queue 30 rows". 0.48.3'te yok. Kart sorun olunca `PR:PERF` içine `usb` yazar (`txfull_ms`, `drop`, `rxgap_ms`, `dtr_off`/`unmount`/`susp`, `fix`, `before`); yerine ulaşmayan rapor sayılmaz, sonraki takılmanın tamamını kapsar. Bağlanırken 1,5 sn'lik `usb` kaydı (`before: PR:HELLO`) zararsız. Tekrarlarsa perf.log'da "stall"/"card gone" satırından sonra ilk ulaşan card satırındaki `usb` alanına bak.
- Ölçüm: `%APPDATA%\PlaneRadarDeck\perf.log` (dolunca `perf.log.1`). `fps` ekran turu sayısıdır, gerçek çizim `q` (sıra açık) ve `draw` / `front` (en yavaş karenin parçaları, ms). Bakılacaklar ayrıca `turn_max`, `cover ... B ... s`, `batch ... skipped`, `stall`, `card gone`.
- Bilgisayarda `pio run -e ws5` derliyor ama bağlanma (link) adımında kütüphane hatası veriyor; kodla ilgisi yok, CI'a güven. `helper/tests/test_core.py` Windows'ta çalışmaz (pty), CI'da çalışır.

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
