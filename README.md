# Plane Radar 3D

> **Bu dal (`ws5`) 5" Waveshare ESP32-S3-Touch-LCD-5 sürümüdür.** 4" ESP32-WROOM sürümü `main` dalında. Kurulum sayfası (`web/`) ve yayın `main` dalından yapılır; bu dala gönderim, `main`'deki derlemeyi başlatır ve iki modelin yazılımı birlikte yayınlanır.


Bu proje **ESP32-WROOM-32U**, **4.0" ST7796S dokunmatik SPI ekran** (480×320, XPT2046) ve **SHT30** sensörüyle çalışan perspektif (3D) uçuş radarıdır. Uçuş verisi [adsb.fi](https://opendata.adsb.fi/) üzerinden alınır.

Yazılım, yuvarlak ekranlı `plane-radar-clock` projesindeki servisleri kullanır: USB kurulum sayfası, adsb.fi istemcisi, saat, hava durumu ve harita katmanı. Değişen kısım ekran tarafıdır.

## İkinci kart: Waveshare ESP32-S3-Touch-LCD-5 (5", 800×480)

Aynı yazılımın 5" sürümü (`ws5` derlemesi). Ekran, dokunmatik (GT911) ve ESP32-S3 (16 MB flash, 8 MB PSRAM) tek kartta; ekran için kablo bağlantısı gerekmez. Sürüm numaraları `5in-` ile başlar.

### Kurulum ve güncelleme

- Kurulum sayfası ilk adımda modeli sorar (4" / 5"); sonra yalnızca o modele ait adımları gösterir.
- **İlk yükleme:** Kartı USB-C ile bağlayıp **Tam kurulum**. BOOT tuşu gerekmez.
- **Sonraki güncellemeler:** **Güncelle** (cihaz `hw: ws5` bildirir, sayfa 5" yazılımını gönderir).
- **Harita ve önemli yerler:** Sayfada **Cihaza kaydet** haritayı, arazi yüksekliklerini, önemli yerleri, yerleşim ve orman alanlarını indirip USB ile gönderir. Harita sunucuları yoğunsa sayfa önemli yerlerin alınamadığını söyler; birkaç dakika sonra tekrar kaydetmek yeter.
- Wi-Fi ağı değişmediyse ve şifre girilmediyse kayıt Wi-Fi bağlantısına dokunmaz.
- Kartın iki ekran sürümü var: **ESP32-S3-Touch-LCD-5** (800×480, `ws5`) ve **ESP32-S3-Touch-LCD-5B** (1024×600, `ws5b`). Yanlış sürüm yüklenirse görüntü karışık yatay çizgiler halinde gelir; kurulum sayfasının Gelişmiş bölümünde 5B için ayrı tam kurulum düğmesi var.

### SHT30 bağlantısı (Rev1.2 kart)

Sensör alttaki yeşil vidalı klemense bağlanır (soldan sayarak):

| Klemens (soldan) | Yazı | SHT30 |
|---|---|---|
| 1 | VIN | **Bağlamayın** (7–36 V güç girişi) |
| 2 | GND | — |
| 3 | VOUT (3,3 V) | VIN / VCC |
| 4 | GND | GND |
| 5 | SDA (GPIO8) | SDA |
| 6 | SCL (GPIO9) | SCL |

Hat dokunmatik ve CH422G ile ortak, SHT30 adresi 0x44. Sensörü karta yapıştırmayın; kartın ısısı sıcaklığı yüksek gösterir.

### Radar ekranı (5")

- **3B görünüm:** Uçaklar üç boyutlu model olarak çizilir. İrtifa ölçeği trafiğe göre otomatik ayarlanır. Uçakların arkasında düz noktalı iz, altlarında zemine inen çizgi var. Renkler: turuncu 10K ft altı, krem 10–25K ft, camgöbeği 25K ft üstü.
- **Harita:** 256 renkli zemin; tepeler kuzeybatıdan gelen ışığa göre gölgelenir, yükseldikçe yeşilden kuru ota ve kayaya döner. Deniz, göller, kıyı ve ana yollar (otoyollar kalın) çizilir. Yerleşim alanları gri, ormanlar koyu yeşil. Harita alanının dışı düz kara olarak görünür.
- **Havalimanları:** 60 km içindeki havalimanları kule/terminal simgesi ve üç harfli koduyla, pistleri yere yatık şerit olarak.
- **Önemli yerler:** İsim yerine simge: köprü, kule, stadyum, cami, tarihi yapı, müze, hayvanat bahçesi, park. En fazla 6 tane; önce her türden birer tane seçilir, sonra ikinciler (bu sırayla). Simgeye dokununca adı birkaç saniye görünür.
- **Kamera:** ‹ › ile 360° döner, ˄ ˅ ile eğim 14–26° arasında. Harita her açıda ekrana sığacak kadar büyük çizilir.
- **Uçak seçme:** Dokunulan uçak seçilir ve kamera onu küçük adımlarla yumuşakça takip eder.

### Ayarlar menüsü (5")

Ekrana 1,5 sn basılı tutunca açılır: Ekranı 180° çevir, Uçak etiketleri, İrtifa abartma, Menzil, Renkler (yüksek kontrast / normal), Uçak bilgisi (köşelerde / etikette), **Önemli yerler (göster / gizle)**. Kapasitif ekran olduğu için kalibrasyon, renk ters çevirme ve kırmızı/mavi seçenekleri yok; arka ışık yalnızca açık/kapalı.

### Teknik notlar (5")

- Arduino çekirdeği 3.x (pioarduino 55.03.30, ESP-IDF 5.5). Arduino kütüphaneleri RGB ekran için önerilen ayarlarla yeniden derlenir (`custom_sdkconfig`: octal PSRAM, 64 bayt önbellek satırı, kod PSRAM'den, Wi-Fi tamponları iç bellekte). Derleme: PlatformIO 6.1.18 + `click<8.2` + `uv`, her ortam (ws5 / ws5b) ayrı `PLATFORMIO_CORE_DIR` ile (bkz. main dalındaki iş akışı).
- Ekran sürücüsü esp_lcd RGB + "bounce buffer" (`include/hardware/bus_rgb_bounce.hpp`), **üç kare tamponu**: kareler ekranın göstermediği tampona çizilir, yırtılma olmaz ve çizim ekran tazelemesini beklemez.
- Kare 8 bitlik paletli bir tampona (PSRAM) çizilir, 256 girişli renk tablosuyla ekran tamponuna aktarılır.
- **Hızlandırmalar:**
  - Kamera dururken zemin, harita, halkalar ve simgeler ayrı bir tamponda saklanır; her karede yalnızca uçakların ve yazıların değiştirdiği bölgeler geri yüklenir.
  - Ekran 32×24 piksellik parçalara bölünür; yalnızca değişen parçalar ekran tamponuna yazılır.
  - Zemin 16 satırlık şeritler halinde hızlı iç bellekte çizilir.
  - Kamera dönerken arazi gölgelemesi yarım yatay ayrıntıyla hesaplanır; durunca tam ayrıntıyla yeniden çizilir.
- Seri logda 30 saniyede bir `radar3d:` satırı: kare hızı, sabit/dönen kare sayısı, aşama süreleri (zemin, çizgiler, uçaklar, arayüz, aktarım), panel tazeleme hızı.
- Panel zamanlaması: 800×480 için Waveshare'in kendi değerleri (16 MHz), 1024×600 için 14 MHz. Ayar: `include/hardware/lgfx_config.hpp`.
- Kartta BOOT tuşu kısayolu yok (GPIO0 ekran veri hattı).

## Malzeme

| Parça | Not |
|---|---|
| ESP32-WROOM-32U geliştirme kartı (DevKitC) | **Harici anten mutlaka takılı olmalı** (u.FL / IPEX1) |
| 4.0" SPI TFT ST7796S + XPT2046 dokunmatik | 480×320, 14 pinli |
| SHT30 modülü (I2C) | Adres 0x44 |
| Dişi-dişi jumper kablolar | |
| Veri destekli micro-USB veya USB-C kablo | Sadece şarj eden kablolar çalışmaz |

## Bağlantılar

### Ekran → ESP32

| Ekran pini | ESP32 pini | Açıklama |
|---|---|---|
| VCC | **5V** (VIN) | Modülün üzerinde regülatör var. Arka ışık için 5V daha iyi sonuç verir. |
| GND | GND | |
| CS | GPIO 5 | |
| RESET | GPIO 22 | |
| DC / RS | GPIO 21 | |
| SDI (MOSI) | GPIO 23 | |
| SCK | GPIO 18 | |
| LED | GPIO 4 | Arka ışık (PWM ile parlaklık) |
| SDO (MISO) | **bağlamayın** | Ekran yalnızca veri alır. Bu pini bağlamak dokunmatiği bozabilir. |
| T_CLK | GPIO 18 | SCK ile aynı pin |
| T_CS | GPIO 15 | |
| T_DIN | GPIO 23 | MOSI ile aynı pin |
| T_DO | GPIO 19 | MISO |
| T_IRQ | GPIO 34 | |

Ekranın arkasındaki SD kart pinleri (SD_CS vb.) kullanılmıyor.

### SHT30 → ESP32

| SHT30 | ESP32 |
|---|---|
| VIN / VCC | **3V3** |
| GND | GND |
| SDA | GPIO 25 |
| SCL | GPIO 26 |

Sensörü kutunun dışına ya da havalandırmalı ayrı bir bölmeye koyun. Ekran ve ESP32 ısı yayar. Sensör yine de yüksek okuyorsa `include/config.h` içindeki `kSht30TempOffsetC` değerini düzeltin (örneğin `-1.5f`).

**Kullanmayın:** GPIO 6–11 (flash belleğe bağlı), GPIO 12 (açılışı bozar), GPIO 0 ve 2.

## Kurulum ve güncelleme sayfası

**ckaraman.github.io/plane-radar-3d**: yazılımı yükleme, güncelleme, Wi-Fi ve konum ayarı ve Windows için USB köprüsü bu sayfada. Chrome veya Edge ile açın.

**Güncelleme:** Sayfada "Cihaza bağlan", sonra 3. adımdaki "Yazılımı güncelle". Yeni yazılım çalışan cihaza USB kablosundan gönderilir ve ikinci yazılım yuvasına yazılır; BOOT düğmesine gerek yoktur, ayarlar silinmez, sorun olursa eski yazılım çalışmaya devam eder (2–3 dakika). 3d-0.4.0'dan eski bir sürümden geçerken bir kereliğine "Gelişmiş → Tam kurulum" gerekir (bellek düzeni değişti).

Depoya her gönderimde (`main`) GitHub Actions yazılımı derler ve bu sayfayı yeni sürümle yayınlar. İlk seferde depoda **Settings → Pages → Source: GitHub Actions** seçilmeli. Her yeni sürümde `include/config.h` içindeki `kFirmwareVersion` değerini artırın.

## Yükleme (elle, sayfa yoksa)

1. Bilgisayarda Chrome veya Edge ile **https://espressif.github.io/esptool-js/** sayfasını açın.
2. Kartı USB ile takın. **Connect** düğmesine basın ve listeden kartı seçin. Kart genelde "CP2102" veya "USB Serial (CH340)" adıyla görünür.
   - Kart listede yoksa Windows'ta CP210x veya CH340 sürücüsünü kurun.
   - Bağlanmazsa kartın **BOOT** düğmesine basılı tutarak yeniden Connect'e basın.
3. **Flash Address: `0x0`** girin ve `firmware-merged.bin` dosyasını seçin. **Program** düğmesine basın.
4. Yükleme bitince kartın **EN / RST** düğmesine basın.

## Kurulum (Wi-Fi ve konum)

Cihaz ilk açılışta sarı kurulum ekranını gösterir.

1. Kurulum sayfasını açın: **ckaraman.github.io/plane-radar-3d**
2. **Cihaza bağlan**'a basın ve listeden kartı (CP2102 / CH340) seçin.
3. Wi-Fi, konum ve saat dilimini girip **Cihaza kaydet**'e basın.

Harita katmanı (sahil ve ana yollar) da gönderilebilir. Yalnız sayfa haritayı en fazla yaklaşık 19 km yarıçapında indirir, 3D görünümde bu alan merkezde kalır.

## Kullanım

| Hareket | Sonuç |
|---|---|
| Sol alttaki **‹ ›** düğmeleri | Görüşü 30° sola / sağa çevirir (yumuşak geçişle). Açı kaydedilir. |
| **˄ ˅** düğmeleri | Kamerayı eğer: yukarı daha tepeden, aşağı daha yatay bakış. Kaydedilir. |
| Bir uçağa dokunma | Seçer. Sağ alttaki kartta bilgileri görünür. |
| Boş bir yere dokunma | Seçimi kaldırır. Kart en yakın uçağı göstermeye döner. |
| Üst şeride dokunma | Menzili değiştirir: 15 → 25 → 35 → 50 km |
| **Ekrana 1,5 sn basılı tutma** | **Ayarlar menüsü** |
| Sağ alttaki **HAVA** düğmesi | 5 günlük hava tahmini ekranını açar; orada **RADAR** düğmesi geri döner |
| **Damla** düğmesi (radar, hava ve saat ekranında) | Su takibi ekranını açar |
| Saat ekranında dokunma | Radara döner (sağ üstteki damla ve HAVA düğmeleri hariç) |
| BOOT kısa basma | Menzili değiştirir |
| BOOT 3 sn basılı tutma | Wi-Fi ve ayarları siler, kurulum ekranına döner |
| Açılışta ekrana dokunuyor olmak | Dokunmatik kalibrasyonunu başlatır |

### Ayarlar menüsü

- **Dokunmatik kalibrasyonu:** Kalibrasyon kaydedilmemişse menü açılırken kendisi başlar. Köşelerde beliren işaretlere sırayla dokunun.
- **Renkleri ters çevir:** Siyah zemin beyaz görünüyorsa açın.
- **Kırmızı / Mavi:** Turuncu uçaklar mavi görünüyorsa değiştirin.
- **Ekranı 180° çevir**, **Uçak etiketleri**, **İrtifa abartma** (1–5×), **Menzil**, **Parlaklık**

Ekran modelleri arasında renk farkı olabildiği için bu ayarlar yazılımı yeniden derlemeden ekrandan değiştirilebiliyor.

### Ekranlar

- **Radar 3D:** Perspektif zemin ızgarası, menzil halkaları, harita (deniz, kıyı çizgisi, ana yollar) ve uçaklar. Uçakların altında zemine inen çizgi ve gölge, arkalarında son bir dakikalık iz var. Renkler: turuncu 10K ft altı, krem 10–25K ft, camgöbeği 25K ft üstü. İrtifa diğer radardaki gibi `1.2K ft` biçiminde.
- **Uçak kartı:** Seçilen (ya da en yakın) uçağın nereden nereye gittiği (adsbdb.com, çağrı koduna göre; bazı uçuşlarda bulunamayabilir), irtifa, hız, mesafe, rota ve tırmanma/alçalma.
- **3B arazi:** Haritayla birlikte arazi yükseklikleri indirilir. Kara düz bir renkle dolu çizilir; tepeler ızgara çizgisi olmadan kabarır, kuzeybatıdan gelen ışığa göre gölgede kalan yamaçlar koyu tonda görünür. Yollar, kıyılar ve göller araziye oturur.
- **Harita:** Deniz, göller ve baraj gölleri dolu, kıyı ve ana yollar çizgi olarak. Kurulum sayfasının 2. adımında harita boyutunu seçip "Cihaza kaydet"e basınca OpenStreetMap verisi indirilip USB ile gönderilir. Wi-Fi gerekmez.
- **Saat:** Seçili menzilde 15 sn boyunca uçak yoksa açılır (menzil dışındaki uçaklar engellemez). Altında İÇ ORTAM (SHT30) ve DIŞ ORTAM sıcaklık ve nemi gösterilir.
- **Hava durumu:** Radarın sağ altındaki HAVA düğmesiyle açılır. Dış ortamın şimdiki durumu (sıcaklık, hissedilen, nem, rüzgâr, gün doğumu/batımı), SHT30'dan ortam sıcaklığı ve nemi (rahatlık etiketiyle) ile 5 günlük tahmin (en yüksek/en düşük sıcaklık, yağış olasılığı). Veri Open-Meteo'dan 10 dakikada bir gelir; Wi-Fi yokken USB köprüsü indirip gönderir.

## Teknik notlar

- Kartta PSRAM yok. Ekran görüntüsü **4-bit (16 renk) tek parça tampon** (76,8 KB) içinde çizilip tek seferde gönderiliyor, bu yüzden titreme olmuyor. Tampon, Wi-Fi başlamadan önce ayrılıyor.
- adsb.fi ve hava durumu istekleri çekirdek 0'da, çizim çekirdek 1'de çalışıyor. Uçaklar iki veri güncellemesi arasında hız ve yönlerine göre ilerletiliyor (yaklaşık 8 kare/sn).
- Wi-Fi gücü tam seviyede (19,5 dBm). C3 sürümündeki 8,5 dBm kısıtlaması harici antenli bu kartta gerekmiyor.

## Derleme (geliştirici)

```
pio run -e wroom32u
pio run -e wroom32u -t merge    # firmware-merged.bin (0x0'a yazılır)
pio run -e ws5                  # Waveshare 5" (ESP32-S3)
pio run -e ws5 -t merge
pio run -e ws5b -t merge          # Waveshare 5B (1024×600)
```

Projeyi GitHub'a koyarsanız `.github/workflows/build.yml` her gönderimde derleme yapar ve `.bin` dosyalarını Actions sekmesinde hazır eder.

Pinler ve zamanlamalar `include/config.h` içinde. Renk paleti ve kamera ayarları `src/ui/radar3d.cpp` içinde.

## Türkçe karakterler

Ekran fontları GNU FreeFont'tan (FreeSans) üretilmiş bitmap fontlardır ve Türkçe harfleri (ÇĞİÖŞÜ çğıöşü) ile ° işaretini içerir. Yazılımdaki metinler UTF-8 yazılır. Fontları yeniden üretmek için:

```
pip install freetype-py
python3 tools/fonts/make_tr_fonts.py /usr/share/fonts/truetype/freefont
```

Başka bir harf gerekirse `tools/fonts/make_tr_fonts.py` içindeki `RANGES` listesine eklenir.

## Mac'te USB köprüsü

`tools/usb-kopru-mac/` (kurulum sayfasında “Mac köprüsünü indir”): `Kopru-Baslat.command` ya da `python3 radar_kopru.py [/dev/cu.usbserial-XXXX]`. Yalnızca macOS'teki python3 ve curl'ü kullanır; Windows köprüsüyle aynı veriyi (uçuş, rota, hava durumu) gönderir.

## Su takibi

- Damla düğmesiyle açılır. Günün toplamı, hedefe göre dolan damla avatar, saatlik çubuklar (önceki günle), son 7 gün; ‹ › ile geçmiş günler.
- 09:00–18:00 arası her saat başı ekran iki kez yanıp söner ve avatar "Su içtin mi?" diye sorar (o saatte zaten kayıt varsa sormaz, 10 dk sonra kapanır).
- Hedef cihazdan ayarlanır ve değiştirilene kadar kalır. Kayıtlar NVS'de: 21 gün saatlik, 366 gün günlük toplam.
- Yalnızca doğru saate ihtiyaç duyar: Wi-Fi'da NTP'den, USB modunda köprüden alır. Köprü olmadan Wi-Fi ile tek başına çalışır.
