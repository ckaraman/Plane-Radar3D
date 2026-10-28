# Plan: iki sistem, tek repo

Tek kart (Waveshare ESP32-S3-Touch-LCD-5, 800×480), iki ayrı firmware. Biri yüklüyken diğerine
"Tam kurulum" ile geçilir; iki firmware aynı bellek düzenini (`partitions/plane_radar_16mb.csv`) kullanır.

| | 1. Sistem: WiFi radar | 2. Sistem: PC destekli radar + deck |
|---|---|---|
| Veri | Kart WiFi'den indirir (adsb.fi, Open-Meteo, adsbdb) | PC programı indirir, ayrıştırır, USB'den hazır gönderir |
| Kartta WiFi / TLS | Var | Yok (RAM ve işlemci çizime kalır) |
| Ekranlar | Radar, hava, saat, su | Radar, hava, saat, su, deck |
| Kurulum | Kurulum sayfası (Wi-Fi, konum, harita, güncelleme) | Sayfa sadece PC programını indirtir; gerisi programdan |
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
| `core/hardware/console` | USB klavye (deck) modu ile kurulum modu iç içe | 3 |
| `core/services/adsb_client` | Uçak listesi (ortak) + HTTPS indirme (1. sistem) | 6 |
| `core/services/weather_service` | Hava verisi (ortak) + indirme (1. sistem) | 6 |
| `core/services/route_service` | Rota verisi (ortak) + indirme (1. sistem) | 6 |
| `core/ui/settings_menu` | Ayarlar menüsü `wifi_setup`'a bağlı | 6 |

Ayrıca 4" ve 5B (1024×600) kalıntıları (`BOARD_WS5B`, `!BOARD_WS5` blokları, `fonts_tr.cpp`,
`fonts_tr_ws5b.cpp`, `ui_fonts_ws5b.cpp`) derlenmiyor; Aşama 3'te silinecek.

## Aşamalar

| # | Adım | Durum |
|---|---|---|
| 0 | Git, GitHub girişi, yeni repo | Tamam |
| 1 | Mevcut kod yeni repoda derleniyor ve yayında | Tamam |
| 2 | Tek dal, `core/net/deck/app` klasörleri, sadece 800×480 derlemesi (davranış aynı) | Bu adım |
| 3 | 1. sistem firmware'i: deck / USB klavye çıkar | |
| 4 | 1. sistem kurulum sayfası + kurtarma rehberi | |
| 5 | 2. sistem USB protokolünün yazılı tasarımı | |
| 6 | 2. sistem firmware'i: WiFi'siz, veri USB'den | |
| 7 | PC programı: veri motoru (uçuş, rota, hava, saat) | |
| 8 | PC programı: ayarlar, harita, firmware yükleme ve kurtarma | |
| 9 | 2. sistem kurulum sayfası + SignPath (imzalı exe) başvurusu | |
| 10 | Performans karşılaştırması, temizlik | |

## Kurtarma (kart açılmıyorsa)

ESP32-S3'ün silinemeyen yükleme modu var: BOOT'u basılı tut, RESET'e bas-bırak, BOOT'u bırak.
Sonra kurulum sayfasından "Tam kurulum".
