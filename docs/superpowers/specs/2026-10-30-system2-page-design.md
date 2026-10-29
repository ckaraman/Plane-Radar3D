# 9a: 2. sistemin kurulum sayfası

## Amaç

Ana sayfa (2. sistem) artık programın yerini tutmasın: kartı hazırlatsın, PC programını
indirtsin, kurtarmayı anlatsın. Ayarlar, harita, tuşlar ve güncelleme programda (8. aşama).
İleride 2. sistemden radar tamamen kalkabilir; sayfa o gün de değişmeden kalabilmeli.

- **Hedef:** sade, karta bağlanmayan bir sayfa; yeni kullanıcı iki adımda hazır.
- **Hedef değil:** 1. sistemin `/radar` sayfasını değiştirmek (yalnızca bir yönlendirme metni
  düzelir); `web/index.html` içindeki artık ölü 2. sistem parçalarının temizliği (10. aşama).
- **Olmaması gereken:** `/radar` sayfasının bozulması; bir kullanıcının "2. sisteme nasıl
  geçerim" sorusunun cevapsız kalması.

## Yaklaşımlar

1. **Ayrı, küçük sayfa dosyası (seçilen).** `web/deck.html` sitenin kökü olur, bugünkü
   `web/index.html` yalnızca `/radar` olarak yayınlanır. 2. sistem sayfası Web Serial kodu
   taşımaz; 1. sistemden bağımsızdır, radar 2. sistemden kalkınca da dokunulmaz.
2. Aynı dosyada `.or` / `.nr` ile gizleme: değişiklik küçük ama iki ürün tek dosyada karışık
   kalır, radar kalkınca temizlik işi olarak geri gelir. Elendi.

## Sayfa (`web/deck.html`)

Başlık: "Plane Radar Deck kurulum" ve bir cümle: kart USB ile PC'ye bağlı çalışır, uçuş/hava
verisini ve tuşları program yönetir.

1. **Kartı hazırla** — "Kartta 2. sistem yoksa (yeni kart ya da 1. sistemden geçiş)":
   esp-web-tools `manifest-full-ws5.json` düğmesi "Kart yazılımını yükle". Not: yeni kart ve
   1. sistem kartı zaten kurulum modunda, BOOT gerekmez; ayarlar silinir, konum ve harita
   programdan girilir. Program açıksa önce menüsünden "Bağlantıyı bırak".
2. **Programı indir** — bugünkü üç düğme (Windows, Mac M, Mac Intel) ve kısa notlar
   ("Ek bilgi → Yine de çalıştır", Mac'te Uygulamalar'a sürükle / sağ tık → Aç, Mac türü nasıl
   anlaşılır). "Konum, harita, tuşlar ve kart yazılımı güncellemeleri programdan yapılır."
3. **Kurtarma** — bugünkü metin, BOOT + RESET ile; "1. adımda bağlıysanız" ve "2. adımdan
   ayarları girin" cümleleri programa göre yeniden yazılır. Aynı `manifest-full-ws5.json`
   düğmesi.

Altta: "Yalnız Wi-Fi radar (1. sistem) için: `radar/` sayfası". Footer: yalnızca bu sayfada
kullanılanlar (esp-web-tools; veri kaynakları programın işi olduğu için kısa bir satır).

Tarayıcı Web Serial desteklemiyorsa (Safari, Firefox, telefon) esp-web-tools'un kendi
"desteklenmiyor" metni görünür; indirme düğmeleri her tarayıcıda çalışır.

Görünüş bugünkü sayfayla aynı (renkler, kartlar, düğmeler): gereken CSS `web/index.html`'den
kopyalanır, yalnızca kullanılan sınıflar.

## Yayın (CI, `.github/workflows/build.yml`)

- `site/index.html` ← `web/deck.html`.
- `site/radar/index.html` ← `web/index.html` (bugünkü `sed` ile `<base href="../">` ve
  `data-sys="radar"`); `mapdata.js`, `tz.json`, manifest ve `.bin` dosyaları kökte kalır.
- Kontrol: `site/index.html` içinde `manifest-full-ws5.json`, `site/radar/index.html` içinde
  `data-sys="radar"` geçmeli (grep).

## `/radar` sayfası

"Deck'li sisteme dönmek için: ana kurulum sayfasını açıp 3. adımdan Yazılımı güncelle" →
"Deck'li sisteme (2. sistem) geçmek için ana kurulum sayfasındaki 'Kartı hazırla' adımını
kullanın. Kartın ayarları silinir; konum ve harita programdan girilir."

## Kısıtlar ve riskler

- Kartın ekranı 2. sistemde kök adresi gösterir (`include/config.h`); adres değişmez.
- 2. sistem kartını yeniden kurmak artık BOOT + RESET ister (kullanıcı kararı).
- Bugünkü kök adresi yer imine eklemiş 1. sistem kullanıcısı sade sayfayı görür; altta
  `radar/` bağlantısı var.

## Yapıcıya kalanlar

- Metinlerin son hâli (yukarıdaki anlam korunarak), CSS'in hangi parçalarının kopyalanacağı.
