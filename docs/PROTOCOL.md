# 2. sistem: PC ↔ kart USB protokolü (Aşama 5 tasarımı)

Kart Wi-Fi kullanmaz. Uçuş, rota, hava ve saat verisini PC programı (`helper/`) indirir,
ayıklar ve USB'den hazır gönderir; kart sadece çizer. Bağlantı her zaman USB klavye modu
(TinyUSB CDC + HID): deck tuşları da aynı kablodan gider.

Kararlar (kullanıcı onaylı):
- PC veriyi hazırlar (indir, ayıkla, menzil dışını at, sırala), **3B döndürme ve çizim kartta**.
  Dokunma, döndürme, yakınlaştırma PC'ye gidip gelmez.
- Kartın ekrandaki ayar menüsü kalır; kartta değişen ayar PC'ye bildirilir.
- PC kapalıyken kart saati gösterir (kart ayrıca besleniyorsa, son eşitlenen saatle).

## İlkeler

1. **Yeni biçim icat edilmez.** Her mesaj bir satır: `PR:<ETİKET> <veri>`; veri ya JSON ya da
   (sık gelen uçak satırında) virgüllü alanlar. Bugünkü kurulum sayfası, USB köprüsü ve deck
   programı zaten bu biçimi konuşuyor; kartın satır okuyucusu (`serial_setup`) aynen kalır.
2. **Akış kontrolü onayla.** TinyUSB CDC uzun, durmasız gönderimde veri kaybediyor. Bu yüzden:
   - küçük paketler (uçak listesi, hava) tek parti halinde gider, kart partiyi tek satırla onaylar;
     PC onayı almadan yeni parti göndermez;
   - büyük veriler (harita, arazi, kapak, deck resimleri) numaralı parçalar + CRC ile gider,
     her parça onaylanır (`ACK`), boşluk olursa kart `NAK {"need":n}` der (bugünkü `COVER*`
     ve `helper/deck/blob.py` ile aynı yöntem).
   - firmware güncellemesi (OTA) klavye modunda **yapılmaz**: PC önce `PR:USBSETUP` ile kartı
     kurulum moduna (USB-Serial/JTAG) alır, güncelleme orada yapılır (bugünkü gibi).
3. **Sürüm anahtarı.** Kart `HELLO` cevabında `"sys":2` ve `"feedv":1` bildirir. PC bunları
   görmezse veri göndermez, "kart yazılımını güncelleyin" der.

## Mesajlar

### Bağlantı ve kalp atışı

| Yön | Satır | Anlamı |
|---|---|---|
| PC → kart | `PR:HELLO` | Kart kim? (bugünkü gibi) |
| kart → PC | `PR:HELLO {"fw":…,"sys":2,"feedv":1,"prefs":{…},"map":…,"elev":…,"deckv":…,…}` | Sürüm, ekran ayarları, harita durumu |
| PC → kart | `PR:VOL {"v":42,"m":false}` | Ses + kalp atışı (~2 sn, bugünkü gibi) |

Kart 20 sn uçak partisi (`AEND`) almazsa "PC yok" sayar: son listeyi siler, radar boşalır,
15 sn sonra saat ekranına geçer (saat ayarı açıksa ve PC saati en az bir kez göndermişse; yoksa
radar "PC programı bekleniyor" der). Saat, son `AEND` ile eşitlenmiş haliyle çalışır. Yeni
parti gelince radar kendiliğinden döner. 1. sistem HELLO'da `"sys":1` bildirir.

### Uçaklar (her 2–5 sn bir parti)

```
PR:ABEGIN
PR:A lat,lon,yön,hız_kt,irtifa_ft,dikey_fpm,çağrı,tip[,nereden,nereden_şehir,nereye,nereye_şehir]
… (en fazla 64 satır, en yakından uzağa)
PR:AEND <unix_saat>
```
Kart cevabı: `PR:AOK {"n":<alınan>}`. Bu bugünkü `usb_feed` biçiminin aynısı (alan sırası
`src/core/services/usb_feed.cpp`). Rota alanı `-` ise "rota yok", hiç yoksa "henüz bakılmadı".

PC'nin işi: adsb.fi'den indirir, kartın menzilinin biraz dışına kadar olanları tutar, uzaklığa
göre sıralar, 64'te keser, rotayı (adsbdb) kendi önbelleğinden ekler, şehir adlarını ASCII'ye
çevirir. Kart yalnızca listeyi alır ve çizer.

### Hava (10 dakikada bir, ayrıca konum değişince)

`PR:W …`: bugünkü köprünün satırı (biçim `src/core/services/weather_parse.h`). PC Open-Meteo'dan
indirir, ayrıştırır, kart yalnızca okur. Cevap gerekmez; kart son satırı tutar.

### Ayarlar

| Yön | Satır | Anlamı |
|---|---|---|
| PC → kart | `PR:SET {"lat":…,"lon":…,"tz":…,"clock":…,"miles":…,"runways":…,"rings":…,"weather":…}` | Bugünkü `SET`, Wi-Fi alanları olmadan |
| kart → PC | `PR:PREFS {"range_km":15,"fetch_km":23,"miles":false}` | **Yeni.** Açılışta bir kez ve ekranda menzil ya da birim değişince. `range_km` = görünen menzil, `fetch_km` = PC'nin uçak indireceği yarıçap (menzilden biraz geniş) |

`PREFS` sadece kartta değişen ekran ayarlarını taşır. Konum ve saat dilimi gibi veri ayarları
PC'de tutulur (PC kaynaktır), karta `SET` ile gider.

### Harita ve arazi

Bugünkü `MAPBEGIN/MAP/MAPEND/MAPCLEAR` ve `ELEVBEGIN/ELEVROW/ELEVEND/ELEVCLEAR`, aynen.
Bugün bu verileri kurulum sayfası (tarayıcı) hazırlıyor; 2. sistemde PC programı hazırlar
(Aşama 8). Kart tarafı değişmez.

### Deck

`VOL`, `NOW`, `COVER*`, `Q*`, `DECK*`, `KEY`, `SETVOL`, `MUTE`: bugünkü gibi
(`src/deck/services/host_link.h`). Değişiklik yok.

## Modüller

**Kart tarafı.** Çizim kodu verinin nereden geldiğini bilmez: radar `adsb`, hava `weather`,
rota `route` deposundan okur. 2. sistemde bu depoları Wi-Fi indiricisi yerine USB satırları
doldurur (`usb_feed`, `weather::handleBridge`, `route::put`, bunlar zaten var). Aşama 6'daki iş,
indirme kodunu (`adsb_client`, `weather_service`, `route_service` içindeki HTTPS kısmı) depodan
ayırıp `src/net/`'e taşımak; 2. sistem `src/net/`'i hiç derlemez. Yeni tek parça `PR:PREFS`
gönderimi ve "PC yok → saat" kararı.

**PC tarafı.** Yeni bir veri motoru (`helper/deck/feed.py`, Aşama 7): bugünkü
`tools/usb-kopru*/radar_kopru.py`'nin indirme, süzme ve satır kurma kodu buraya taşınır.
- Dışa açık yüzü küçük: `Feed(card, fetch).tick(now)`: zamanı gelen işi yapar (uçak partisi,
  hava, rota sorgusu) ve döner.
- İki bağımlılığı dışarıdan alır: `card` (satır gönder / satır oku: gerçek seri port
  `device.py` ya da testlerdeki sahte kart) ve `fetch` (HTTP: gerçek ya da testte hazır cevap).
  İki taraf da iki adaptörlü, yani gerçek bir ek yeri (seam).
- Testler bu yüzden internet ve kart olmadan çalışır (`helper/tests/test_core.py`'deki sahte
  kart genişletilir): sahte adsb.fi cevabı → karta giden `PR:A` satırları, `AOK` gelmezse
  bekleme, menzil değişince süzme.

## Hata durumları

| Durum | Ne olur |
|---|---|
| `AOK` 3 sn içinde gelmez | PC partiyi atlar, sonrakini gönderir (eski veri birikmez) |
| İnternet yok / adsb.fi cevap vermez | PC son listeyi göndermez; kart 20 sn sonra boş radar, sonra saat |
| Kart eski yazılımlı (`feedv` yok) | PC veri göndermez, pencerede güncelleme önerir |
| USB bağlantısı takılır | Kartın bugünkü kurtarması: 20 sn sessizlikte yazılımla tak-çıkar (`console::tick`) |
| Kart açılmıyor | Kurtarma: BOOT + RESET, PC programından "Tam kurulum" (Aşama 8) |
