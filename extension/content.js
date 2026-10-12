// HB Download Manager — content script
// 1) Intercepts download clicks (fallback / browser-independent).
// 2) Reports <video>/<audio>/<source> URLs on the page to the background script.
// 3) IDM-Style Floating Video Download Button Overlay over all detected video players!

(() => {
  'use strict';
  const api = typeof browser !== 'undefined' ? browser : chrome;
  const DL_EXT = /\.(zip|rar|7z|tar|gz|tgz|bz2|xz|dmg|pkg|iso|exe|msi|apk|deb|rpm|pdf|epub|mobi|doc|docx|xls|xlsx|ppt|pptx|mp4|mkv|avi|mov|webm|mp3|flac|wav|m4a|aac|ogg|jar|bin|img|csv|psd|ai|ttf|otf|woff2?)(\?|#|$)/i;

  let state = { online: false, clickIntercept: false, settings: { intercept: true } };
  const IS_FIREFOX = typeof browser !== 'undefined' && String(browser.runtime.getURL('')).startsWith('moz-extension:');

  function refresh() {
    const apply = (r) => { if (r) state = r; };
    try {
      if (IS_FIREFOX) {
        browser.runtime.sendMessage({ type: 'status' }).then(apply, () => {});
      } else {
        api.runtime.sendMessage({ type: 'status' }, (r) => { if (!api.runtime.lastError) apply(r); });
      }
    } catch {}
  }
  refresh();
  setInterval(refresh, 30000);

  function send(msg) {
    return new Promise((resolve) => {
      try {
        if (IS_FIREFOX) browser.runtime.sendMessage(msg).then(resolve, () => resolve(null));
        else api.runtime.sendMessage(msg, (r) => resolve(api.runtime.lastError ? null : r));
      } catch { resolve(null); }
    });
  }

  // ---------- 1. Click Interception ----------
  let passThrough = null;
  document.addEventListener('click', (e) => {
    if (!e.isTrusted) return;
    // YouTube ve video akış sayfalarında link tıklamalarını kesinlikle engelleme
    if (/youtube\.com|youtu\.be/i.test(location.hostname)) return;

    if (!state.clickIntercept || !state.settings?.intercept) return;
    if (!state.online && !(state.canLaunch && state.settings?.autoLaunch)) return;
    if (e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.altKey || e.shiftKey) return;
    const a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
    if (!a) return;
    if (a === passThrough) { passThrough = null; return; }
    const href = a.href;
    if (!/^https?:/i.test(href)) return;
    const list = state.captureExtensions || [];
    const m = href.split(/[?#]/)[0].match(/\.([a-z0-9]{1,8})$/i);
    const ext = m ? m[1].toLowerCase() : '';
    if (list.length ? !(ext && list.includes(ext)) : !(a.hasAttribute('download') || DL_EXT.test(href))) return;
    e.preventDefault();
    e.stopPropagation();
    const downloadAttr = a.getAttribute('download');
    const filename = downloadAttr || (a.textContent ? a.textContent.trim().slice(0, 100) : '');
    send({ type: 'download', url: href, filename, referer: location.href, kind: 'file' }).then((r) => {
      if (r && r.ok) { if (!state.online) refresh(); return; }
      passThrough = a;
      a.click();
    });
  }, true);

  // ---------- 2. Media Sniffing Scanner ----------
  const reported = new Set();
  function scanMedia() {
    const items = [];
    document.querySelectorAll('video, audio, source').forEach((el) => {
      // Reklam videolarını atla
      if (el.closest && el.closest('.video-ads, .ytp-ad-player-overlay, [class*="ad-container"]')) return;
      const src = el.currentSrc || el.src || el.getAttribute('src');
      if (!src || !/^https?:/i.test(src) || reported.has(src)) return;
      reported.add(src);
      items.push({ url: src, type: el.getAttribute('type') || '', name: document.title ? document.title.slice(0, 80) : '' });
    });
    if (items.length) {
      try { api.runtime.sendMessage({ type: 'pageMedia', items }); } catch {}
    }
  }
  scanMedia();
  document.addEventListener('play', scanMedia, true);
  document.addEventListener('loadedmetadata', scanMedia, true);
  let n = 0;
  const iv = setInterval(() => { scanMedia(); if (++n > 20) clearInterval(iv); }, 3000);

  // ---------- 3. IDM-Style Floating Video Download Button Overlay ----------
  const KNOWN_PLATFORMS = /(youtube\.com|youtu\.be|vimeo\.com|dailymotion\.com|twitter\.com|x\.com|facebook\.com|fb\.watch|instagram\.com|tiktok\.com|twitch\.tv|reddit\.com)/i;

  function getCanonicalVideoUrl(video) {
    const host = location.hostname.toLowerCase();

    // 1) YouTube: Reklam oynuyor olsa bile her zaman asıl videonun URL'sini al!
    if (/youtube\.com|youtu\.be/i.test(host)) {
      const v = new URLSearchParams(location.search).get('v');
      if (v) return 'https://www.youtube.com/watch?v=' + v;
      if (location.pathname.startsWith('/shorts/')) {
        return 'https://www.youtube.com' + location.pathname;
      }
      return location.href;
    }

    // 2) X (Twitter): Ana sayfa feed'inde veya arama sonuçlarındayken tweet permalink'ini bul
    if (/x\.com|twitter\.com/i.test(host)) {
      if (/\/status\/\d+/i.test(location.pathname)) {
        const m = location.pathname.match(/^(\/[^\/]+\/status\/\d+)/i);
        if (m) return 'https://x.com' + m[1];
        return location.href.split('?')[0];
      }
      const tweet = video ? (video.closest('article[data-testid="tweet"]') || video.closest('article')) : null;
      if (tweet) {
        const timeLink = tweet.querySelector('time')?.closest('a[href*="/status/"]');
        const statusLink = timeLink || tweet.querySelector('a[href*="/status/"]');
        if (statusLink && statusLink.getAttribute('href')) {
          const raw = statusLink.getAttribute('href');
          const clean = raw.split('?')[0].replace(/\/photo\/\d+|\/video\/\d+/, '');
          return clean.startsWith('http') ? clean : ('https://x.com' + clean);
        }
      }
      return location.href;
    }

    // 3) Instagram: Feed veya Reels listesindeyken asıl post/reel bağlantısını bul
    if (/instagram\.com/i.test(host)) {
      const pathMatch = location.pathname.match(/\/(reel|p)\/([A-Za-z0-9_-]+)/i);
      if (pathMatch) {
        return 'https://www.instagram.com/' + pathMatch[1] + '/' + pathMatch[2] + '/';
      }
      const container = video ? (video.closest('article') || video.closest('div[role="dialog"]') || video.closest('main') || video.parentElement) : null;
      if (container) {
        const postLink = container.querySelector('a[href*="/reel/"], a[href*="/p/"]');
        if (postLink && postLink.getAttribute('href')) {
          const raw = postLink.getAttribute('href');
          const m = raw.match(/\/(reel|p)\/([A-Za-z0-9_-]+)/i);
          if (m) return 'https://www.instagram.com/' + m[1] + '/' + m[2] + '/';
          return postLink.href.split('?')[0];
        }
      }
      const src = video ? (video.currentSrc || video.src || '') : '';
      if (src && /^https?:/i.test(src) && !src.startsWith('blob:')) {
        return src;
      }
      return location.href;
    }

    // 4) Facebook: Feed'de veya Watch'tayken video permalink'ini bul
    if (/facebook\.com|fb\.watch/i.test(host)) {
      const v = new URLSearchParams(location.search).get('v');
      if (v) return 'https://www.facebook.com/watch/?v=' + v;
      if (/\/reel\/\d+/i.test(location.pathname) || /\/videos\/\d+/i.test(location.pathname)) {
        return 'https://www.facebook.com' + location.pathname.split('?')[0];
      }
      const feedPost = video ? (video.closest('div[role="article"], div[data-pagelet*="FeedUnit"]') || video.closest('div[data-video-id]') || video.parentElement) : null;
      if (feedPost) {
        const link = feedPost.querySelector('a[href*="/videos/"], a[href*="/watch/"], a[href*="/reel/"], a[href*="permalink"], a[href*="/posts/"]');
        if (link && link.href) {
          return link.href.split('&')[0];
        }
      }
      return location.href;
    }

    if (KNOWN_PLATFORMS.test(host)) {
      return location.href;
    }

    const src = video ? (video.currentSrc || video.src || '') : '';
    if (src && /^https?:/i.test(src) && !src.startsWith('blob:')) {
      return src;
    }
    return location.href;
  }

  const OVERLAY_CSS = `
    :host {
      position: absolute !important;
      top: 14px !important;
      right: 14px !important;
      z-index: 2147483647 !important;
      display: inline-block !important;
      width: auto !important;
      height: auto !important;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif !important;
      font-size: 13px !important;
      line-height: 1.2 !important;
      user-select: none !important;
      pointer-events: none !important;
      opacity: 0.95 !important;
      visibility: visible !important;
      transition: opacity 0.2s ease, transform 0.2s ease !important;
    }
    :host(:hover), :host(.hbdm-menu-open) {
      opacity: 1 !important;
      transform: scale(1.02) !important;
    }
    .hbdm-bar {
      display: inline-flex !important;
      align-items: stretch !important;
      background: linear-gradient(135deg, #0078d4 0%, #005a9e 100%) !important;
      color: #ffffff !important;
      border-radius: 6px !important;
      box-shadow: 0 4px 16px rgba(0, 0, 0, 0.65), 0 0 0 1px rgba(255, 255, 255, 0.3) !important;
      cursor: pointer !important;
      backdrop-filter: blur(8px) !important;
      position: relative !important;
      pointer-events: auto !important;
      transition: background 0.2s, box-shadow 0.2s, transform 0.15s !important;
    }
    .hbdm-bar:hover {
      box-shadow: 0 6px 22px rgba(0, 120, 212, 0.75), 0 0 0 1px rgba(255, 255, 255, 0.5) !important;
    }
    .hbdm-btn-main {
      display: flex !important;
      align-items: center !important;
      gap: 6px !important;
      padding: 7px 12px !important;
      font-weight: 600 !important;
      border-top-left-radius: 5px !important;
      border-bottom-left-radius: 5px !important;
      transition: background 0.15s !important;
    }
    .hbdm-btn-main:hover {
      background: rgba(255, 255, 255, 0.18) !important;
    }
    .hbdm-icon {
      width: 16px !important;
      height: 16px !important;
      fill: currentColor !important;
      flex-shrink: 0 !important;
    }
    .hbdm-text {
      white-space: nowrap !important;
      letter-spacing: 0.2px !important;
    }
    .hbdm-toggle {
      display: flex !important;
      align-items: center !important;
      justify-content: center !important;
      padding: 7px 9px !important;
      border-left: 1px solid rgba(255, 255, 255, 0.25) !important;
      border-top-right-radius: 5px !important;
      border-bottom-right-radius: 5px !important;
      transition: background 0.15s !important;
    }
    .hbdm-toggle:hover {
      background: rgba(255, 255, 255, 0.22) !important;
    }
    .hbdm-arrow {
      width: 14px !important;
      height: 14px !important;
      fill: currentColor !important;
    }
    .hbdm-menu {
      display: none;
      position: absolute !important;
      top: calc(100% + 6px) !important;
      right: 0 !important;
      min-width: 290px !important;
      max-height: 380px !important;
      overflow-y: auto !important;
      background: #1e1e1e !important;
      border: 1px solid #3c3c3c !important;
      border-radius: 6px !important;
      box-shadow: 0 8px 28px rgba(0, 0, 0, 0.85) !important;
      padding: 6px !important;
      z-index: 2147483647 !important;
    }
    .hbdm-menu.hbdm-open {
      display: block !important;
      animation: hbdm-anim 0.15s ease !important;
    }
    @keyframes hbdm-anim {
      from { opacity: 0; transform: translateY(-4px); }
      to { opacity: 1; transform: translateY(0); }
    }
    .hbdm-item {
      display: flex !important;
      align-items: center !important;
      justify-content: space-between !important;
      gap: 8px !important;
      padding: 8px 10px !important;
      border-radius: 4px !important;
      font-size: 12px !important;
      font-weight: 500 !important;
      color: #e0e0e0 !important;
      cursor: pointer !important;
      transition: background 0.15s, color 0.15s !important;
    }
    .hbdm-item-left {
      display: flex !important;
      align-items: center !important;
      gap: 8px !important;
    }
    .hbdm-item-badge {
      font-size: 10px !important;
      font-weight: 600 !important;
      padding: 2px 6px !important;
      background: rgba(255, 255, 255, 0.12) !important;
      border-radius: 3px !important;
      color: #79c0ff !important;
    }
    .hbdm-item:hover {
      background: #094771 !important;
      color: #ffffff !important;
    }
    .hbdm-item:hover .hbdm-item-badge {
      background: #1177bb !important;
      color: #ffffff !important;
    }
    .hbdm-divider {
      height: 1px !important;
      background: #333333 !important;
      margin: 4px 0 !important;
    }
    .hbdm-success {
      background: linear-gradient(135deg, #2ebd59 0%, #28a745 100%) !important;
    }
  `;

  let currentYtData = null;
  window.addEventListener('HBDM_YT_STREAM_DATA', (e) => {
    if (e.detail && Array.isArray(e.detail.adaptiveFormats)) {
      currentYtData = e.detail;
      document.querySelectorAll('.hbdm-video-overlay-host').forEach((host) => {
        if (typeof host.updateQualityMenu === 'function') {
          host.updateQualityMenu(currentYtData);
        }
      });
    }
  });

  function formatBytes(bytes) {
    if (!bytes || isNaN(bytes) || bytes <= 0) return '';
    const units = ['B', 'KB', 'MB', 'GB'];
    let i = 0;
    let val = bytes;
    while (val >= 1024 && i < units.length - 1) {
      val /= 1024;
      i++;
    }
    return val.toFixed(i === 0 ? 0 : (val >= 100 ? 0 : 1)) + ' ' + units[i];
  }

  function getQualityMeta(height) {
    const h = parseInt(height, 10);
    if (h >= 4320) return { icon: '👑', label: '8K Ultra HD', badge: '8K UHD' };
    if (h >= 2160) return { icon: '🌟', label: '4K Ultra HD', badge: '4K UHD' };
    if (h >= 1440) return { icon: '🎬', label: '2K Quad HD', badge: '1440p QHD' };
    if (h >= 1080) return { icon: '🎬', label: 'Full HD Kalite', badge: '1080p FHD' };
    if (h >= 720)  return { icon: '📺', label: 'HD Kalite', badge: '720p HD' };
    if (h >= 480)  return { icon: '📱', label: 'Standart Kalite', badge: '480p' };
    if (h >= 360)  return { icon: '📱', label: 'Düşük Kalite', badge: '360p' };
    return { icon: '📱', label: `${h}p Kalite`, badge: `${h}p` };
  }

  function getVideoScore(f) {
    const mime = (f.mimeType || '').toLowerCase();
    const isMp4 = mime.includes('video/mp4');
    let codecRank = 0;
    if (mime.includes('av01')) codecRank = 300;       // AV1 in MP4 (yt-dlp default #1)
    else if (mime.includes('avc1') || mime.includes('h264')) codecRank = 200;  // H.264 in MP4
    else if (mime.includes('vp9') || mime.includes('vp09')) codecRank = 100;   // VP9 in WebM
    else codecRank = 50;

    const containerRank = isMp4 ? 40 : 0;
    const fpsBonus = (f.fps && f.fps >= 60) ? 5 : 0;
    return codecRank + containerRank + fpsBonus;
  }

  function getAudioScore(f) {
    const mime = (f.mimeType || '').toLowerCase();
    const isM4a = mime.includes('audio/mp4') || mime.includes('m4a');
    const bitrate = f.bitrate || 0;
    return (isM4a ? 10000000 : 0) + bitrate;
  }

  function createMenuItem(opts) {
    const item = document.createElement('div');
    item.className = 'hbdm-item';
    if (opts.action) item.setAttribute('data-action', opts.action);
    if (opts.quality) item.setAttribute('data-quality', opts.quality);
    if (opts.estimatedBytes !== undefined && opts.estimatedBytes !== null) {
      item.setAttribute('data-estimated-bytes', String(opts.estimatedBytes));
    }

    const left = document.createElement('div');
    left.className = 'hbdm-item-left';

    const iconSpan = document.createElement('span');
    iconSpan.textContent = opts.icon || '';

    const labelSpan = document.createElement('span');
    labelSpan.textContent = opts.label || '';

    left.appendChild(iconSpan);
    left.appendChild(labelSpan);
    item.appendChild(left);

    if (opts.badge) {
      const badgeSpan = document.createElement('span');
      badgeSpan.className = 'hbdm-item-badge';
      badgeSpan.textContent = opts.badge;
      item.appendChild(badgeSpan);
    }
    return item;
  }

  function createMenuDivider() {
    const div = document.createElement('div');
    div.className = 'hbdm-divider';
    return div;
  }

  function populateQualityMenu(menu, ytData) {
    while (menu.firstChild) menu.removeChild(menu.firstChild);

    if (ytData && Array.isArray(ytData.adaptiveFormats) && ytData.adaptiveFormats.length > 0) {
      const formats = ytData.adaptiveFormats;

      // yt-dlp bestaudio[ext=m4a] matches audio/mp4 (e.g. itag 140)
      let bestAudioBytes = 0;
      let bestAudioFmt = null;
      formats.filter(f => f.mimeType && f.mimeType.startsWith('audio/')).forEach(f => {
        const score = getAudioScore(f);
        if (!bestAudioFmt || score > bestAudioFmt.score) {
          const bytes = parseInt(f.contentLength || (f.bitrate && f.approxDurationMs ? (f.bitrate * f.approxDurationMs / 8000) : 0), 10);
          bestAudioFmt = { format: f, score: score, bytes: bytes };
          bestAudioBytes = bytes;
        }
      });

      // Group video formats by height using exact yt-dlp format priority
      const videoFormatsByHeight = {};
      formats.filter(f => f.height && f.height >= 144).forEach(f => {
        const h = f.height;
        const score = getVideoScore(f);
        const bytes = parseInt(f.contentLength || (f.bitrate && f.approxDurationMs ? (f.bitrate * f.approxDurationMs / 8000) : 0), 10);
        if (!videoFormatsByHeight[h] || score > videoFormatsByHeight[h].score) {
          videoFormatsByHeight[h] = {
            height: h,
            score: score,
            bytes: bytes,
            qualityLabel: f.qualityLabel || `${h}p`,
            fps: f.fps || 30
          };
        }
      });

      const heights = Object.keys(videoFormatsByHeight).map(Number).sort((a, b) => b - a);

      if (heights.length > 0) {
        const maxHeight = heights[0];
        const maxMeta = getQualityMeta(maxHeight);
        const maxTotal = (videoFormatsByHeight[maxHeight].bytes || 0) + bestAudioBytes;
        const maxSizeStr = formatBytes(maxTotal);

        menu.appendChild(createMenuItem({
          action: 'download',
          quality: 'best',
          estimatedBytes: maxTotal,
          icon: '🌟',
          label: `En Yüksek Kalite (${maxMeta.badge})`,
          badge: maxSizeStr ? '~' + maxSizeStr : maxMeta.badge
        }));
        menu.appendChild(createMenuDivider());

        heights.forEach(h => {
          const item = videoFormatsByHeight[h];
          const meta = getQualityMeta(h);
          const totalBytes = (item.bytes || 0) + bestAudioBytes;
          const sizeStr = formatBytes(totalBytes);
          const badgeText = sizeStr ? `~${sizeStr}` : (item.qualityLabel || `${h}p`);

          menu.appendChild(createMenuItem({
            action: 'download',
            quality: String(h),
            estimatedBytes: totalBytes,
            icon: meta.icon,
            label: `${meta.label} (${item.qualityLabel || h + 'p'})`,
            badge: badgeText
          }));
        });
      }

      menu.appendChild(createMenuDivider());
      menu.appendChild(createMenuItem({
        action: 'download-audio',
        quality: 'audio',
        estimatedBytes: bestAudioBytes,
        icon: '🎵',
        label: 'Sadece Sesi İndir',
        badge: bestAudioBytes ? '~' + formatBytes(bestAudioBytes) : 'MP3'
      }));
      menu.appendChild(createMenuItem({
        action: 'copy',
        icon: '📋',
        label: 'Video Bağlantısını Kopyala'
      }));
      return;
    }

    menu.appendChild(createMenuItem({ action: 'download', quality: 'best', icon: '🌟', label: 'En Yüksek Kalite', badge: 'Otomatik' }));
    menu.appendChild(createMenuItem({ action: 'download', quality: '1080', icon: '🎬', label: 'Full HD Kalite', badge: '1080p MP4' }));
    menu.appendChild(createMenuItem({ action: 'download', quality: '720', icon: '📺', label: 'HD Kalite', badge: '720p MP4' }));
    menu.appendChild(createMenuItem({ action: 'download', quality: '480', icon: '📱', label: 'Standart Kalite', badge: '480p MP4' }));
    menu.appendChild(createMenuDivider());
    menu.appendChild(createMenuItem({ action: 'download-audio', quality: 'audio', icon: '🎵', label: 'Sadece Sesi İndir', badge: 'MP3' }));
    menu.appendChild(createMenuItem({ action: 'copy', icon: '📋', label: 'Video Bağlantısını Kopyala' }));
  }

  function createOverlay(video, player) {
    const host = document.createElement('div');
    host.className = 'hbdm-video-overlay-host';
    host.style.cssText = 'position: absolute !important; top: 14px !important; right: 14px !important; z-index: 2147483647 !important; display: inline-block !important; width: auto !important; height: auto !important; pointer-events: none !important; opacity: 0.95 !important; visibility: visible !important;';

    const shadow = host.attachShadow({ mode: 'closed' });

    const style = document.createElement('style');
    style.textContent = OVERLAY_CSS;
    shadow.appendChild(style);

    const bar = document.createElement('div');
    bar.className = 'hbdm-bar';

    // Main download button
    const mainBtn = document.createElement('div');
    mainBtn.className = 'hbdm-btn-main';
    mainBtn.title = 'Bu videoyu HB Download Manager ile indir';
    mainBtn.innerHTML = `
      <svg class="hbdm-icon" viewBox="0 0 24 24">
        <path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM17 13l-5 5-5-5h3V9h4v4h3z"/>
      </svg>
      <span class="hbdm-text">Bu Videoyu İndir</span>
    `;

    // Dropdown arrow toggle
    const toggle = document.createElement('div');
    toggle.className = 'hbdm-toggle';
    toggle.title = 'Kalite Seçenekleri (4K, 1440p, 1080p, 720p, MP3)';
    toggle.innerHTML = `
      <svg class="hbdm-arrow" viewBox="0 0 24 24">
        <path d="M7 10l5 5 5-5z"/>
      </svg>
    `;

    // Dropdown menu with dynamic quality items
    const menu = document.createElement('div');
    menu.className = 'hbdm-menu';

    bar.appendChild(mainBtn);
    bar.appendChild(toggle);
    bar.appendChild(menu);
    shadow.appendChild(bar);

    function setSuccessState(text) {
      const origText = mainBtn.querySelector('.hbdm-text').textContent;
      bar.classList.add('hbdm-success');
      mainBtn.querySelector('.hbdm-text').textContent = text;
      setTimeout(() => {
        bar.classList.remove('hbdm-success');
        mainBtn.querySelector('.hbdm-text').textContent = origText;
      }, 3000);
    }

    function triggerDownload(kind, quality, estimatedBytes) {
      menu.classList.remove('hbdm-open');
      host.classList.remove('hbdm-menu-open');
      const url = getCanonicalVideoUrl(video);
      setSuccessState(kind === 'audio' ? '✓ Ses İndiriliyor...' : '✓ İndirmeye Gönderildi!');
      send({
        type: 'download',
        url: url,
        filename: '', // let app / yt-dlp determine the filename safely
        referer: location.href,
        kind: kind,
        quality: quality || 'best',
        estimatedBytes: estimatedBytes || 0
      });
    }

    function bindMenuEvents() {
      menu.querySelectorAll('.hbdm-item').forEach((item) => {
        item.addEventListener('click', (e) => {
          if (!e.isTrusted) return;
          e.stopPropagation();
          const action = item.getAttribute('data-action');
          const quality = item.getAttribute('data-quality') || 'best';
          const estimatedBytes = parseInt(item.getAttribute('data-estimated-bytes') || '0', 10);
          if (action === 'download') {
            triggerDownload('video', quality, estimatedBytes);
          } else if (action === 'download-audio') {
            triggerDownload('audio', 'audio', estimatedBytes);
          } else if (action === 'copy') {
            const url = getCanonicalVideoUrl(video);
            try {
              navigator.clipboard.writeText(url);
              setSuccessState('✓ Bağlantı Kopyalandı!');
            } catch {}
            menu.classList.remove('hbdm-open');
            host.classList.remove('hbdm-menu-open');
          }
        });
      });
    }

    function renderMenu(data) {
      populateQualityMenu(menu, data || currentYtData);
      bindMenuEvents();
    }

    host.updateQualityMenu = renderMenu;
    renderMenu(currentYtData);

    // Toggle dropdown
    toggle.addEventListener('click', (e) => {
      if (!e.isTrusted) return;
      e.stopPropagation();
      window.dispatchEvent(new CustomEvent('HBDM_REQUEST_YT_DATA'));
      const open = menu.classList.toggle('hbdm-open');
      host.classList.toggle('hbdm-menu-open', open);
    });

    document.addEventListener('click', (e) => {
      if (e && !e.isTrusted) return;
      menu.classList.remove('hbdm-open');
      host.classList.remove('hbdm-menu-open');
    });

    // Prevent underlying video player from intercepting mouse clicks / pausing / navigating
    ['mousedown', 'mouseup', 'pointerdown', 'pointerup', 'dblclick', 'contextmenu'].forEach((ev) => {
      bar.addEventListener(ev, (e) => {
        e.stopPropagation();
      });
    });

    // Main button click -> Best video
    mainBtn.addEventListener('click', (e) => {
      if (!e.isTrusted) return;
      e.stopPropagation();
      const bestItem = menu.querySelector('.hbdm-item[data-quality="best"]');
      const est = bestItem ? parseInt(bestItem.getAttribute('data-estimated-bytes') || '0', 10) : 0;
      triggerDownload('video', 'best', est);
    });

    return host;
  }

  function isMainVideo(video) {
    if (!video) return false;

    // Reklam videolarını kesinlikle atla (YouTube reklamları ve belirgin reklam kapsayıcıları)
    if (video.closest && video.closest('.video-ads, .ytp-ad-player-overlay, .ad-container, #ad-container, [data-ad-container]')) {
      return false;
    }

    const host = location.hostname.toLowerCase();

    // YouTube: Sadece ana izleme ekranı (#movie_player) içindeki videoyu al!
    if (/youtube\.com|youtu\.be/i.test(host)) {
      if (!location.pathname.startsWith('/watch') && !location.pathname.startsWith('/shorts')) {
        return false;
      }
      if (video.closest && video.closest('ytd-thumbnail, ytd-video-preview, .ytd-moving-thumbnail-renderer, ytd-compact-video-renderer, ytd-rich-grid-media, [class*="thumbnail"], [class*="preview"], [id*="preview"]')) {
        return false;
      }
      const mainPlayer = video.closest('#movie_player, .html5-video-player');
      if (!mainPlayer) return false;
      return true;
    }

    // X (Twitter): Tweet ve video player içindeki tüm videolar geçerlidir
    if (/x\.com|twitter\.com/i.test(host)) {
      return true;
    }

    // Instagram: Reels veya Post videoları
    if (/instagram\.com/i.test(host)) {
      return true;
    }

    // Facebook: Feed veya Watch videoları
    if (/facebook\.com|fb\.watch/i.test(host)) {
      return true;
    }

    // Diğer siteler: Boyut kontrolü
    const rect = video.getBoundingClientRect();
    const w = rect.width || video.videoWidth || 0;
    const h = rect.height || video.videoHeight || 0;

    if (w === 0 && h === 0 && !video.duration) {
      return false;
    }

    const isVertical = (w >= 130 && h >= 220);
    const isHorizontal = (w >= 240 && h >= 130);
    const isLargeArea = (w * h >= 32000);

    return isVertical || isHorizontal || isLargeArea;
  }

  function attachToVideo(video) {
    if (!isMainVideo(video)) return;

    const host = location.hostname.toLowerCase();
    let player = null;

    if (/youtube\.com|youtu\.be/i.test(host)) {
      player = document.getElementById('movie_player') ||
               document.querySelector('.html5-video-player');
    } else if (/x\.com|twitter\.com/i.test(host)) {
      player = video.closest('[data-testid="videoComponent"]') ||
               video.closest('[data-testid="videoPlayer"]') ||
               video.parentElement;
    } else if (/instagram\.com/i.test(host)) {
      player = video.closest('div[style*="aspect-ratio"], article, div[role="presentation"]') ||
               video.parentElement;
    } else if (/facebook\.com|fb\.watch/i.test(host)) {
      player = video.closest('div[data-video-id], div[role="presentation"], div[role="region"]') ||
               video.parentElement;
    } else {
      player = video.closest('.video-js, .plyr, .jwplayer, [data-player], .player') ||
               video.parentElement;
    }

    if (!player) return;

    // Zaten eklenmişse tekrar ekleme
    if (player.querySelector('.hbdm-video-overlay-host')) {
      return;
    }

    const pos = window.getComputedStyle(player).position;
    if (pos === 'static') {
      player.style.position = 'relative';
    }

    const overlay = createOverlay(video, player);
    player.appendChild(overlay);
  }

  // X (Twitter) tweetlerinin altındaki işlem butonlarına (Like, Retweet yanı) indirme butonu ekler
  function scanXTweetActions() {
    const tweets = document.querySelectorAll('article[data-testid="tweet"]');
    tweets.forEach((tweet) => {
      const hasVideo = tweet.querySelector('video, [data-testid="videoComponent"], [data-testid="videoPlayer"]');
      if (!hasVideo) return;

      const group = tweet.querySelector('div[role="group"]');
      if (!group || group.querySelector('.hbdm-x-action-btn')) return;

      const timeLink = tweet.querySelector('time')?.closest('a[href*="/status/"]');
      const statusLink = timeLink || tweet.querySelector('a[href*="/status/"]');
      let tweetUrl = location.href;
      if (statusLink && statusLink.getAttribute('href')) {
        const raw = statusLink.getAttribute('href');
        const clean = raw.split('?')[0].replace(/\/photo\/\d+|\/video\/\d+/, '');
        tweetUrl = clean.startsWith('http') ? clean : ('https://x.com' + clean);
      }

      const btn = document.createElement('div');
      btn.className = 'hbdm-x-action-btn';
      btn.title = 'Videoyu HB Download Manager ile İndir';
      btn.style.cssText = 'display: inline-flex; align-items: center; margin-left: 8px; cursor: pointer; color: #1d9bf0; font-size: 13px; font-weight: 600; padding: 4px 10px; border-radius: 9999px; background: rgba(29, 155, 240, 0.12); transition: background 0.2s, transform 0.15s; user-select: none; z-index: 10;';
      btn.innerHTML = `
        <svg viewBox="0 0 24 24" style="width: 16px; height: 16px; fill: currentColor; margin-right: 5px;">
          <path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM17 13l-5 5-5-5h3V9h4v4h3z"/>
        </svg>
        <span>İndir</span>
      `;

      btn.addEventListener('mouseenter', () => {
        btn.style.background = 'rgba(29, 155, 240, 0.22)';
        btn.style.transform = 'scale(1.05)';
      });
      btn.addEventListener('mouseleave', () => {
        btn.style.background = 'rgba(29, 155, 240, 0.12)';
        btn.style.transform = 'none';
      });

      ['mousedown', 'mouseup', 'click'].forEach((ev) => {
        btn.addEventListener(ev, (e) => {
          e.stopPropagation();
          e.preventDefault();
        });
      });

      btn.addEventListener('click', (e) => {
        if (!e.isTrusted) return;
        e.stopPropagation();
        e.preventDefault();
        btn.innerHTML = `
          <svg viewBox="0 0 24 24" style="width: 16px; height: 16px; fill: #00ba7c; margin-right: 5px;">
            <path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/>
          </svg>
          <span style="color: #00ba7c;">İndiriliyor...</span>
        `;
        btn.style.background = 'rgba(0, 186, 124, 0.18)';
        send({
          type: 'download',
          url: tweetUrl,
          filename: '',
          referer: location.href,
          kind: 'video',
          quality: 'best',
          estimatedBytes: 0
        });
        setTimeout(() => {
          btn.innerHTML = `
            <svg viewBox="0 0 24 24" style="width: 16px; height: 16px; fill: currentColor; margin-right: 5px;">
              <path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM17 13l-5 5-5-5h3V9h4v4h3z"/>
            </svg>
            <span>İndir</span>
          `;
          btn.style.background = 'rgba(29, 155, 240, 0.12)';
        }, 3000);
      });

      group.appendChild(btn);
    });
  }

  function scanVideos() {
    if (/youtube\.com|youtu\.be/i.test(location.hostname)) {
      if (!location.pathname.startsWith('/watch') && !location.pathname.startsWith('/shorts')) {
        const old = document.querySelectorAll('.hbdm-video-overlay-host');
        old.forEach((o) => o.remove());
        return;
      }
      const ytPlayer = document.getElementById('movie_player') || document.querySelector('.html5-video-player');
      if (ytPlayer) {
        const vid = ytPlayer.querySelector('video');
        if (vid && isMainVideo(vid)) {
          attachToVideo(vid);
        }
      }
      return;
    }

    const videos = document.querySelectorAll('video');
    videos.forEach((v) => {
      if (isMainVideo(v)) attachToVideo(v);
    });

    if (/x\.com|twitter\.com/i.test(location.hostname)) {
      scanXTweetActions();
    }
  }

  function onYtNavigate() {
    currentYtData = null;
    const old = document.querySelectorAll('.hbdm-video-overlay-host');
    old.forEach((o) => o.remove());
    setTimeout(scanVideos, 600);
  }

  window.addEventListener('yt-navigate-finish', onYtNavigate);
  window.addEventListener('popstate', onYtNavigate);
  window.addEventListener('load', scanVideos);

  let scanDebounce = null;
  const observer = new MutationObserver(() => {
    if (scanDebounce) return;
    scanDebounce = setTimeout(() => {
      scanDebounce = null;
      scanVideos();
    }, 400);
  });
  observer.observe(document.body || document.documentElement, { childList: true, subtree: true });
  setInterval(scanVideos, 2000);
  scanVideos();
})();
