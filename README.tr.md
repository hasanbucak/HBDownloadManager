# HB Download Manager (Linux / Ubuntu Versiyonu)

Linux ekosistemi için **C++ & Qt (Qt 6.11 / Qt 5.15)** ile sıfırdan geliştirilmiş, yüksek performanslı, çok parçalı ve yerel Linux / Ubuntu indirme yöneticisi sürümüdür.

Standart Qt/C++ mimari ve kodlama kurallarına (`m_` üye değişkenleri, `at*` sinyalleri, `on*` slotları, açık `emit`, açık veri tipleri ve QObject bellek hiyerarşisi) tam uyumlu olarak inşa edilmiştir.

---

## 🚀 Özellikler

1. **Çok Parçalı İndirme Motoru (HTTP Range):**
   * Dosyayı 1–16 (varsayılan 8) paralel HTTP Range parçasına böler.
   * `posix_fallocate` ile disk alanını peşin tahsis ederek dosya sistemi parçalanmasını (fragmentation) engeller.
   * `pwrite64` ile gelen baytları kendi offset'lerine atomik yazar; indirme bittiğinde dosyaları birleştirme (merge) bekleme süresi **sıfırdır**.
   * Sunucu `Range` desteklemiyorsa otomatik tek akışa (`single stream`) düşer.
2. **Kusursuz Duraklat / Devam Et (Pause & Resume):**
   * **Büyük Dosyalar:** Parça bayt offsetleri diske ve JSON state dosyasına anlık kaydedilir. Duraklatıldığında veya uygulama yeniden açıldığında `.part` dosyasındaki kaldığı bayttan devam eder.
   * **Video Akışları (yt-dlp):** Duraklatıldığında süreç `SIGTERM` ile nazikçe kapatılarak `.part` dosyası korunur. Devam et denildiğinde `--continue` ile kaldığı bayt/parçadan indirilir.
3. **IDM Tarzı Yüzen Video İndirme Butonu (Floating Overlay Button) & Kalite Seçimi:**
   * Web sayfalarında (`YouTube`, `Twitter/X`, `Vimeo`, `Dailymotion`, HTML5 video oynatıcılar vb.) video algılandığında oynatıcının sağ üst köşesinde IDM tarzı şık bir indirme butonu belirir.
   * Video başladığında 3.5 saniye kendini gösterir, fare oynatıcı üzerine geldiğinde parlar, fare çekildiğinde arka plana kaybolur.
   * Açılır menüden **"🌟 En Yüksek Kalite (1080p/4K MP4)"**, **"🎬 Full HD (1080p MP4)"**, **"📺 HD (720p MP4)"**, **"📱 Standart (480p MP4)"**, **"🎵 Sadece Ses (MP3)"** veya **"📋 Video Bağlantısını Kopyala"** seçilebilir.
   * Reklam koruması: Tarayıcıdaki geçici reklam videoları atlanır, doğrudan ana kanonik video ID'si yakalanarak reklamsız orijinal video akışı indirilir.
   * Sayfa CSS stillerinden etkilenmemesi için izole **Shadow DOM** ile render edilir.
4. **1 Gbps Bağlantılar İçin Yumuşatılmış Stabil Hız Göstergesi (EMA):**
   * Gigabit internet hatlarında soket dalgalanmalarından kaynaklanan saniyede 10 kez hız sıçraması problemi **Üstel Hareketli Ortalama (Exponential Moving Average)** ile filtrelenerek tamamen akıcı ve stabil hale getirilmiştir.
5. **Tarayıcı Entegrasyonu (WebExtension & Native Messaging):**
   * **Chrome, Chromium, Brave, Edge ve Firefox** için tam uyumlu WebExtension (`extension/`).
   * `127.0.0.1:48213` üzerinde hafif REST köprüsü dinler. Tarayıcı indirmeyi iptal edip çerezler, referer ve user-agent ile uygulamaya aktarır.
   * **Native Messaging Host:** Uygulama kapalıyken tarayıcıdan indirme başlatıldığında arka planda otomatik çalıştırılır (`com.hasanbucak.hbdownloadmanager`).
6. **Video / Medya Akışı İndirme:**
   * Web sayfalarındaki video, HLS (`.m3u8`) ve DASH (`.mpd`) akışlarını `yt-dlp`, `ffmpeg` ve Node.js JS motoru üzerinden en yüksek kalitede indirip birleştirir.
   * Otomatik gerçek video başlığını algılar (`watch` yerine videonun orijinal adını verir).
7. **Pano (Clipboard) İzleyici:**
   * Panoya bir dosya veya video bağlantısı kopyalandığında algılayıp indirme penceresi sunar.
8. **IDM Tarzı Arayüz (Dark Mode):**
   * Sol panelde kategoriler (Resimler, Müzik, Videolar, Programlar, Belgeler, Sıkıştırılmış vb.).
   * Canlı segment ilerleme çubukları (IDM tarzı 8/16 segment görselleştirme diyaloğu).
   * Sistem tepsisi (System Tray) ikonu ve masaüstü bildirimleri.

---

## 🛠️ Derleme ve Çalıştırma

### Gereksinimler
* Ubuntu 20.04 / 22.04 / 24.04
* Qt 6.x veya Qt 5.15 (`Qt::Core`, `Qt::Gui`, `Qt::Widgets`, `Qt::Network`)
* `g++` (C++17 desteği)
* `yt-dlp` (Video indirme desteği için)

### Derleme Adımları
```bash
# 1. Ana GUI Uygulamasını Derle
qmake hb-download-manager.pro
make -j$(nproc)

# 2. Tarayıcı Native Host İkilisini Derle
qmake hbdm-native-host.pro -o Makefile.host
make -f Makefile.host -j$(nproc)

# 3. Tarayıcı Native Host Kayıtlarını Otomatik Kur (Tek Komut)
./hbdm-native-host --install
```

### Çalıştırma
```bash
./hb-download-manager
# veya arka planda tepside başlatmak için:
./hb-download-manager --tray
```

---

## 🌐 Tarayıcı Eklentisinin Kurulumu

### Google Chrome / Chromium / Brave / Edge:
1. `chrome://extensions` adresine gidin.
2. Sağ üstteki **Geliştirici modu (Developer mode)** anahtarını açın.
3. **Paketlenmemiş öğe yükle (Load unpacked)** butonuna tıklayın.
4. Proje dizinindeki `extension/` klasörünü seçin.

### Opera / Opera GX:
1. `opera://extensions` adresine gidin.
2. Sağ üstteki **Geliştirici modu** anahtarını açın.
3. **Paketlenmemiş öğe yükle** butonuna tıklayın.
4. Proje dizinindeki `extension/` klasörünü seçin.

### Mozilla Firefox:
1. `about:debugging#/runtime/this-firefox` adresine gidin.
2. **Geçici Eklenti Yükle (Load Temporary Add-on...)** butonuna tıklayın.
3. `extension/manifest.json` dosyasını seçin.

### Apple Safari (macOS - Deneysel / Şablon):
> [!NOTE]
> Safari desteği deneysel bir Xcode şablonudur (macOS ve Xcode gerektirir).

1. Terminal'de tek komutla Safari uzantısını dönüştürün:
   ```bash
   xcrun safari-web-extension-converter extension/ --project-location ./safari-build --app-name "HB Download Manager"
   ```
2. Oluşan Xcode projesini derleyin ve Safari Ayarlar -> Uzantılar menüsünden aktif edin. (Detaylar `safari/README.md` dosyasında).

---

## 📡 REST Köprüsü API Belirtimi (127.0.0.1:48213)

| Metot | Yol | Açıklama |
|---|---|---|
| `GET` | `/ping` | Sürüm, aktif indirme sayısı ve yakalanacak dosya uzantıları |
| `GET` | `/downloads` | Son 50 indirmenin durum, ilerleme ve hız bilgisi |
| `POST` | `/download` | `{ url, filename, referer, userAgent, cookieHeader, cookies, kind, quality, segments }` |
| `POST` | `/action` | `{ id, action: "pause" \| "resume" \| "cancel" \| "remove" }` |

*Köprü yalnızca yetkili tarayıcı uzantılarından gelen CORS isteklerini kabul eder.*
