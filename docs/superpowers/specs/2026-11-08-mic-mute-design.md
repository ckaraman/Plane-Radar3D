# Kartın ses düğmesinde mikrofon susturma (10q)

## Amaç

Toplantı veya oyun sırasında bilgisayarın mikrofonunu karttan tek dokunuşla kapatmak ve
mikrofonun kapalı olup olmadığını kartın üzerinde görmek.

## Kapsam

- Yalnız tuşlar sayfasındaki ses halkası değişir. Müzik ve sıra ekranlarına mikrofon gelmez.
- Windows. Mac'te program mikrofon bilgisi göndermez, düğme bugünkü gibi tek parça kalır.
- Kullanılmayacak: uygulama içi susturma (Discord / Teams'in kendi düğmesi). Windows düzeyinde
  susturma o uygulamaların simgesini değiştirmez; bu kabul edildi.

## Görünüm (seçilen: A, ince çizgi)

- Dış halka ses seviyesi olarak aynı kalır.
- İç düğme yatay ince bir çizgiyle ikiye bölünür: üstte hoparlör simgesi, altta mikrofon simgesi.
- Kapalı olanın yalnız simgesi kırmızıya (`kMutedRed`) döner; mikrofon simgesinin üstünde çizgi olur.
- Mikrofon yoksa ya da program bağlı değilse düğme bugünkü gibi tek parçadır.

## Davranış

- Halkanın içinde üst yarıya dokunmak hoparlörü, alt yarıya dokunmak mikrofonu aç / kapa yapar.
  Dokunma kuralı bugünkü dokunma kuralıyla aynıdır (az hareket, 700 ms altı).
- Halkayı çevirmek eskisi gibi sesi değiştirir, mikrofona dokunmaz.
- Tek parça haldeyken dokunuş bugünkü gibi hoparlörü susturur.
- Hoparlör bugünkü gibi dokunulduğu anda değişir. Mikrofon bilerek öyle değildir: kart kırmızıyı
  yalnız program bildirince gösterir, ekrandaki durum gerçek durumdur.

## Hangi mikrofonlar (seçilen: Yol 3, hepsi)

Windows'taki etkin (`DEVICE_STATE_ACTIVE`) bütün kayıt cihazları. Sanal cihazlar da buna dahildir.

- Dokunuşta: etkin mikrofonlardan biri bile açıksa hepsi kapanır; hepsi kapalıysa hepsi açılır.
  Önceden tek tek kapalı olanlar hatırlanmaz, açarken onlar da açılır (kabul edildi).
- Kartın gördüğü durum: **yalnız hepsi kapalıysa "kapalı"**, biri bile açıksa "açık". Yeni takılan
  açık bir mikrofon kartı "açık"a çevirir; kart yanlışlıkla "kapalı" göstermez.
- Hiç etkin mikrofon yoksa "yok".
- Windows'tan ya da klavyeden yapılan değişiklikler de karta yansır.

## Mesajlar

- Program → kart: var olan `PR:VOL` satırına `mic` alanı eklenir: `0` yok, `1` açık, `2` kapalı.
- Kart → program: yeni `PR:MICMUTE {}`.
- Eski kart `mic` alanını yok sayar. Eski program kartta `mic` göndermediği için düğme tek parça
  kalır. Yeni satırı tanımayan eski program onu yok sayar.

## Yollar

| | Ne yapar | Sonuç |
|---|---|---|
| 1. Yalnız varsayılan mikrofon | Tek cihaz | Görüşmedeki mikrofon açık kalabilir, elendi |
| 2. Varsayılan ve iletişim mikrofonu | İki rol | Elle seçilen mikrofonu kapatmaz, elendi |
| **3. Bütün etkin mikrofonlar** | Her cihaz | Seçildi; başka kayıtları da keser |

## Uygulayıcının kararları (kullanıcı seçmedi)

- Mikrofon bilgisi `send_volume` ayarına bağlıdır; ayar kapalıysa düğme tek parça kalır.
- Cihaz listesi `volume.py`'deki ses izleme iş parçacığında, 5 saniyede bir yenilenir; durum
  yaklaşık 250 ms'de bir okunur. Yeni takılan cihaz en geç 5 sn'de görünür.
- Dokunma alanının sınırı halkanın yatay ortası (`kKnobCy`) olur.

## Denenecekler

- Birden çok mikrofonla: dokununca hepsi kapanır, Windows ses ayarlarında görünür.
- Windows'tan birini açınca kart "açık"a döner.
- Mikrofon çıkarılınca, program kapanınca düğme tek parçaya döner.
- Üst ve alt yarı karışmaz; halkayı çevirmek susturmaz.
