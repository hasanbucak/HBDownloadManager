# HB Download Manager — Safari Entegrasyonu (macOS)

Safari Web Uzantısı, standart `extension/` klasöründeki kodları birebir kullanır. Apple Safari kuralları gereği uzantı bir `.app` veya `.appex` paketleyicisi içinde çalışır.

## MacBook'ta Safari Eklentisini Hazırlama Adımları:

### Yöntem 1: Tek Komutla Otomatik Safari Projesi Oluşturma
Terminal'de projenin kök dizininde şu komutu çalıştırın:
```bash
xcrun safari-web-extension-converter extension/ \
  --project-location ./safari-build \
  --app-name "HB Download Manager" \
  --bundle-identifier "com.hasanbucak.hbdownloadmanager"
```

Bu komut:
1. `extension/` klasöründeki HTML, CSS, JS ve ikonları Safari Web Extension formatına çevirir.
2. Bir Xcode projesi üretir.

### Yöntem 2: Xcode ile Derleme ve Çalıştırma
1. Üretilen `HB Download Manager.xcodeproj` dosyasını Xcode ile açın.
2. **Product -> Run** (Cmd + R) ile projeyi bir kez derleyip çalıştırın.
3. Safari'yi açın:
   * **Safari -> Ayarlar (Preferences) -> Geliştirici (Advanced)** sekmesine gidin.
   * "Geliştirici menüsünü göster" (Show Develop menu) seçeneğini işaretleyin.
   * Üst menüden **Geliştirici (Develop) -> İmzalanmamış Uzantılara İzin Ver (Allow Unsigned Extensions)** seçeneğini açın.
   * **Safari -> Ayarlar -> Uzantılar (Extensions)** bölümünde **HB Download Manager** kutucuğunu işaretleyip aktif edin.

Artık Safari üzerinde de video oynatırken sağ üstte kayan indirme butonu belirir ve indirmeler 127.0.0.1:48213 üzerinden HB Download Manager uygulamasına yönlendirilir!
