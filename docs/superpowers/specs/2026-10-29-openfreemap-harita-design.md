# Harita verisi: OpenFreeMap (Overpass yedek)

## Neden
Harita bilgisi bugün yalnızca Overpass sunucularından geliyor. 29 Ekim'de dört sunucunun dördü de
hata verdi (406 / 502 / 504 / zaman aşımı). OpenFreeMap aynı OpenStreetMap verisini hazır
karolar (MVT, `tiles.openfreemap.org/planet`) olarak sunuyor: anahtar yok, sınır yok, sorgu
çalıştırmadığı için yoğunlukta çökmüyor.

## Hedefler
- `web/mapdata.js` haritayı önce OpenFreeMap'ten alır, olmazsa Overpass'a döner (bugünkü kod).
- Hem kurulum sayfası (1. sistem, Wi-Fi) hem Deck programı (2. sistem) bunu kendiliğinden kullanır,
  çünkü ikisi de aynı dosyayı yüklüyor.
- Karta giden harita dosyası (biçim, 31 KB / 3000 nokta sınırları) aynı kalır.

## Hedef dışı
- Kart yazılımı değişmez. Kart haritayı hiçbir zaman kendisi indirmez; hazırlık her iki sistemde
  de bilgisayarda (tarayıcı / program penceresi) yapılır. Bu yüzden kartta hız kaybı yoktur.
- Arazi yükseklikleri (AWS / Open-Meteo) değişmez.
- Harita seçici (konumu haritadan seçme) bu işte yok.

## Yöntem (kullanıcı seçti: A)
- **A, seçildi:** Karoları okuyan küçük bir çözücü + Overpass biçimine çeviren katman. Çizim kodu
  (`buildMapBlob`, göl, orman, yerleşim, önemli yerler) olduğu gibi kalır.
- B, seçilmedi: çizim kodunu karolara göre yeniden yazmak. Daha büyük değişiklik, harita
  görünümü değişebilir, Overpass yedeği için iki çizim yolu gerekir.
- Yalnızca Deck programında indirmek elendi: kurulum sayfası Overpass'a bağlı kalırdı.

## Kararlaşanlar
- Önemli yerler OpenFreeMap'in önem sırasıyla (poi `rank`) seçilir; Vikipedi şartı kalkar
  (kullanıcı: "Olur"). Havalimanları `aerodrome_label.iata`, pistler `aeroway` katmanından.
- Overpass yedek olarak kalır.
- Sayfa ve programdaki kaynak notu OpenFreeMap / OpenMapTiles / OpenStreetMap'i anar.

## Uygulayıcıya kalan (benim kararlarım)
- MVT çözücüyü kendim yazarım (~100 satır, kütüphane yok), Node ile küçük bir testle denenir.
- Kıyı: `water` katmanındaki `class=ocean` alanlarının kenarlarından, karo sınırı üzerindeki
  kenarlar atılarak kıyı çizgisi çıkarılır, yönü OSM kıyı kuralına (kara solda) göre ayarlanır.
- Yakınlaştırma seviyesi ölçülerek seçilir (z12 civarı: en büyük haritada ~50 karo).
- Eşlemeler: `transportation class=motorway/trunk` → otoyol; `water` (ocean dışı) → göl;
  `landcover class=wood` → orman; `landuse residential/commercial/industrial/retail` → yerleşim.
- Deneme: İstanbul (kıyı + göl), iç bölge bir şehir (Ankara) ve kıyısız/küçük bir yer; eski ve
  yeni haritalar tarayıcıda yan yana karşılaştırılır.
