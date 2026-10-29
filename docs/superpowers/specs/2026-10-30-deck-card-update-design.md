# 8c: Kart yazılımını PC programından güncelleme

## Amaç

2. sistemde (ws5) kartın yazılımı, kurulum sayfasını açmadan PlaneRadarDeck'ten güncellensin.
Program yeni sürümü kendisi fark edip haber versin; güncelleme kullanıcı onaylayınca başlasın.

- **Hedef:** yeni sürümü bildirmek, tek tıkla güvenli güncelleme, ilerlemeyi göstermek.
- **Hedef değil:** tam kurulum ve kurtarma (sayfada kalır), PC programının kendini güncellemesi
  (9. aşama, imza işiyle birlikte), 1. sistem (`radar`).
- **Olmaması gereken:** sormadan güncellemek; kartı kurulum modunda ya da yarım yazılımla
  bırakmak; yanlış sistemin dosyasını yüklemek.

## Yaklaşımlar

1. **Kartın kendi USB güncellemesi, Python'dan (seçilen).** Kurulum sayfasındaki akışın
   (`web/index.html` → `usbUpdate`) Python karşılığı. Yeni yazılım ikinci bölüme yazılır;
   kablo çıkarsa kart eski yazılımla açılır. Karta değişiklik gerekmez.
2. esptool ile indirme modunda yazma: kart yazılımı bozukken de çalışır ama çalışan bölümün
   üstüne yazar, exe büyür, Defender riski artar. Kurtarma sayfada kaldığı için elendi.
3. Pencerede sayfanın JS'ini (Web Serial) çalıştırmak: WebView2'de güvenilir değil, port
   Python'da açık. Elendi.

## Nasıl çalışır

**Sürüm denetimi.** Program açılışta ve 6 saatte bir
`https://ckaraman.github.io/Plane-Radar3D/version.json` dosyasını okur, `version_ws5` ile
kartın `HELLO`'daki `fw` değerini karşılaştırır. Kart bağlı değilse ya da `sys` 2 değilse
denetim yapılmaz. Ağ hatası sessizce geçilir (bir sonraki denemeye kalır).

**Haber verme.** Sürümler farklıysa:
- tepside bir kez bildirim ("Kart için yeni yazılım var: X"),
- pencerede bir şerit: **Güncelle** / **Şimdi değil**. "Şimdi değil" o sürümü hatırlar; o sürüm
  için bir daha bildirim çıkmaz (şerit pencerede sönük durur, istenirse yine basılabilir).
- tepsi menüsüne "Kart yazılımını güncelle…" satırı (yeni sürüm varken).

**Güncelleme.** Onaydan sonra:
1. `firmware-ws5.bin` indirilir (`?v=<sürüm>` ile, önbelleksiz). Boyut ve CRC32 hesaplanır.
2. Kart `HELLO`'da `hw` / `sys` ile denetlenir; uymazsa güncelleme başlamaz.
3. Mevcut `_in_setup_mode` yolu (`helper/deck/device.py`) kartı kurulum moduna alır; bu sırada
   uçak/hava akışı ve kapak gönderimleri durur (`busy`).
4. `PR:OTABEGIN {"len","crc"}`, sonra `PR:OTA <seq> <len> <base64>` parçaları (v2, `otav>=2`),
   her biri `OTAACK` beklenir; `OTANAK` gelirse istenen parçadan devam edilir; en sonda
   `PR:OTAEND` → `OTADONE`. Herhangi bir hatada `PR:OTAABORT`.
5. Kart yeniden başlar ve kurulum modunda açılır (bayrak NVS'te). Programın izleyicisi kurulum
   portunu görünce `PR:USBNORMAL` ile klavye moduna döndürür (bugünkü davranış). Yeni `HELLO`
   ile sürüm doğrulanır, şerit "Güncellendi (sürüm X)" olur.

İlerleme pencerede yüzde çubuğuyla görünür; kart kendi ekranında da ilerleme çizer.

**Hata durumları.** İndirme hatası, kart uyuşmazlığı, `ota_no_space`, zaman aşımı: kullanıcıya
Türkçe kısa mesaj, "Kart eski yazılımla çalışmaya devam eder, tekrar deneyin". Kart kurulum
modunda kalmışsa `USBNORMAL` ile geri alınır.

## Kısıtlar

- Kart tarafında değişiklik yok; protokol `src/core/services/serial_ota.cpp` ve
  `docs/PROTOCOL.md` neyse o.
- Windows ve Mac sürümü aynı kodu kullanır (`device.py` ortak).
- Kalan risk: yeni yazılım açılışta çökerse kendiliğinden eski sürüme dönülmez (bugün sayfada
  da böyle); çözüm sayfadaki kurtarma.

## Yapıcıya kalanlar

- Parça boyutu ve zaman aşımları (sayfadaki değerler başlangıç noktası).
- Şeridin ve ilerleme çubuğunun pencerede tam yeri ve görünüşü (mevcut stile uyacak).
- "Şimdi değil" ile atlanan sürümün nerede saklanacağı (programın mevcut ayar dosyası).
