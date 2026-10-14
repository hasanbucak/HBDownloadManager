# HB Download Manager (Linux)

[![Qt 6.x](https://img.shields.io/badge/Qt-6.x%20%7C%205.15-41CD52?logo=qt&logoColor=white)](https://www.qt.io/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![Linux](https://img.shields.io/badge/Platform-Linux%20%2F%20Ubuntu-E95420?logo=ubuntu&logoColor=white)](https://ubuntu.com/)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**HB Download Manager**, modern Linux ve Ubuntu masaüstü sistemleri için sıfırdan **C++ & Qt** ile geliştirilmiş, ultra hızlı, çok parçalı (segmented) ve tam özellikli bir dosya ve medya indirme yöneticisidir.

İnternet İndirme Yöneticisi (IDM) deneyimini Linux'a taşırken; modern sistem çağrıları (`posix_fallocate`, `pwrite64`), tarayıcı içi yüzen video indirme butonu, akıllı hız yumuşatma algoritması ve hafif yerel mimarisi ile kusursuz bir indirme hızı ve stabilitesi sunar.

---

## 🌟 Öne Çıkan Özellikler

### 1. 🚀 Çok Parçalı Yüksek Hızlı İndirme (HTTP Range)
* Dosyaları 1 ile 16 (varsayılan 8) paralel HTTP Range parçasına bölerek bant genişliğini sonuna kadar kullanır.
* **Sıfır Birleştirme Süresi (Zero-Merge Overhead):** Dosya başlangıcında `posix_fallocate` ile disk alanı tek seferde tahsis edilir. Gelen veri parçaları `pwrite64` ile doğrudan kendi disk konumlarına atomik olarak yazılır. İndirme %100 olduğunda dosya zaten birleşiktir; dakikalarca süren dosya birleştirme bekleme süresi **sıfırdır**.
* Sunucu `Range` başlığını desteklemediğinde otomatik ve şeffaf olarak tek akışa (`single stream`) düşer.

### 2. ⏸️ Kusursuz Duraklat ve Devam Et (Pause & Resume)
* **Büyük Dosyalar:** Her bir parçanın indirilen bayt ofseti 5 saniyelik periyotlarla `fdatasync` ile diske yazılır ve JSON durum kütüğüne kaydedilir. Uygulama kapatıldığında veya duraklatıldığında tüm tamponlar anında senkronize edilir; uygulama yeniden başlatıldığında `.part` dosyasındaki kaldığı bayttan devam eder.
* **Medya Akışları (yt-dlp):** Duraklatıldığında alt süreç `SIGTERM` ile nazikçe sonlandırılarak `.part` dosyasının bütünlüğü korunur. Devam ettirildiğinde `--continue` bayrağı ile kaldığı parçadan devam eder.

### 3. 🎬 IDM Tarzı Yüzen Video İndirme Butonu (Floating Overlay)
* Web sayfalarında (`YouTube`, `X / Twitter`, `Instagram`, `Facebook`, `Vimeo`, `Dailymotion`, HTML5 oynatıcılar vb.) video algılandığında oynatıcının sağ üst köşesinde IDM tarzı şık bir indirme butonu belirir.
* Sayfa stillerinden etkilenmemesi için izole **Shadow DOM** ile render edilir.
* Video başladığında 3.5 saniye görünür, fare yaklaştığında belirginleşir, fare çekildiğinde arka plana çekilir.
* Açılır kalite menüsünden:
  * 🌟 **En Yüksek Kalite (1080p / 4K MP4)**
  * 🎬 **Full HD (1080p MP4)**
  * 📺 **HD (720p MP4)**
  * 📱 **Standart (480p MP4)**
  * 🎵 **Sadece Ses (MP3)**
  * 📋 **Video Bağlantısını Kopyala**
* **Dinamik Boyut Hesabı:** Kalite seçeneklerinin yanında tahmini dosya boyutları (örn. `~45 MB`) anlık olarak gösterilir.
* **Reklam Koruması:** YouTube pre-roll veya mid-roll reklam videolarını atlayarak doğrudan ana videoyu yakalar.

### 4. 📈 1 Gbps Bağlantılar İçin Yumuşatılmış Hız Göstergesi (EMA)
* Gigabit bağlantılarda soket dalgalanmalarından kaynaklanan ani hız sıçramaları **Üstel Hareketli Ortalama (Exponential Moving Average - EMA)** filtresi ile dengelenir:
  $$\text{EMA}_t = \alpha \cdot \text{Hız}_{\text{anlık}} + (1 - \alpha) \cdot \text{EMA}_{t-1}$$
* Bu sayede indirme hızı saniyede onlarca kez sıçramaz, tamamen stabil ve akıcı görünür.

### 5. 🌐 Çoklu Tarayıcı Entegrasyonu (WebExtension & Native Messaging)
* **Google Chrome, Chromium, Brave, Microsoft Edge, Opera ve Mozilla Firefox** ile tam uyumlu WebExtension (Apple Safari deneysel / macOS şablonu).
* `127.0.0.1:48213` üzerinde hafif yerel REST köprüsü üzerinden tarayıcı indirmelerini otomatik yakalar; oturum çerezlerini (cookies), referer ve user-agent başlıklarını uygulamaya aktarır.
* **Resimler bilerek yakalanmaz:** Eklenti resim dosyalarını (png, jpg, gif, webp...) yakalamaz; bunları tarayıcı kendisi indirir. Bu bir tasarım tercihidir.
* **Native Messaging Host:** Uygulama kapalıyken tarayıcıdan indirme başlatıldığında arka planda otomatik çalıştırılır (`com.hasanbucak.hbdownloadmanager`).

### 6. 🎨 Modern Dark-Mode IDM Arayüzü
* Sol panelde kategoriler (Tümü, Belgeler, Sıkıştırılmış, Resimler, Müzik, Programlar, Videolar, Tamamlananlar, İndirilenler).
* Canlı segment görselleştirme diyaloğu (IDM tarzı 8/16 segment blok ilerleme çubukları).
* Sistem tepsisi (System Tray) ikonu, arka planda sessiz çalışma (`--tray`) ve masaüstü bildirimleri.
* Pano (Clipboard) izleyici: Bağlantı kopyalandığında otomatik indirme penceresi önerir.

---

## 🏗️ Mimari Şema

```text
+---------------------------------------------------------------+
|                      Web Tarayıcıları                         |
|   (Chrome / Brave / Edge / Opera / Firefox / Safari [Deneysel])|
|                                                               |
|  [ Content Script (Shadow DOM) ]    [ Service Worker (MV3) ]  |
|  - Yüzen Video İndirme Butonu       - İndirme Yakalama (API)  |
|  - Kalite & Boyut Listesi           - Çerez / Header Yönetimi |
+-------------------------------+-------------------------------+
                                | HTTP REST (127.0.0.1:48213)
                                | veya Native Messaging (STDIN/OUT)
                                v
+---------------------------------------------------------------+
|                    HB Download Manager Core                   |
|                                                               |
|  [ LocalServer ]             [ ClipboardWatcher ]             |
|  - REST API Köprüsü          - Pano Bağlantı Algılama         |
|                                                               |
|                     [ DownloadManager ]                       |
|                     - Kuyruk Yönetimi                         |
|                     - EMA Hız Yumuşatma                       |
|                     - JSON Durum Kalıcılığı                   |
|                              |                                |
|            +-----------------+-----------------+              |
|            |                                   |              |
|            v                                   v              |
|  [ SegmentedDownloader ]              [ VideoDownloader ]     |
|  - HTTP Range (1-16 parça)            - yt-dlp & ffmpeg       |
|  - posix_fallocate (peşin alan)       - Yapısal Progress      |
|  - pwrite64 (atomik yazım)            - HLS / DASH / Akış     |
|  - Sıfır Birleştirme Süresi           - SIGTERM nazik durdurma|
|                                                               |
|                   [ Qt6 / C++ Modern GUI ]                    |
|  - MainWindow (Dark Mode)     - Segment Görselleştirici       |
|  - Sistem Tepsisi (Tray)      - Masaüstü Bildirimleri         |
+---------------------------------------------------------------+
```

---

## 🛠️ Gereksinimler & Kurulum

### Sistem Gereksinimleri
* Ubuntu 20.04 / 22.04 / 24.04 veya Debian tabanlı modern Linux dağıtımları
* Qt 6.x veya Qt 5.15 (`Qt::Core`, `Qt::Gui`, `Qt::Widgets`, `Qt::Network`)
* GCC / G++ (C++17 destekli)
* `yt-dlp` (Güncel sürüm; Ubuntu apt deposu eski kalabildiğinden resmi ikili veya pip önerilir) ve `ffmpeg`

```bash
# Temel sistem ve Qt kütüphaneleri
sudo apt update
sudo apt install -y build-essential qt6-base-dev qt6-base-dev-tools ffmpeg

# Güncel yt-dlp kurulumu (Önerilen)
sudo curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -o /usr/local/bin/yt-dlp
sudo chmod a+rx /usr/local/bin/yt-dlp
# Alternatif: pip install -U yt-dlp
```

> `ffmpeg` yoksa video ve ses akışlarını birleştirmesi gereken medya indirmeleri başarısız olabilir. Uygulama bu durumda durum çubuğunda **"Uyarı: ffmpeg yok"** gösterir.

### Derleme (Build)

#### Seçenek 1: QMake ile Derleme (Önerilen)
```bash
# 1. Ana GUI Uygulamasını Derle
qmake hb-download-manager.pro
make -j$(nproc)

# 2. Tarayıcı Native Host İkilisini Derle
qmake hbdm-native-host.pro -o Makefile.host
make -f Makefile.host -j$(nproc)

# 3. Tarayıcı Native Host Kayıtlarını Sisteme Tanıt
./hbdm-native-host --install
```

#### Seçenek 2: CMake ile Derleme
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

---

## 🚀 Çalıştırma

```bash
# Normal modda başlatma (Pencere açık)
./hb-download-manager

# Sistem tepsisinde (arka planda) başlatma
./hb-download-manager --tray

# Komut satırından doğrudan dosya indirme
./hb-download-manager --url "https://example.com/file.zip"
```

---

## 🧩 Tarayıcı Eklentisinin Kurulumu

### Google Chrome / Chromium / Brave / Microsoft Edge
1. Tarayıcınızda `chrome://extensions` adresine gidin.
2. Sağ üst köşedeki **Geliştirici modu (Developer mode)** anahtarını açın.
3. **Paketlenmemiş öğe yükle (Load unpacked)** butonuna tıklayın.
4. Proje dizinindeki `extension/` klasörünü seçin.

### Opera / Opera GX
1. `opera://extensions` sayfasına gidin.
2. **Geliştirici modu**nu etkinleştirin.
3. **Paketlenmemiş öğe yükle** diyerek `extension/` klasörünü seçin.

### Mozilla Firefox
1. `about:debugging#/runtime/this-firefox` adresine gidin.
2. **Geçici Eklenti Yükle (Load Temporary Add-on...)** butonuna tıklayın.
3. `extension/manifest.json` dosyasını seçin.

### Apple Safari (macOS - Deneysel / Şablon)
> [!NOTE]
> Safari desteği deneysel bir Xcode şablonudur (macOS ve Xcode gerektirir).

1. Terminal üzerinden Safari uzantısını dönüştürün:
   ```bash
   xcrun safari-web-extension-converter extension/ --project-location ./safari-build --app-name "HB Download Manager"
   ```
2. Oluşan Xcode projesini derleyip Safari Ayarlar -> Uzantılar menüsünden etkinleştirin (Ayrıntılar için `safari/README.md`).

---

## 📡 REST Köprüsü API Belirtimi (`127.0.0.1:48213`)

Köprü, yalnızca yerel makineden (`127.0.0.1`) gelen istekleri kabul eder. Güvenlik için imzalı istekler ve porta bağlı bir sınama-yanıt (challenge-response) mekanizması kullanılır. Token ağ üzerinden hiçbir zaman gönderilmez:
* **Token:** `~/.config/hb-download-manager/bridge-token` dosyasında `0600` izinleriyle saklanır (64 küçük harfli onaltılık karakter). Dosyayı yalnızca uygulama ve native host okur; eklenti token'ı native host üzerinden alır.
* **İmzalı İstekler:** `GET /ping` dışındaki her istek üç başlık taşır:
  * `X-HBDM-Ts`: Unix zamanı (saniye)
  * `X-HBDM-Nonce`: 32 küçük harfli onaltılık karakter (16 rastgele bayt)
  * `X-HBDM-Sig`: `hex(HMAC-SHA256(token, "hbdm-req:v1\n" + METOT + "\n" + yol + "\n" + ts + "\n" + nonce + "\n" + hex(SHA-256(gövde))))`

  `yol`, istekte gönderildiği haliyle (sorgu dizesi dahil) yazılır. Sunucu, zamanı 30 saniyeden fazla sapan istekleri ve son 120 saniyede görülmüş bir `nonce`'u reddeder; imza sabit zamanlı karşılaştırılır. Eski `X-HBDM-Token` başlığı artık kabul edilmez.
* **Porta Bağlı Ping Kanıtı:** Eklenti `/ping?nonce=<32 hex>` çağırır. Yalnızca `nonce` tam olarak `^[0-9a-f]{32}$` ise sunucu `proof = hex(HMAC-SHA256(token, "hbdm-ping:v1:" + dinlenen_port + ":" + nonce))` döner. Kanıt porta bağlı olduğundan başka bir portta dinleyen sahte bir süreç onu aktaramaz. Token alınamıyorsa eklenti uygulamayı çevrimdışı sayar ve tarayıcı indirmeyi kendisi sürdürür.

| Metot | Uç Nokta | Açıklama |
|---|---|---|
| `GET` | `/ping` | Servis durumu, sürüm, yt-dlp/ffmpeg varlığı, aktif indirmeler ve HMAC kanıtı |
| `GET` | `/downloads` | Son 50 indirmenin durum, hız, yüzde ve boyut bilgileri (İmza zorunlu) |
| `POST` | `/download` | Yeni indirme başlatır (`url`, `filename`, `cookies`, `kind`, `quality`, vb.) (İmza zorunlu) |
| `POST` | `/action` | İndirmeyi yönetir (`id`, `action: "pause" \| "resume" \| "cancel" \| "remove"`) (İmza zorunlu) |

---

## 💾 Durum Kaydı (Süreklilik)

İlerleme anlık değil, **düzenli aralıklarla** kalıcı hale getirilir: etkin indirmelerin parça dosyası her 5 saniyede bir `fdatasync` ile diske yazılır ve ancak ondan sonra `downloads.json` içine kaydedilen sayaçlar güncellenir. Duraklatma ve uygulamadan çıkış sırasında da aynı işlem yapılır. Böylece JSON, diskteki veriden hiçbir zaman ileride olmaz. Beklenmedik bir çökmede en fazla son birkaç saniyelik veri yeniden indirilir.

---

## 🧪 Testler

```bash
./run_tests.sh
```

Betik, `tests/` altında `qmake` (`qmake6` veya PATH'teki `qmake`; `QMAKE=/yol/qmake` ile değiştirilebilir) ile tüm test ikililerini derler ve **hepsini** çalıştırır. Eksik bir ikili veya başarısız bir test çıkış kodunu sıfırdan farklı yapar; "ALL TESTS PASSED" yalnızca her şey geçtiğinde yazılır. Ayrıca `tests/test_server.py --self-test`, eklenti dosyalarında `node --check` ve `scripts/check_version.sh` çalıştırılır, gerçek `downloads.json` ve `bridge-token` dosyalarının değişmediği md5 ile doğrulanır. Testler `HOME` ve `XDG_*` dizinlerini geçici bir klasöre taşır (`tests/test_guard.h`).

CMake ile: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` (her test `QT_QPA_PLATFORM=offscreen` ile kayıtlıdır). GitHub Actions iş akışı `.github/workflows/ci.yml` aynısını yapar.

---

## 🌍 Diller

Kaynak dil **Türkçe**dir: `tr()` metinleri Türkçe yazılır ve Türkçe sistemlerde çeviri yüklenmez. İngilizce katalog `translations/hbdm_en.ts` dosyasındadır; `lrelease` ile derlenip uygulamaya gömülür (`:/i18n/`). Katalog yalnızca sistemin **ilk** arayüz dili Türkçe değilse yüklenir (ikinci dili İngilizce olan bir Türk kullanıcı Türkçe görür). Metinleri değiştirince `lupdate src -ts translations/hbdm_en.ts` ile kataloğu güncelleyin.

---

## 🔢 Sürüm

Tek kaynak `VERSION` dosyasıdır; qmake (`$$cat`) ve CMake (`file(READ)`) bunu `HBDM_VERSION` olarak derlemeye verir. `scripts/check_version.sh`, `extension/manifest.json` ve `safari/Info.plist` ile tutarlılığı denetler.

---

## 📄 Lisans

Bu proje [MIT Lisansı](LICENSE) kapsamında açık kaynak olarak lisanslanmıştır.
