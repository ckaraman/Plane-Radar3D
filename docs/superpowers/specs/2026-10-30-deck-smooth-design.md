# 10: Akıcı deck ve radar (2. sistem) — ölç, gereksiz yükü kaldır

## Amaç ve neden

2. sistemde (kart USB'den PC programına bağlı, Wi-Fi yok) YouTube Music listesi tarayıcı
eklentisinden karta geldiğinde kart yavaşlıyor: işlemler 4-5 sn sürüyor, radar ekranı neredeyse
duruyor. Kullanıcı bunu bugünkü sürümlerde (kart 5in-0.41.1, program 1.9.2) de görüyor.

Kullanıcının istediği: Chrome/Edge'de çalan YouTube Music listesi kartta görünsün ve kart deck
olarak akıcı kullanılsın; radar ekranına geçince uçaklar akıcı hareket etsin. Bu, PLAN.md
10. aşamasının "performans karşılaştırması" kısmıdır: değişiklikten önce ve sonra kartta ölçülür.

- **Hedef:** şarkı geçişinde ve liste gelirken radar takılmadan çizer; deck dokunuşlara hemen
  cevap verir; gereksiz tekrar gönderim ve tekrar JPEG açma kalkar. Önce/sonra sayılarla gösterilir.
- **Hedef değil:** 1. sistem (Wi-Fi'li) — `radar` derlemesi davranış olarak değişmez; USB okumayı
  kartın öbür çekirdeğine taşımak (yol 2, ölçüm yetmediğini gösterirse ayrı iş); PLAN'daki temizlik
  (eski kart blokları, `.nr` parçaları) — ayrı küçük iş olarak sona.
- **Olmaması gereken:** kapaklar/liste hiç gelmez ya da yanlış satıra yanlış kapak oturur; eski
  program + yeni kart ya da yeni program + eski kart bozulur; kart yazılım güncellemesi (8c, OTA)
  ya da harita/tuş gönderimi etkilenir.

## Bugün nasıl çalışıyor (koddan)

- Eklenti listeyi programa verir (`helper/deck/browser_link.py`). `queue_card.QueueSender` en çok
  3 sn'de bir çalan şarkıdan başlayan 30 satırı (`QBEGIN/QITEM/QEND`, `gen` ile) gönderir, sonra
  ilk 10 satırın küçük kapağını (`qthumb` px, 40) teker teker `QART` ile yollar: 180 baytlık
  parçalar, her parça kartın cevabını bekler (`device._transfer`, pencere 1).
- Şarkı değişince satırlar bir kayar; başlıklar değiştiği için `gen` artar, `sent_pics` sıfırlanır,
  10 kapağın hepsi yeniden gider (programın önbelleğinden, internetten değil). Kartta `QEND` eski
  satırların kapaklarını siler (`freeArts`), `refreshThumbs` (`deck_screen.cpp:730`) her kapağı
  yeniden JPEG'den açar.
- Büyük kapak (`COVER*`, 300×300'e kadar) da parça parça gelir. Gelirken (`coverReceiving()`)
  `main.cpp` hangi ekran açık olursa olsun çizimi en çok 4 kare/sn'ye düşürür (radar dahil):
  kart döngünün her turunda kablodan bir parça okuduğu için çizim azalınca kapak çabuk gelir.
- YouTube Music çalarken büyük kapak iki kez gidebilir: önce Windows medya oturumunun kapağı, sonra
  eklentiden gelen büyük kapak (`core._check_hq_cover`).
- Kart hangi ekranın açık olduğunu programa bildirmiyor.
- Ölçüm: radar birkaç sn'de bir `radar3d: … fps …` satırını konsola yazar; program bu satırları
  göstermez/saklamaz; programın günlük dosyası yok.

## Yaklaşımlar

1. **Önce ölç, sonra gereksiz yükü kaldır (seçilen).** Ölçüm satırları; kapaklar şarkıya bağlı
   tutulur ve elde olan tekrar gönderilmez; radar ekranındayken kapak trafiği bekletilir ve
   4 kare/sn sınırı radarda kalkar. Kart yazılımı + program birlikte değişir.
2. USB okuma ve JPEG açmayı kartın öbür çekirdeğine taşımak: kökten ama iki çekirdeğin ortak veriye
   dokunması kilitlenme/bozuk görüntü riski getirir. Yol 1'in ölçümü yetmezse sıradaki.
3. JPEG yerine ham piksel göndermek: kablodan giden veri çok büyür; elendi.

## Ne yapılacak

Üç küçük adım, her biri ayrı yayın ve kart denemesi (kullanıcının "küçük aşamalar" tercihi):

### 10a — Ölçüm (davranış aynı)

- Kart, deck yazılımında (`WITH_DECK`) birkaç saniyede bir tek satırlık ölçüm raporu gönderir
  (örn. `PR:PERF {...}`): açık ekran, kare/sn, en uzun döngü turu (ms), bu aralıkta okunan kablo
  satırı sayısı, JPEG açma sayısı ve toplam süresi (büyük kapak ve küçük kapaklar ayrı), kapak
  gelirken geçen süre.
- Program bu raporları ve kendi gönderim sürelerini (büyük kapak, küçük kapak, liste: kaç bayt,
  kaç sn — kapak için bugün zaten yazılıyor) pencerenin günlüğünde gösterir ve bir dosyaya da yazar
  (`%APPDATA%/PlaneRadarDeck/perf.log`, boyutu sınırlı), böylece kullanıcı karttaki denemeden sonra
  dosyayı verebilir.
- Kullanıcı denemesi (önce): radar ekranı açıkken ve deck ekranı açıkken şarkı geçişi; birkaç
  geçişin sayıları kaydedilir.

### 10a ölçümünün sonucu (2026-10-30, kart 5in-0.42.0, Deck 1.10.0)

- Kablo çok yavaş: büyük kapak 25 KB → 29 sn, 72 KB → 82 sn (~0,9 KB/sn); 30 satırlık liste 6,5 sn;
  küçük kapak ~2 sn. Kart bu sırada boşta (5 sn'de ~900 tur, ~28 satır).
- Sebep programda: `device._read_loop` `ser.read(512)` ile 0,2 sn zaman aşımıyla okuyor; 25 baytlık
  her cevap 0,2 sn bekliyor, parça parça aktarımda her parça bir cevap bekliyor.
- Kapak gelirken radar 4 kare/sn (normalde 29-37); bu kapak boyunca, yani 30-80 sn sürüyor.
- Aktarım `stream_lock`'u tutarken uçak partileri 5 sn bekleyip atlanıyor; 20 sn parti gelmeyince kart
  PC'yi yok sayıyor (`usb_feed` `kActiveMs`): kullanıcı radarda bağlantının kesildiğini görüyor.
- Müzik sayfasının kapakla kurulması ~1,9 sn (bazen arka arkaya iki kez): dokunuşlar bu sürede bekliyor.
- Küçük kapak açma ~1 ms: önemsiz.

Kullanıcı kararı: 10b üç küçük adıma bölünür, her biri ölçülür.

### 10b-1 — Programın okuması (yalnız program)

- `device._read_loop` kablodaki baytları beklemeden okur (`ser.read(ser.in_waiting or 1)`); bir cevap
  geldiği anda işlenir.
- `perf.log` uçak partilerini de yazar (`aircraft <n> ok|skipped|noanswer <sn> s`), böylece bağlantı
  kopması ölçümde görünür.
- Kullanıcı denemesi: 10a ile aynı; büyük kapak, liste ve küçük kapak süreleri, radar kare/sn'si ve
  atlanan uçak partileri karşılaştırılır.

### 10b-2 — Kart (10b-1 ölçümünden sonra planlanır)

- Kapak gelirken 4 kare/sn sınırı radarda kalkar (yalnız deck müzik sayfasında).
- Müzik/sıra sayfasının kapakla kurulması hızlanır ve gereksiz ikinci kurulum olmaz.
- 10b-1'den sonra aktarımlar hâlâ uçak partilerini atlatıyorsa: partiler aktarım parçalarının arasına girer.

### 10b-3 — Gerekirse (10b-1/10b-2 ölçümüne göre)

Aşağıdaki maddeler (ilk tasarım) ölçüm hâlâ başarı ölçütünün altında kalırsa yapılır.

### 10b — Gereksiz yükü kaldır (ilk tasarım, artık 10b-3)

- **Küçük kapaklar şarkıya bağlı:** kart `QEND`'de eski listeden yeni listeye, aynı şarkı (satırın
  `x` sırası ve başlığı aynı) olan satırların kapağını taşır, siler değil. Program da aynı kuralla
  kartta hangi kapakların olduğunu bilir; yalnızca kartta olmayanları gönderir. Şarkı geçişinde
  10 yerine normalde 1 kapak gider. Kart bunu `HELLO`'da yeni bir alanla bildirir (örn.
  `"qkeep":1`); bu alanı görmeyen program eskisi gibi hepsini yollar, alanı olmayan karta yeni
  program da eskisi gibi hepsini yollar.
- **Kart tekrar açmaz:** ekranda gösterilmek için açılmış küçük kapaklar (`s_thumb`) satır kayınca
  yeniden açılmaz, satırla birlikte taşınır.
- **Radar önce:** kart açık ekranı programa bildirir (ekran değişince bir satır + `HELLO`'da).
  Radar (ya da deck dışı bir ekran) açıkken program büyük/küçük kapak göndermeyi bekletir; liste
  satırları (küçük metin) gitmeye devam eder. Deck açılınca bekleyen kapaklar gider. Kartın
  "kapak gelirken 4 kare/sn" sınırı yalnızca deck'in müzik sayfasında uygulanır; tuş sayfasında
  (`Page::Keys`) kapaklar gelmeye devam eder ama çizim yavaşlatılmaz, dokunuşlar hemen cevaplanır.
  Bunun için kartın bildirimi deck sayfasını da içerir (yapımcı kararı: sonradan ikinci bir
  protokol değişikliği gerekmesin).
- **Büyük kapak iki kez gitmez:** eklentiden büyük kapak gelecekse programın medya oturumu kapağını
  göndermesi kaldırılır — yalnızca 10a ölçümü başarı ölçütündeki eşiği aşarsa.
- Kullanıcı denemesi (sonra): 10a ile aynı deneme, sayılar yan yana.

### 10c — Temizlik (PLAN'daki)

- `BOARD_WS5B` / `!BOARD_WS5` derlenmeyen blokları (`main.cpp`, `serial_setup.cpp`, `display.cpp`,
  `lgfx_config.hpp`, `sht30.cpp`, `ui_scale.h`, iki font dosyası, `config.h`).
- `web/index.html` `.nr` parçaları; README metni (9a'dan ertelenen).
- Davranış değişmez; iki derleme de (`ws5`, `radar`) temiz derlenir.

## Başarı ölçütü (kullanıcı onayladı)

- **Radar:** şarkı geçişi sırasında, aynı oturumda şarkı geçişi yokken çizdiği kare/sn'nin en az
  %80'i (normal hız 10a ölçümünden).
- **Deck:** dokunuşa en geç 0,2 sn'de cevap; kartın tek bir döngü turu 200 ms'yi geçmez.
- **Büyük kapağın iki kez gitmesi:** 10a ölçümü şarkı başına 1 sn'den fazla kablo trafiği gösterirse
  10b'de kaldırılır, göstermezse kalır.
- 10b sonrası ölçüm bu eşiklere ulaşmazsa yol 2 (öbür çekirdek) ayrı iş olarak konuşulur.

## Kısıtlar

- Kart yazılımı ve program birlikte yayınlanır; kart programın kendi güncelleme özelliğiyle (8c)
  güncellenir. Eski/yeni karışımı bozulmaz (HELLO alanlarıyla).
- Protokol değişiklikleri `docs/PROTOCOL.md`'ye yazılır.
- 1. sistem (`radar`) derlemesi etkilenmez: `platformio.ini`'de `radar` ortamı `WITH_DECK`'i kaldırır
  (`build_unflags`) ve `src/deck`'i derlemez; bu yüzden kart değişiklikleri `WITH_DECK` içinde kalır.
- Mevcut testler (`helper/tests/test_core.py` sahte kart, `test_firmware.py`, host testleri) geçer;
  yeni davranış (kapak taşıma, bekletme) sahte kartla test edilir.
- Merge yalnızca kullanıcı "Birleştir" deyince.

## Yapıcıya kalanlar

- Ölçüm raporunun alan adları, aralığı (öneri 5 sn) ve tam biçimi; döngü turunun nasıl ölçüldüğü.
- `perf.log` boyut sınırı ve biçimi.
- Ekran bildiriminin satır adı ve değerleri; programın beklettiği kapakları hangi sırayla gönderdiği.
- Kapak taşıma eşleşmesinin ayrıntısı (aynı şarkı listede iki kez geçerse).
