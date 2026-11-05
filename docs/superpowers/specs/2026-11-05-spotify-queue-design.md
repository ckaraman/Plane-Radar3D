# Spotify sırası kartta (tarayıcıdaki open.spotify.com)

## Amaç ve neden

Bugün kartın Müzik ekranında "Sıradaki" listesi yalnızca YouTube Music'te (tarayıcı eklentisiyle)
görünüyor. Kullanıcı Spotify'ı da kullanıyor ve aynı şeyi Spotify'da istiyor: tarayıcıda
open.spotify.com'da çalarken sıradaki şarkılar küçük kapaklarıyla kartta görünsün, dokunulan
şarkı çalsın.

- **Hedef:** open.spotify.com'da sağdaki sıra paneli açıkken kart sırayı gösterir (en çok 30 satır,
  çalan şarkıdan başlayarak) ve satıra dokununca o şarkıya geçer. YouTube Music aynen çalışmaya
  devam eder; iki site aynı anda açıksa kart o an çalanın sırasını gösterir.
- **Hedef değil:** Spotify Web API (5 kişi sınırı, Premium şartı, giriş akışı — ileride ayrı bir
  seçenek olabilir); sıra panelini eklentinin kendiliğinden açması; kart yazılımında (firmware)
  değişiklik; Mac'e özel bir iş (eklenti tarayıcıda çalışır, yardımcı program iki sistemde aynı).
- **Olmaması gereken:** YouTube Music sırasının bozulması; iki sekme açıkken listenin iki site
  arasında gidip gelmesi; başka bir şey çalarken yanlış sıranın gösterilmesi; yanlış satıra
  dokunulunca başka şarkının çalması.

## Bugün nasıl çalışıyor (koddan)

- `helper/browser-extension/ytmusic.js` music.youtube.com'da sıra panelini okur ve
  `{t:'queue', items:[{t,a,d,img}], cur, now, art}` gönderir. `background.js` **tek** bir sıra
  sekmesi tutar (`queueTab`, `lastQueue`) ve mesajı WebSocket'le programa iletir. Kart `QPLAY`
  gönderince `play` mesajı o sekmeye gider.
- `helper/deck/browser_link.py` gelen sırayı `self.queue` / `self.art` olarak tutar (bağlantı
  başına tek sıra). `queue_card.card_rows` sırayı yalnızca `items[cur]` başlığı çalan şarkıyla
  eşleşirse gösterir. `core._check_hq_cover` `link.art` ile büyük kapağı getirir.
- Kapaklar yalnızca YouTube'un resim sunucularından indirilir (`queue_card.PIC_HOSTS`).

## Spotify sayfasında görülenler (05.11, Chrome'da canlı)

- Sıra paneli: `aside[aria-label="Sıraya Ekle"]` (dil Türkçe; başka dilde etiket değişir).
  İçinde sırayla `ul[role=treegrid]` bölümleri: "Şu anda çalan" (1 satır), "Sonraki" (80 satır).
  Kullanıcının elle eklediği şarkılar için ayrı bir bölüm olması bekleniyor, görülmedi.
- Satır: `li[role=row]`; ilk satır metni başlık, ikincisi sanatçı / program; `img` 64×64
  `i.scdn.co`; `[data-testid=play-button]` çal düğmesi. **Süre yok.**
- Çalan şarkı: `navigator.mediaSession.metadata` (başlık, sanatçı, 64/300/640 kapak).
- Sıradaki 3. satırın çal düğmesine `click()` → 3 sn içinde o şarkıya geçti; atlanan satırlar
  sıradan düştü (YouTube Music'te liste yerinde kalıyordu).
- Spotify sekmesi başka cihazda çalanın sırasını da gösteriyor.

## Yollar

- **A — Spotify için ayrı betik (seçilen).** `spotify.js` aynı mesaj biçimini üretir; YouTube Music
  kodu değişmez. Kullanıcı bunu seçti.
- B — İki siteyi tek ortak betikte toplamak. Daha düzenli ama çalışan YouTube Music kodunu yeniden
  yazmayı gerektirir; şimdilik gereksiz risk.
- C — Spotify'ın sayfa içi verisini / Web API'yi kullanmak. Süreler gelirdi; kırılgan, kurallara
  aykırı olabilir (sayfa içi) ya da 5 kişi + Premium sınırına takılır (API). Elendi.

## Ne yapılacak

1. **Eklenti** (`helper/browser-extension/`)
   - `spotify.js`: sıra panelindeki tüm `ul[role=treegrid]` bölümlerinin satırlarını sırayla
     okur; `cur = 0` (ilk bölüm çalan şarkı), `d = ''`, `img` = satır resmi, `now` / `art`
     mediaSession'dan (en büyük kapak). Panel yoksa boş sıra gönderir. `play(i, title)`: i'inci
     satırı bulur, başlık tutmazsa başlıkla arar (ytmusic.js ile aynı), çal düğmesine basar.
     Değişiklik izleme ve gönderme sıklığı ytmusic.js gibi. Seçiciler `aria-label` yerine
     `role` / `data-testid` üstüne kurulur ki dil değişince de çalışsın (panelin kendisi:
     `control-button-queue` ile ilişkili `aside`; kesin seçici uygulamada bulunur).
   - `manifest.json`: `open.spotify.com` izin ve içerik betiği; açıklama metni; sürüm 1.2.0.
   - `background.js`: tek `queueTab` yerine **sekme başına** son sıra; her mesaja `tab` kimliği
     eklenir; sekme kapanınca o sekmenin boş sırası gider; `play` / `refresh` mesajı programın
     söylediği sekmeye gider. Yeniden bağlanınca bütün sekmelerin son sırası yeniden gönderilir.
2. **Yardımcı program** (`helper/deck/`)
   - `browser_link.py`: sıraları `(bağlantı, sekme)` başına tutar. Dışarıya tek bir "geçerli
     sıra" verir: `items[cur]` başlığı çalan şarkıyla eşleşen; eşleşen yoksa en son güncellenen
     (bugünkü davranış). `art` da aynı kaynaktan. `play` seçili kaynağın sekmesine gider.
     Eski eklenti (`tab` alanı yok) tek kaynak gibi çalışmaya devam eder.
   - `queue_card.py`: `PIC_HOSTS` içine `scdn.co`. Boş süre kartta sorun çıkarmamalı (kontrol).
   - `web/index.html`: "(YouTube Music)" geçen metinler Spotify'ı da anar; Spotify için sıra
     panelinin açık olması gerektiği bir cümleyle söylenir.
3. **Testler** (`helper/tests/`): kaynak seçimi (iki kaynak, biri eşleşiyor; hiçbiri eşleşmiyor;
   eski biçim), sekme kapanınca kaynağın düşmesi, `scdn.co` kapağının kabulü.
4. **Deneme:** Chrome'da Spotify + YouTube Music sekmeleri birlikte; kartta liste, dokununca
   geçiş; panel kapatılınca listenin kalkması. Masaüstü Spotify + açık Spotify sekmesi ayrıca
   denenir (çalışırsa artı, çalışmazsa not düşülür).

## Riskler

- Spotify sayfa yapısını değiştirirse `spotify.js` düzeltilmeli; seçiciler tek yerde toplanır.
- Kullanıcı tarafında eklentiyi yenileyip tarayıcıyı yeniden açmak gerekir (yeni site izni).
- Kartta süre sütunu boş kalır.

## Uygulayıcıya kalanlar

Kesin seçiciler, gönderme sıklığı, sürüm numaraları (program sürümü), test dosya düzeni.
