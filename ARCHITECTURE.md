# HB Download Manager — Detaylı Mimari & Kod Kılavuzu (Architecture & AI Guide)

Bu doküman, **HB Download Manager** projesinin tüm mimarisini, veri akışlarını, bileşenlerini ve kullanılan algoritmaları ayrıntılı olarak açıklamaktadır. Projeyi inceleyen veya üzerinde geliştirme yapacak herhangi bir yazılımcı ya da yapay zeka modelinin kod tabanını sıfır kafa karışıklığıyla tam olarak anlaması için hazırlanmıştır.

---

## 1. Genel Bakış ve Temel Felsefe

**HB Download Manager**, Linux/Ubuntu ortamında Internet Download Manager (IDM) performansını ve kullanıcı deneyimini sunmak üzere **C++ (C++17) ve Qt (Qt 6.x / Qt 5.15)** kullanılarak sıfırdan inşa edilmiş, yerel (native) ve çok parçalı bir indirme yöneticisidir.

### Temel Hedefler:
1. **Maksimum Bant Genişliği:** Dosyaları dinamik olarak 1–16 parçaya bölerek çoklu HTTP Range bağlantısıyla indirmek.
2. **Sıfır Birleştirme Süresi (Zero-Merge Overhead):** Geleneksel indirme yöneticilerinin aksine, indirme bittiğinde parçaları birleştirmek için diske fazladan okuma/yazma yapmaz; `posix_fallocate` ve `pwrite64` ile veriyi doğrudan diskteki nihai konumuna yazar.
3. **Kusursuz Pause & Resume:** Hem standart HTTP dosyalarında hem de yt-dlp ile indirilen video akışlarında kaldığı bayttan devam edebilme.
4. **Tarayıcı İçi Video Yakalama (Floating Overlay):** Web sayfalarındaki videolarda (YouTube, X/Twitter, Instagram, Facebook vb.) izole Shadow DOM ile yüzen indirme butonu, kalite menüsü ve canlı boyut gösterimi sunmak.
5. **1 Gbps Hatlar İçin Stabil Hız:** Yüksek hızlı bağlantılarda hız göstergesinin saniyede onlarca kez sıçramasını Üstel Hareketli Ortalama (EMA) ile engellemek.

---

## 2. Sistem Mimarisi ve Veri Akışı

```text
+---------------------------------------------------------------------------------+
|                                WEB TARAYICILARI                                 |
|            (Google Chrome, Chromium, Brave, Edge, Opera, Firefox, Safari)       |
|                                                                                 |
|  [ Content Script (content.js) ]               [ Service Worker (background.js) ] |
|  - HTML5 / YouTube / X video algılama          - chrome.downloads.onCreated dinle  |
|  - Shadow DOM içinde yüzen indirme butonu      - Tarayıcı indirmesini iptal et     |
|  - Kalite menüsü (1080p, 720p, MP3 vb.)       - Cookie, Referer, User-Agent topla |
|  - Anlık boyut tahmini (HEAD / probe)          - REST API veya Native Host'a ilet  |
+---------------------------------------+-----------------------------------------+
                                        | HTTP POST /download (127.0.0.1:48213)
                                        | veya STDIN/STDOUT Native Messaging
                                        v
+---------------------------------------------------------------------------------+
|                          HB DOWNLOAD MANAGER (C++ / QT)                         |
|                                                                                 |
|  +------------------------+  +----------------------+  +---------------------+  |
|  |      LocalServer       |  |   hbdm-native-host   |  |   ClipboardWatcher  |  |
|  |  (127.0.0.1:48213)     |  | (Native Messaging)   |  |  (Pano Link Takibi) |  |
|  |  - /ping               |  |  - 4-byte uzunluk    |  |  - URL regex        |  |
|  |  - /downloads          |  |  - JSON I/O köprüsü  |  |  - Yeni indirme aç  |  |
|  |  - /download           |  +----------+-----------+  +----------+----------+  |
|  |  - /action             |             |                         |             |
|  +-----------+------------+             +------------+------------+             |
|              |                                       |                          |
|              +-------------------> + <---------------+                          |
|                                    |                                            |
|                                    v                                            |
|                         [ DownloadManager (Core) ]                              |
|                         - İndirme kuyruğu ve durum makinesi                     |
|                         - JSON durum kalıcılığı (~/.config/.../downloads.json)  |
|                         - 1 Gbps EMA hız yumuşatma filtresi                     |
|                         - Video vs. Standart dosya ayrımı                       |
|                                    |                                            |
|              +---------------------+---------------------+                      |
|              |                                           |                      |
|              v                                           v                      |
|   [ SegmentedDownloader ]                       [ VideoDownloader ]             |
|   - HTTP HEAD (Boyut, Range desteği)            - yt-dlp + ffmpeg alt süreci    |
|   - posix_fallocate (peşin disk alanı)          - --progress-template HBDM_PROG |
|   - 1-16 QNetworkReply Range iş parçacığı       - MergedChannels (stdout+stderr)|
|   - pwrite64 (doğrudan atomik blok yazım)       - Canlı byte, ETA, hız ayrıştırma|
|   - JSON parça durum kaydı                      - SIGTERM ile nazik duraklatma  |
|   - Sıfır birleştirme bekleme süresi            - Disk boyutu teyidi            |
|                                    |                                            |
|                                    v                                            |
|                          [ Qt6 Grafik Arayüzü ]                                 |
|   - MainWindow: Dark mode tema, kategori filtreleme, indirme tablosu            |
|   - DownloadDetailDialog: IDM tarzı görsel 8/16 segment blok göstergesi         |
|   - NewDownloadDialog: URL, klasör ve segment sayısı seçim diyaloğu             |
|   - TrayManager: Sistem tepsisi simgesi, arka plan modu (--tray), bildirimler   |
+---------------------------------------------------------------------------------+
```

---

## 3. Dizin ve Dosya Yapısı (Kod Haritası)

### Kök Dizin
* **`hb-download-manager.pro`**: Ana GUI uygulaması için Qt QMake proje yapılandırma dosyası.
* **`hbdm-native-host.pro`**: Tarayıcı Native Messaging Host ikilisi için QMake yapılandırma dosyası.
* **`CMakeLists.txt`**: Modern CMake derleme sistemi (QMake yerine CMake tercih eden ortamlar için).
* **`hb-download-manager.desktop`**: Linux masaüstü ortamları (GNOME, KDE vb.) için uygulama başlatıcı dosyası.
* **`README.md` & `README.tr.md`**: Kullanıcı ve geliştirici karşılama dokümanları.
* **`ARCHITECTURE.md`**: Bu detaylı teknik kılavuz.
* **`.gitignore`**: Derleme çıktıları (`*.o`, `moc_*`, ikililer) için Git yoksayma listesi.

---

### Kaynak Kodlar (`src/`)

#### 1. Çekirdek İndirme Motoru (`src/core/`)
* **`DownloadItem.h` & `DownloadItem.cpp`**:
  * İndirilen her bir görevin veri modelidir.
  * Durumlar (`State` enum): `Queued`, `Downloading`, `Paused`, `Completed`, `Error`, `Canceled`.
  * Alanlar: URL, dosya adı, hedef dizin, toplam boyut (`m_totalBytes`), indirilen boyut (`m_receivedBytes`), anlık hız (`m_speedBps`), EMA yumuşatılmış hız (`m_smoothedSpeedBps`), kalan süre (`m_etaSeconds`), segment sayısı, video kalitesi, tür (`Direct` veya `Video`), hata mesajı.
  * JSON serileştirme (`toJson()` / `fromJson()`): İndirme listesinin diskte saklanmasını sağlar.

* **`SegmentInfo.h`**:
  * Parçalı indirmedeki tek bir HTTP segmentinin bilgilerini tutar (`m_id`, `m_startOffset`, `m_endOffset`, `m_currentOffset`, `m_downloadedBytes`, `m_isFinished`).

* **`SegmentedDownloader.h` & `SegmentedDownloader.cpp`**:
  * Standart HTTP/HTTPS çok parçalı indirme motoru.
  * **Adım 1:** Hedef URL'ye `HEAD` isteği atarak `Content-Length` ve `Accept-Ranges: bytes` başlıklarını sorgular.
  * **Adım 2:** Sunucu `Range` destekliyorsa, dosya boyutunu `N` eşit parçaya böler. Desteklemiyorsa tek segment (`0` ile dosya sonu) oluşturur.
  * **Adım 3 (Peşin Alan Tahsisi):** `open()` ile hedef dosya açılır ve `posix_fallocate(fd, 0, totalSize)` çağrılır. Bu işlem:
    * Diskteki parçalanmayı (fragmentation) sıfıra indirir.
    * Diskte yer yetersizse en başta hata verir (indirme ortasında disk dolup patlamaz).
  * **Adım 4 (Paralel İstekler):** Her parça için `Range: bytes=start-end` başlığıyla bir `QNetworkReply` başlatılır.
  * **Adım 5 (Atomik Yazım):** Gelen her veri bloğu `pwrite64(fd, data, len, offset)` ile dosyadaki tam ofsetine yazılır.
  * **Sıfır Birleştirme (Zero-Merge):** Tüm parçalar bittiğinde `close(fd)` yapılır ve işlem anında tamamlanır. Parçaları birleştirmek için fazladan disk G/Ç süresi gerekmez!

* **`VideoDownloader.h` & `VideoDownloader.cpp`**:
  * `yt-dlp` ve `ffmpeg` motorunu yöneten medya indirme sınıfıdır.
  * YouTube, X (Twitter), Instagram, Facebook, Vimeo vb. yüzlerce siteden ses/video akışlarını yakalar.
  * **Yapısal İlerleme (`--progress-template`):**
    * yt-dlp çağrısına özel şablon eklenir:
      `--progress-template "download:HBDM_PROG:%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress._speed_str)s|%(progress._eta_str)s|%(progress._percent_str)s|%(progress.filename)s"`
    * Bu sayede konsol çıktısındaki karmaşık karakterler yerine kesin sayısal değerler ayrıştırılır.
  * **Kanal Birleştirme (`MergedChannels`):** X/Twitter gibi HLS tabanlı akışlarda yt-dlp çıktıları stderr ve stdout arasında bölünebildiğinden, `QProcess::MergedChannels` kullanılarak hiçbir ilerleme mesajı kaçırılmaz.
  * **Nazik Duraklatma (Graceful Pause):** İndirme duraklatıldığında `SIGTERM` gönderilir. Böylece yt-dlp açık dosyaları düzgünce kapatır ve `.part` dosyası bozulmadan kalır. Devam ettirildiğinde `--continue` ile kaldığı yerden devam eder.
  * **Diskten Boyut Teyidi:** İndirme tamamlandığında dosyanın diskteki net boyutu (`QFileInfo::size()`) okunarak tabloya yazılır.

* **`DownloadManager.h` & `DownloadManager.cpp`**:
  * Tüm indirme sürecini yöneten merkezi kontrolör (orchestrator).
  * İndirme kuyruğunu (`m_items`) ve aktif downloader nesnelerini yönetir.
  * 1 saniyelik zamanlayıcı (`QTimer`) ile indirme hızlarını günceller ve EMA algoritmasını uygular.
  * JSON durum dosyasına periyodik kaydeder (`~/.config/hb-download-manager/downloads.json`).

---

#### 2. Sunucu ve IPC Katmanı (`src/server/` & `src/native_host/`)
* **`LocalServer.h` & `LocalServer.cpp`**:
  * `127.0.0.1:48213` IP ve portunu dinleyen yerel HTTP REST sunucusudur (`QTcpServer`).
  * Güvenlik: Yalnızca `127.0.0.1` bağlantılarını kabul eder. CORS başlıklarında yetkili tarayıcı uzantı ID'lerini ve `X-HBDM-Extension` başlığını kontrol eder.
  * Uç Noktalar (Endpoints):
    * `GET /ping`: Uygulamanın ayakta olup olmadığını, sürümünü ve yakalanan dosya uzantılarını döner.
    * `GET /downloads`: Aktif ve geçmiş indirmelerin durumunu döner.
    * `POST /download`: Tarayıcıdan gönderilen yeni indirme isteğini (`url`, `cookies`, `referer`, `quality`, vb.) karşılayıp kuyruğa ekler.
    * `POST /action`: İndirmeleri duraklatma, devam ettirme veya silme isteklerini yürütür.

* **`src/native_host/native_host_main.cpp`**:
  * Chrome ve Firefox Native Messaging standardına uygun C++ ikilisidir (`hbdm-native-host`).
  * Standart girdi (`stdin`) ve çıktıyı (`stdout`) 4 baytlık küçük endian (little-endian) uzunluk öneki ile JSON mesajları üzerinden okur.
  * Uygulama kapalıyken tarayıcıdan indirme başlatıldığında `hb-download-manager` uygulamasını otomatik olarak arka planda uyandırır.

---

#### 3. Kullanıcı Arayüzü Katmanı (`src/ui/`)
* **`MainWindow.h` & `MainWindow.cpp`**:
  * Modern Dark-Mode IDM benzeri ana pencere.
  * Sol panel: Kategori listesi (Tümü, Belgeler, Sıkıştırılmış, Resimler, Müzik, Programlar, Videolar, Tamamlananlar, İndirilenler).
  * Orta panel: İndirme tablosu (Dosya Adı, Boyut, İlerleme Çubuğu, Hız, Kalan Süre, Durum, Eklenme Tarihi).
  * Üst araç çubuğu: Yeni İndirme, Duraklat, Devam Et, İptal Et, Sil, Klasörde Göster.
* **`DownloadDetailDialog.h` & `DownloadDetailDialog.cpp`**:
  * Seçili indirmenin ayrıntılarını ve IDM tarzı çok parçalı (8/16 segment) canlı ilerleme bloklarını gösteren diyalog.
* **`NewDownloadDialog.h` & `NewDownloadDialog.cpp`**:
  * Manuel URL girişi, kategori seçimi, hedef klasör ve segment sayısı (1–16) belirleme penceresi.
* **`TrayManager.h` & `TrayManager.cpp`**:
  * Sistem tepsisi (System Tray) entegrasyonu.
  * Pencere kapatıldığında arka planda çalışmaya devam etme veya `--tray` parametresiyle doğrudan tepside başlama desteği.

---

#### 4. Yardımcı Bileşenler (`src/utils/`)
* **`ClipboardWatcher.h` & `ClipboardWatcher.cpp`**:
  * Sistem panosunu (`QClipboard`) izler.
  * Panoya geçerli bir HTTP/HTTPS dosya veya video bağlantısı kopyalandığında otomatik indirme diyaloğunu tetikler.

---

### Tarayıcı Eklentisi (`extension/`)
* **`manifest.json`**: Manifest V3 standardında Chrome, Edge, Brave, Opera ve Firefox uyumlu eklenti manifesti.
* **`background.js`**:
  * Service worker. `chrome.downloads.onCreated` olayını dinler.
  * İndirilen dosyanın uzantısını `captureExtensions` listesiyle karşılaştırır.
  * Uygunsa tarayıcı indirmesini derhal iptal eder (`chrome.downloads.cancel`).
  * `chrome.cookies.getAll` ile ilgili alanın oturum çerezlerini toplar ve `POST http://127.0.0.1:48213/download` ile C++ uygulamasına iletir.
* **`content.js`**:
  * Web sayfalarına enjekte edilen içerik betiği.
  * `MutationObserver` ile sayfadaki `<video>` öğelerini ve dinamik oynatıcıları (YouTube `#movie_player`, X/Twitter `[data-testid="videoComponent"]`, Instagram video kapsayıcıları) sürekli izler.
  * Oynatıcının üzerine izole bir **Shadow DOM** içinde şık bir yüzen indirme butonu yerleştirir.
  * Kalite seçeneklerini ve tahmini dosya boyutlarını sunar.
  * Video indirme tıklandığında `background.js` üzerinden `kind: "video"` bayrağı ile C++ uygulamasına gönderir.
* **`overlay.css`**: Yüzen butonun karanlık tema IDM stilleri, hover animasyonları ve açılır menü tasarımı.

---

### Safari Eklentisi (`safari/`)
* Apple Safari (macOS) için Web Extension dönüşüm dosyaları, `SafariWebExtensionHandler.swift` ve Xcode proje şablonu.

---

## 4. Özel Algoritmalar ve Kritik Tasarım Kararları

### 1. Sıfır Birleştirme Süreli Disk Yazımı (Zero-Merge Architecture)
Geleneksel araçlar her parçayı `part_0.tmp`, `part_1.tmp` gibi ayrı dosyalara yazar ve indirme bitince bu dosyaları birleştirir. 10 GB'lık bir dosyada bu işlem diski dakikalarca kilitler.
**HB Download Manager Çözümü:**
1. Dosya boyutu öğrenildiği anda `posix_fallocate(fd, 0, totalSize)` çağrılır.
2. Segment $k$, $O_k$ ofsetinden başladığında, gelen veriler `pwrite64(fd, buf, len, O_k + downloaded)` ile doğrudan nihai dosyaya yazılır.
3. Son bayt yazıldığında dosya hazırdır; birleştirme süresi **0 milisaniyedir**.

### 2. 1 Gbps Üstel Hareketli Ortalama (EMA) Hız Filtresi
Gigabit internette TCP pencere boyutlarındaki dalgalanmalar nedeniyle ham hız hesabı saniyede onlarca kez sıçrar. Bunu önlemek için EMA filtresi uygulanır:
$$\text{EMA}_t = \alpha \cdot \text{Hız}_{\text{anlık}} + (1 - \alpha) \cdot \text{EMA}_{t-1}$$
Burada $\alpha = 0.25$ seçilmiştir. Bu değer, hem ani hız düşüşlerini/artışlarını 1-2 saniye içinde hassas biçimde yakalar hem de saniyelik titreşimleri filtreleyerek kullanıcıya sakin ve okunaklı bir hız değeri sunar.

### 3. X (Twitter) & HLS Akışlarının Yönetimi
X (Twitter) ve benzeri sosyal ağlar videoları parçalı HLS (`.m3u8`) akışları olarak sunar. Bu formatta:
* Boyut baştan kesin bilinmez, yt-dlp `~ 14.5MiB` şeklinde yaklaşık boyut bildirir.
* ETA veya hız satırları konsol çıktısında `stderr` kanalına düşebilir.
* **Çözüm:** `QProcess::MergedChannels` ile tüm kanallar tek hatta birleştirildi ve özel `--progress-template` parametresiyle kesin byte ve tahmini byte ayrıştırıldı. İndirme bittiğinde diskteki dosya boyutu sorgulanarak arayüzdeki boyut `0` kalmaktan kurtarıldı.

---

## 5. PaySol / ASIS C++ Kodlama Standartları Uyumu

Projedeki tüm C++ sınıfları aşağıdaki standartlara sıkı sıkıya bağlı kalarak yazılmıştır:
* **Değişken İsimlendirme:** Sınıf üye değişkenleri kesinlikle `m_` önekiyle başlar (örn. `m_totalBytes`, `m_process`). Statik değişkenler `s_` ile başlar.
* **Sinyal ve Slot İsimlendirmesi:** Sinyaller `at*` önekiyle (örn. `atProgressUpdated`, `atFinished`), slotlar `on*` önekiyle (örn. `onProgressUpdated`, `onFinished`) adlandırılır.
* **Açık Sinyal Yayımı:** Sinyaller her zaman `emit atSignalName(...)` şeklinde açık anahtar kelime ile tetiklenir.
* **Bellek Yönetimi:** Tüm QObject türevi nesneler ebeveyn-çocuk hiyerarşisine (`parent-child`) göre yönetilir; bellek sızıntısı (memory leak) oluşması engellenir.
* **Yasaklı İfadeler:** Kod tabanında, commit geçmişinde veya dokümanlarda hiçbir harici kişisel isim veya yasaklı referans barındırılmaz; proje ismi **HB Download Manager** (`hb-download-manager`) olarak tescillidir.

---

## 6. Projeyi Derleme, Çalıştırma ve Test Etme

### Derleme:
```bash
# Ana GUI
qmake hb-download-manager.pro
make -j$(nproc)

# Native Messaging Host
qmake hbdm-native-host.pro -o Makefile.host
make -f Makefile.host -j$(nproc)

# Native Host Entegrasyonu
./hbdm-native-host --install
```

### Canlılık ve API Testi:
```bash
# Servisi arka planda başlat
./hb-download-manager --tray &

# Ping atarak kontrol et
curl -s -H "X-HBDM-Extension: chrome" http://127.0.0.1:48213/ping

# Beklenen Yanıt:
# {"active":0,"app":"HBDownloadManager","captureExtensions":[...],"ok":true,"version":"1.3.0","ytdlp":true}
```
