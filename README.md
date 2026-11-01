# Plane Radar 3D

Waveshare **ESP32-S3-Touch-LCD-5** (5", 800×480) kartında çalışan perspektif (3D) uçuş radarı. Uçuş verisi [adsb.fi](https://opendata.adsb.fi/), rota [adsbdb.com](https://www.adsbdb.com/), hava durumu [Open-Meteo](https://open-meteo.com/) üzerinden gelir. Firmware, kurulum sayfaları (`web/`) ve PC programı (`helper/`) tek dalda (`main`). Kod düzeni ve aşamalar: [docs/PLAN.md](docs/PLAN.md). Eski 4" sürüm ve geçmiş: [plane-radar-3D](https://github.com/ckaraman/plane-radar-3D) (arşiv).

Ekran, dokunmatik (GT911) ve ESP32-S3 (16 MB flash, 8 MB PSRAM) tek kartta; ekran için kablo bağlantısı gerekmez. 5B sürümü (1024×600) desteklenmez: bu yazılım ona yüklenirse görüntü karışık yatay çizgiler halinde gelir.

## İki sistem

Aynı kart için iki ayrı firmware var. Biri yüklüyken diğerine "Tam kurulum" ile geçilir.

| | 1. sistem: Wi-Fi radar | 2. sistem: PC destekli radar + deck |
|---|---|---|
| Veri | Kart Wi-Fi'den kendisi indirir | Bilgisayardaki Plane Radar Deck programı indirir, USB'den gönderir |
| Ekranlar | Radar, hava, saat, su | Radar, hava, saat, su, deck |
| Kurulum | [ckaraman.github.io/Plane-Radar3D/radar/](https://ckaraman.github.io/Plane-Radar3D/radar/) | [ckaraman.github.io/Plane-Radar3D](https://ckaraman.github.io/Plane-Radar3D/) |
| Sürüm | `radar-…` | `5in-…` |

Kurulum sayfaları yalnızca bilgisayarda **Chrome** veya **Edge** ile çalışır.

## Kurulum ve güncelleme

### 1. sistem (Wi-Fi radar)

- **İlk yükleme:** Kurtarma bölümündeki **Tam kurulum** ile. Kartı USB-C ile bağlayın, düğmeye basıp listede kartı seçin.
- **Ayarlar:** **Cihaza bağlan**, sonra Wi-Fi (yalnızca 2,4 GHz), konum, saat dilimi ve ekran seçenekleri, **Cihaza kaydet**. Wi-Fi ağı değişmediyse ve şifre girilmediyse kayıt Wi-Fi bağlantısına dokunmaz.
- **Harita ve önemli yerler:** **Cihaza kaydet** haritayı, arazi yüksekliklerini, önemli yerleri, yerleşim ve orman alanlarını indirip USB ile gönderir. Harita sunucuları yoğunsa sayfa önemli yerlerin alınamadığını söyler; birkaç dakika sonra tekrar kaydetmek yeter.
- **Güncelleme:** 3. adımdaki **Yazılımı güncelle**. Yeni yazılım çalışan karta USB kablosundan gönderilir ve ikinci yazılım yuvasına yazılır; BOOT gerekmez, ayarlar silinmez, sorun olursa eski yazılım çalışmaya devam eder. Sayfa `hw: ws5` bildirmeyen karta yazılım göndermez.

### 2. sistem (PC destekli radar + deck)

1. **Kartı hazırla:** Ana sayfadaki **Kart yazılımını yükle** (yeni kartta ve 1. sistemden geçişte BOOT gerekmez; kartın ayarları silinir).
2. **Programı indir:** Plane Radar Deck (Windows, Mac Apple M, Mac Intel). Kart çalıştığı sürece program açık kalmalı. Program kartı kendisi bulur; konum, ekran ayarları, harita, tuşlar ve kart yazılımı güncellemeleri programdan yapılır.
3. Program açıkken tarayıcıdan karta yazılım yüklenecekse önce programın simgesinden **Bağlantıyı bırak** deyin, bitince geri alın.

### Kurtarma (kart açılmıyorsa)

ESP32-S3'ün silinemeyen yükleme modu var, kart her zaman kurtarılabilir: kartın arka yüzündeki **BOOT**'u basılı tutun, **RESET**'e basıp bırakın, BOOT'u bırakın. Sonra kurulum sayfasının sonundaki kurtarma kartından **Tam kurulum** (ayarlar silinir, harita kalır; "Erase device" işaretlenirse harita da silinir). Bitince RESET'e bir kez basın.

## SHT30 bağlantısı (Rev1.2 kart)

Sensör alttaki yeşil vidalı klemense bağlanır (soldan sayarak):

| Klemens (soldan) | Yazı | SHT30 |
|---|---|---|
| 1 | VIN | **Bağlamayın** (7–36 V güç girişi) |
| 2 | GND | — |
| 3 | VOUT (3,3 V) | VIN / VCC |
| 4 | GND | GND |
| 5 | SDA (GPIO8) | SDA |
| 6 | SCL (GPIO9) | SCL |

Hat dokunmatik ve CH422G ile ortak, SHT30 adresi 0x44. Sensörü karta yapıştırmayın; kartın ısısı sıcaklığı yüksek gösterir. Sensör yine de yüksek okuyorsa `include/config.h` içindeki `kSht30TempOffsetC` değerini düzeltin (örneğin `-1.5f`).

## Radar ekranı

- **3B görünüm:** Uçaklar üç boyutlu model olarak çizilir. İrtifa ölçeği trafiğe göre otomatik ayarlanır. Uçakların arkasında düz noktalı iz, altlarında zemine inen çizgi var. Renkler: turuncu 10K ft altı, krem 10–25K ft, camgöbeği 25K ft üstü.
- **Harita:** 256 renkli zemin; tepeler kuzeybatıdan gelen ışığa göre gölgelenir, yükseldikçe yeşilden kuru ota ve kayaya döner. Deniz, göller, kıyı ve ana yollar (otoyollar kalın) çizilir. Yerleşim alanları gri, ormanlar koyu yeşil. Harita alanının dışı düz kara olarak görünür.
- **Havalimanları:** 60 km içindeki havalimanları kule/terminal simgesi ve üç harfli koduyla, pistleri yere yatık şerit olarak.
- **Önemli yerler:** İsim yerine simge: köprü, kule, stadyum, cami, tarihi yapı, müze, hayvanat bahçesi, park. En fazla 6 tane; önce her türden birer tane seçilir, sonra ikinciler (bu sırayla). Simgeye dokununca adı birkaç saniye görünür.
- **Dokunmatik kontrol:** Parmakla sağa/sola sürükleyince harita döner (360°), yukarı/aşağı sürükleyince eğilir (14–26°); hızlı bırakınca bir süre süzülür. İki parmakla açınca menzil küçülür, kapatınca büyür. Üst şeride dokunmak da menzili değiştirir. Su düğmesi sol altta, HAVA sağ altta.
- **Uçak seçme:** Dokunulan uçak seçilir ve kamera onu küçük adımlarla yumuşakça takip eder. Seçilen (ya da en yakın) uçağın nereden nereye gittiği (çağrı koduna göre; bazı uçuşlarda bulunamayabilir), irtifa, hız, mesafe, rota ve tırmanma/alçalma köşede görünür.

### Ayarlar menüsü

Ekrana 1,5 sn basılı tutunca açılır: Ekranı 180° çevir, Uçak etiketleri, İrtifa abartma, Menzil, Renkler (yüksek kontrast / normal), Uçak bilgisi (köşelerde / etikette), **Önemli yerler (göster / gizle)**. Kapasitif ekran olduğu için kalibrasyon, renk ters çevirme ve kırmızı/mavi seçenekleri yok; arka ışık yalnızca açık/kapalı.

## Diğer ekranlar

- **Saat:** Seçili menzilde 15 sn boyunca uçak yoksa açılır (menzil dışındaki uçaklar engellemez). Altında iç ortam (SHT30) ve dış ortam sıcaklık ve nemi gösterilir.
- **Hava durumu:** Radarın sağ altındaki HAVA düğmesiyle açılır; RADAR düğmesi geri döner. Dış ortamın şimdiki durumu (sıcaklık, hissedilen, nem, rüzgâr, gün doğumu/batımı), SHT30'dan ortam sıcaklığı ve nemi (rahatlık etiketiyle) ve 5 günlük tahmin (en yüksek/en düşük sıcaklık, yağış olasılığı).
- **Su takibi:** Damla düğmesiyle açılır. Günün toplamı, hedefe göre dolan damla avatar, saatlik çubuklar (önceki günle), son 7 gün; ‹ › ile geçmiş günler. 09:00–18:00 arası her saat başı ekran iki kez yanıp söner ve avatar "Su içtin mi?" diye sorar (o saatte zaten kayıt varsa sormaz, 10 dk sonra kapanır). Hedef cihazdan ayarlanır. Kayıtlar NVS'de: 21 gün saatlik, 366 gün günlük toplam.
- **Deck (yalnızca 2. sistem):** Radarda HAVA'nın solundaki ızgara düğmesiyle açılır; RADAR düğmesi geri döndürür. Tuşlar sayfasında 12 tuş ve sağda yuvarlak ses düğmesi var: saat yönünde çevirince ses açılır, ters yönde kısılır, dokununca sessize alır / açar. Müzik sayfası çalan şarkıyı kapağıyla ve sıradaki şarkıları gösterir. Tuşların resmi, yazısı ve işlevi (site, program ya da klasör açma, medya tuşları, tuş kombinasyonu, metin yazma) Plane Radar Deck programından ayarlanır.

## Teknik notlar

- Arduino çekirdeği 3.x (pioarduino 55.03.30, ESP-IDF 5.5). Arduino kütüphaneleri RGB ekran için önerilen ayarlarla yeniden derlenir (`custom_sdkconfig`: octal PSRAM, 64 bayt önbellek satırı, kod PSRAM'den, Wi-Fi tamponları iç bellekte). Derleme: PlatformIO 6.1.18 + `click<8.2` + `uv`, ayrı bir `PLATFORMIO_CORE_DIR` ile (bkz. `.github/workflows/build.yml`).
- Ekran sürücüsü esp_lcd RGB + "bounce buffer" (`src/core/hardware/bus_rgb_bounce.hpp`), **üç kare tamponu**: kareler ekranın göstermediği tampona çizilir, yırtılma olmaz ve çizim ekran tazelemesini beklemez.
- Kare 8 bitlik paletli bir tampona (PSRAM) çizilir, 256 girişli renk tablosuyla ekran tamponuna aktarılır.
- **Hızlandırmalar:**
  - Kamera dururken zemin, harita, halkalar ve simgeler ayrı bir tamponda saklanır; her karede yalnızca uçakların ve yazıların değiştirdiği bölgeler geri yüklenir.
  - Ekran 32×24 piksellik parçalara bölünür; yalnızca değişen parçalar ekran tamponuna yazılır.
  - Zemin 16 satırlık şeritler halinde hızlı iç bellekte çizilir.
  - Zemin iki çekirdekte birlikte çizilir (üst kısmı ikinci çekirdek); paylaşım ölçülen sürelere göre kendiliğinden ayarlanır. Görüntü kalitesi hareket sırasında da düşmez.
- Seri logda 30 saniyede bir `radar3d:` satırı: kare hızı, sabit/dönen kare sayısı, aşama süreleri (zemin, çizgiler, uçaklar, arayüz, aktarım), panel tazeleme hızı. 2. sistemde program ayrıca `%APPDATA%\PlaneRadarDeck\perf.log` yazar.
- Panel zamanlaması Waveshare'in kendi değerleri (16 MHz). Ayar: `src/core/hardware/lgfx_config.hpp`.
- Kartta BOOT tuşu kısayolu yok (GPIO0 ekran veri hattı).

## Derleme (geliştirici)

```
pio run -e ws5       # 2. sistem (5in-…)
pio run -e radar     # 1. sistem (radar-…)
```

`main`'e her gönderimde GitHub Actions iki firmware'i ve PC programını derler, kurulum sayfalarını yeni sürümle yayınlar. Her yeni sürümde `include/config.h` içindeki `kFirmwareVersion` değeri artırılır (iki sistemin ayrı satırı var). Pinler ve zamanlamalar `include/config.h`, renk paleti ve kamera ayarları `src/core/ui/radar3d.cpp` içinde.

## Türkçe karakterler

Ekran fontları GNU FreeFont'tan (FreeSans) üretilmiş bitmap fontlardır ve Türkçe harfleri (ÇĞİÖŞÜ çğıöşü) ile ° işaretini içerir. Yazılımdaki metinler UTF-8 yazılır. Fontları yeniden üretmek için:

```
pip install freetype-py
python3 tools/fonts/make_tr_fonts.py /usr/share/fonts/truetype/freefont
```

Başka bir harf gerekirse `tools/fonts/make_tr_fonts.py` içindeki `RANGES` listesine eklenir.
