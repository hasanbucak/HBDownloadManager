// HB Download Manager bridge extension — background script (Chrome MV3 / Safari / Firefox)
'use strict';

const api = typeof browser !== 'undefined' ? browser : chrome;
const t = (k, subs) => { try { return api.i18n.getMessage(k, subs) || k; } catch { return k; } };
// Firefox (and its forks) serve extension pages from moz-extension:// URLs; works in content scripts too.
const IS_FIREFOX = typeof browser !== 'undefined' && String(browser.runtime.getURL('')).startsWith('moz-extension:');
const IS_SAFARI = !IS_FIREFOX && typeof browser !== 'undefined' && !!browser.runtime.sendNativeMessage && !chrome.downloads?.onDeterminingFilename;

const DEFAULTS = { port: 48213, intercept: true, minSizeMB: 0, sniff: true, autoLaunch: true };
const NATIVE_HOST = 'com.hasanbucak.hbdownloadmanager';
// Native messaging host bundled with the app (Chrome/Brave/Edge/Firefox); Safari talks to its app extension.
const BROWSER = IS_FIREFOX ? 'firefox' : IS_SAFARI ? 'safari' : (navigator.brave ? 'brave' : (/OPR\/|Opera/i.test(navigator.userAgent) ? 'opera' : (/Edg\//.test(navigator.userAgent) ? 'edge' : 'chrome')));

const MEDIA_EXT = /\.(mp4|m4v|webm|mkv|mov|avi|flv|mp3|m4a|aac|ogg|opus|wav|flac)(\?|#|$)/i;
const FRAGMENT = /\.(ts|m4s|m4f|cmfv|cmfa|fmp4|init)(\?|#|$)|[/_.-](seg|segment|chunk|frag|fragment)[-_]?\d+/i;
const IGNORED_HOSTS = /(^|\.)googlevideo\.com$|(^|\.)fbcdn\.net$|(^|\.)twimg\.com$/i;

// ---------- Settings ----------
async function getSettings() {
  const s = await api.storage.local.get('settings');
  return { ...DEFAULTS, ...(s.settings || {}) };
}
async function setSettings(patch) {
  const cur = await getSettings();
  await api.storage.local.set({ settings: { ...cur, ...patch } });
}
const base = (s) => `http://127.0.0.1:${s.port}`;

// ---------- Auth token (native host handshake) ----------
// The token is only ever used as an HMAC key: it is never sent to the bridge. A process that
// squats the port while the app is down therefore learns nothing it could reuse.
let cachedToken = null;
const HAS_SESSION_STORAGE = !!api.storage?.session;

if (api.storage?.session?.setAccessLevel) {
  try {
    api.storage.session.setAccessLevel({ accessLevel: 'TRUSTED_CONTEXTS' }).catch(() => {});
  } catch {}
}

async function requestTokenFromNativeHost() {
  if (!api.runtime.sendNativeMessage) return null;
  try {
    const r = await api.runtime.sendNativeMessage(IS_SAFARI ? 'application.id' : NATIVE_HOST, { action: 'getToken' });
    if (r && r.ok && typeof r.token === 'string' && /^[0-9a-f]{64}$/.test(r.token)) {
      cachedToken = r.token;
      if (HAS_SESSION_STORAGE) {
        await api.storage.session.set({ token: r.token });
      } else {
        await api.storage.local.set({ token: r.token });
      }
      return r.token;
    }
  } catch {}
  return null;
}

async function getAuthToken() {
  if (cachedToken) return cachedToken;
  try {
    const area = HAS_SESSION_STORAGE ? api.storage.session : api.storage.local;
    const s = await area.get('token');
    if (s?.token) {
      cachedToken = s.token;
      return cachedToken;
    }
  } catch {}
  return await requestTokenFromNativeHost();
}

const enc = new TextEncoder();
const toHex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
function randomHex(bytes) {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return toHex(a);
}
async function hmacHex(keyStr, msg) {
  const key = await crypto.subtle.importKey('raw', enc.encode(keyStr), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return toHex(await crypto.subtle.sign('HMAC', key, enc.encode(msg)));
}
async function sha256Hex(str) {
  return toHex(await crypto.subtle.digest('SHA-256', enc.encode(str)));
}

// Signs one request; the server accepts each nonce once and only within 30 s of ts
async function signedHeaders(token, method, path, body, extraHeaders = {}) {
  const ts = String(Math.floor(Date.now() / 1000));
  const nonce = randomHex(16);
  const bodyHash = await sha256Hex(body || '');
  const sig = await hmacHex(token, `hbdm-req:v1\n${method}\n${path}\n${ts}\n${nonce}\n${bodyHash}`);
  return {
    'X-HBDM-Extension': BROWSER,
    'X-HBDM-Extension-Id': api.runtime.id,
    'X-HBDM-Ts': ts,
    'X-HBDM-Nonce': nonce,
    'X-HBDM-Sig': sig,
    ...extraHeaders,
  };
}

// fetch + JSON with one timeout that also covers reading the body
async function fetchJSON(url, init = {}, ms = 5000) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), ms);
  try {
    const r = await fetch(url, { cache: 'no-store', ...init, signal: ctrl.signal });
    const body = await r.json().catch(() => null);
    return { status: r.status, ok: r.ok, body };
  } finally {
    clearTimeout(timer);
  }
}

// ---------- App communication ----------
// lastPingStatus: 'ok' | 'offline' (nothing answered) | 'unverified' (no token to check with) |
// 'mismatch' (something answered but could not prove it knows our token)
let lastPingStatus = 'offline';
let lastVerified = { port: 0, at: 0 };
let mismatchNotified = false;

async function ping(settings) {
  settings = settings || (await getSettings());
  const nonce = randomHex(16);
  let app = null;
  try {
    const res = await fetchJSON(
      base(settings) + '/ping?nonce=' + nonce + '&browser=' + BROWSER,
      { headers: { 'X-HBDM-Extension': BROWSER, 'X-HBDM-Extension-Id': api.runtime.id } },
      1500,
    );
    app = res.ok ? res.body : null;
  } catch {
    app = null;
  }
  if (!app || !app.ok) {
    lastPingStatus = 'offline';
    return null;
  }

  const proofMsg = `hbdm-ping:v1:${settings.port}:${nonce}`;
  let token = await getAuthToken();
  if (!token) {
    // Fail closed: without a token we cannot tell the app from an impostor
    lastPingStatus = 'unverified';
    return null;
  }
  if (app.proof !== (await hmacHex(token, proofMsg))) {
    // The token may have been recreated since we cached it: fetch it once more
    const fresh = await requestTokenFromNativeHost();
    if (!fresh || app.proof !== (await hmacHex(fresh, proofMsg))) {
      lastPingStatus = 'mismatch';
      if (!mismatchNotified) {
        mismatchNotified = true;
        notify(t('notifOfflineTitle'), 'Port ' + settings.port + ': HB Download Manager could not be verified.');
      }
      return null;
    }
    token = fresh;
  }

  mismatchNotified = false;
  lastPingStatus = 'ok';
  lastVerified = { port: settings.port, at: Date.now() };
  app.genuine = true;
  if (app.active > 0) startProgress();
  if (Array.isArray(app.captureExtensions)) api.storage.local.set({ captureExtensions: app.captureExtensions });
  return app;
}

async function ensureVerified(settings) {
  if (lastPingStatus === 'ok' && lastVerified.port === settings.port && Date.now() - lastVerified.at < 10000) {
    return true;
  }
  return !!(await ping(settings));
}

// Signed request to the bridge; one retry with a refreshed token after a 403
async function bridgeRequest(settings, method, path, payload) {
  const body = payload === undefined ? '' : JSON.stringify(payload);
  const send = async (token) => {
    const headers = await signedHeaders(token, method, path, body, body ? { 'Content-Type': 'application/json' } : {});
    return fetchJSON(base(settings) + path, method === 'GET' ? { method, headers } : { method, headers, body }, 8000);
  };
  let token = await getAuthToken();
  if (!token) throw new Error('no bridge token');
  let res = await send(token);
  if (res.status === 403) {
    token = await requestTokenFromNativeHost();
    if (!token) throw new Error('forbidden');
    res = await send(token);
  }
  return res;
}

// ---------- Download progress: toolbar badge + popup list ----------
async function bridgeJSON(path, method = 'GET', payload) {
  const settings = await getSettings();
  if (!(await ensureVerified(settings))) throw new Error('app offline or unverified');
  const res = await bridgeRequest(settings, method, path, payload);
  return res.body;
}
async function getDownloads() {
  try { const j = await bridgeJSON('/downloads'); return (j && j.items) || []; } catch { return []; }
}
async function downloadAction(id, action) {
  try {
    const j = await bridgeJSON('/action', 'POST', { id, action });
    tickProgress();
    return j || { ok: false, error: 'no answer' };
  } catch (e) { return { ok: false, error: String(e) }; }
}
const ACTIVE_STATES = new Set(['downloading', 'probing']);
let progressTimer = null;
let badgeShown = false;
async function setGlobalBadge(text, color) {
  try { await api.action.setBadgeText({ text }); if (color) await api.action.setBadgeBackgroundColor({ color }); } catch {}
}
// Per-tab media counts override the global badge; while downloads run, every tab falls back to the global text.
async function useGlobalBadgeOnAllTabs(on) {
  try {
    const tabs = await api.tabs.query({});
    for (const tab of tabs) {
      if (on) { try { await api.action.setBadgeText({ tabId: tab.id, text: null }); } catch {} }
      else { const list = await readMedia(tab.id); try { await api.action.setBadgeText({ tabId: tab.id, text: list.length ? String(list.length) : '' }); } catch {} }
    }
  } catch {}
}
async function tickProgress() {
  const list = await getDownloads();
  const active = list.filter((i) => ACTIVE_STATES.has(i.state));
  if (active.length) {
    const measurable = active.filter((i) => i.total > 0);
    let text = '…';
    if (active.length > 1) text = active.length + '↓';
    else if (measurable.length) {
      const total = measurable.reduce((a, i) => a + i.total, 0);
      const got = measurable.reduce((a, i) => a + Math.min(i.received, i.total), 0);
      text = Math.round((100 * got) / total) + '%';
    }
    if (!badgeShown) { badgeShown = true; await useGlobalBadgeOnAllTabs(true); }
    await setGlobalBadge(text, '#2f6feb');
  } else {
    stopProgress();
    if (badgeShown) {
      badgeShown = false;
      await setGlobalBadge('✓', '#2ebd59');
      setTimeout(async () => { await setGlobalBadge('', '#2f6feb'); await useGlobalBadgeOnAllTabs(false); }, 4000);
    }
  }
}
function startProgress() {
  if (progressTimer) return;
  tickProgress();
  progressTimer = setInterval(tickProgress, 1500);  // each fetch keeps the MV3 worker alive while something runs
}
function stopProgress() {
  if (progressTimer) { clearInterval(progressTimer); progressTimer = null; }
}

async function cookiesFor(url) {
  try {
    const list = await api.cookies.getAll({ url });
    return {
      header: list.map((c) => `${c.name}=${c.value}`).join('; '),
      list: list.map((c) => ({
        name: c.name, value: c.value, domain: c.domain, path: c.path,
        secure: !!c.secure, expirationDate: c.expirationDate || 0,
      })),
    };
  } catch {
    return { header: '', list: [] };
  }
}

function notify(title, message) {
  try {
    api.notifications?.create?.('', {
      type: 'basic', iconUrl: 'icons/icon128.png', title, message: String(message || '').slice(0, 200),
    });
  } catch {}
}

async function sendToApp({ url, filename, referer, kind, quality, estimatedBytes }) {
  const settings = await getSettings();
  let app = await ping(settings);
  if (!app && settings.autoLaunch && lastPingStatus === 'offline') {
    app = await ensureApp(settings);
  }
  if (!app) {
    // Cookies are only ever sent to a bridge that proved it knows our token
    return { ok: false, error: 'app ' + lastPingStatus };
  }

  const ck = await cookiesFor(url);
  const payload = {
    url, filename: filename || '', referer: referer || '', kind: kind || 'file',
    estimatedBytes: estimatedBytes || 0,
    userAgent: navigator.userAgent, cookieHeader: ck.header, cookies: ck.list,
  };
  if (kind === 'video' || kind === 'audio') {
    payload.quality = quality || 'best';
  }
  try {
    const res = await bridgeRequest(settings, 'POST', '/download', payload);
    const j = res.body;
    if (!j || !j.ok) throw new Error((j && j.error) || 'app error');
    notify(kind === 'video' || kind === 'audio' ? t('notifVideoTitle') : t('notifSentTitle'), filename || url);
    startProgress();
    return { ok: true };
  } catch (e) {
    notify(t('notifOfflineTitle'), t('notifOfflineBody'));
    return { ok: false, error: String(e) };
  }
}

// Launch the app through native messaging: Safari's app extension, or the host bundled with the app
// (registered by the app in each browser's NativeMessagingHosts folder).
async function launchApp() {
  if (!api.runtime.sendNativeMessage) return false;
  try {
    const r = await api.runtime.sendNativeMessage(IS_SAFARI ? 'application.id' : NATIVE_HOST, { action: 'launchApp' });
    return !!(r && r.ok);
  } catch {
    return false;
  }
}

// Returns the app's verified ping answer, starting the app first when NOTHING answers on the port
// (waits up to ~12 s). A port that answers but cannot be verified is never "fixed" by launching.
let launching = null;
async function ensureApp(settings) {
  const app = await ping(settings);
  if (app) return app;
  if (lastPingStatus !== 'offline') return null;
  if (!launching) {
    launching = (async () => {
      if (!(await launchApp())) return null;
      const until = Date.now() + 12000;
      while (Date.now() < until) {
        await new Promise((r) => setTimeout(r, 500));
        const a = await ping(settings);
        if (a) return a;
        if (lastPingStatus === 'mismatch' || lastPingStatus === 'unverified') return null;
      }
      return null;
    })();
    launching.finally(() => { launching = null; });
  }
  return launching;
}

// ---------- Capture file-type filter (list comes from the app) ----------
function extOf(name) {
  const m = String(name || '').split(/[?#]/)[0].match(/\.([a-z0-9]{1,8})$/i);
  return m ? m[1].toLowerCase() : '';
}
function matchesCaptureTypes(app, filename, url) {
  const list = app && Array.isArray(app.captureExtensions) ? app.captureExtensions : [];
  if (!list.length) return true;
  let path = '';
  try { path = new URL(url).pathname; } catch {}
  const e = extOf(filename) || extOf(path);
  return !!e && list.includes(e);
}

// ---------- Download interception (Chrome / Firefox) ----------
async function interceptDownload(item) {
  const settings = await getSettings();
  if (!settings.intercept) return false;
  const url = item.finalUrl || item.url;
  if (!/^https?:/i.test(url)) return false;
  if (item.byExtensionId) return false;
  if (settings.minSizeMB > 0 && item.totalBytes > 0 && item.totalBytes < settings.minSizeMB * 1024 * 1024) return false;
  let app = await ping(settings);
  if (!app && settings.autoLaunch && lastPingStatus === 'offline') {
    // Only start the app for file types it would take (list remembered from its last answer).
    const { captureExtensions } = await api.storage.local.get('captureExtensions');
    if (matchesCaptureTypes({ captureExtensions }, item.filename || '', url)) app = await ensureApp(settings);
  }
  if (!app) return false;
  if (!matchesCaptureTypes(app, item.filename || '', url)) return false;

  const filename = item.filename ? item.filename.split(/[\\/]/).pop() : '';
  const kind = 'file';
  const res = await sendToApp({
    url,
    filename,
    referer: item.referrer,
    kind,
    estimatedBytes: item.totalBytes > 0 ? item.totalBytes : 0,
  });

  // Only cancel the browser download AFTER the app responds with {ok: true}.
  // If the app returns ok: false or is offline, let the browser download proceed!
  if (!res || !res.ok) {
    return false;
  }

  try { await api.downloads.cancel(item.id); } catch {}
  if (IS_FIREFOX) {
    // A small file can finish before the app has answered; delete the browser's duplicate copy.
    try {
      const [d] = await api.downloads.search({ id: item.id });
      if (d && d.state === 'complete' && d.exists !== false) await api.downloads.removeFile(item.id);
    } catch {}
  }
  try { await api.downloads.erase({ id: item.id }); } catch {}
  return true;
}

if (api.downloads?.onDeterminingFilename) {
  api.downloads.onDeterminingFilename.addListener((item, suggest) => {
    interceptDownload(item)
      .then((took) => { if (!took) { try { suggest(); } catch {} } })
      .catch(() => { try { suggest(); } catch {} });
    return true;
  });
} else if (api.downloads?.onCreated) {
  // Firefox has no onDeterminingFilename: the browser download is already running when we hear about it.
  // Pause it at once so it does not keep downloading while the app is asked; resume it if the app declines.
  api.downloads.onCreated.addListener((item) => {
    const paused = item.state === 'in_progress' ? Promise.resolve(api.downloads.pause(item.id)).then(() => true, () => false) : Promise.resolve(false);
    interceptDownload(item).then(async (took) => {
      if (!took && (await paused)) { try { await api.downloads.resume(item.id); } catch {} }
    });
  });
}

// ---------- Media sniffing ----------
const mem = new Map(); // tabId -> Map(url -> info)   (fallback when storage.session is unavailable)

async function readMedia(tabId) {
  if (api.storage.session) {
    const key = 'media:' + tabId;
    const r = await api.storage.session.get(key);
    return r[key] || [];
  }
  return Array.from((mem.get(tabId) || new Map()).values());
}
async function writeMedia(tabId, list) {
  if (api.storage.session) {
    await api.storage.session.set({ ['media:' + tabId]: list });
  } else {
    mem.set(tabId, new Map(list.map((m) => [m.url, m])));
  }
  try {
    await api.action.setBadgeText({ tabId, text: list.length ? String(list.length) : '' });
    await api.action.setBadgeBackgroundColor({ tabId, color: '#2f6feb' });
  } catch {}
}
async function clearMedia(tabId) {
  if (api.storage.session) await api.storage.session.remove('media:' + tabId);
  mem.delete(tabId);
  try { await api.action.setBadgeText({ tabId, text: '' }); } catch {}
}

function normalizeKey(url) {
  try {
    const u = new URL(url);
    ['range', 'bytes', 'start', 'end', 'rn', 'rbuf', 'r', '_'].forEach((p) => u.searchParams.delete(p));
    return u.origin + u.pathname + (u.searchParams.toString() ? '?' + u.searchParams.toString() : '');
  } catch {
    return url;
  }
}

async function addMedia(tabId, info) {
  if (tabId < 0) return;
  const settings = await getSettings();
  if (!settings.sniff) return;
  const list = await readMedia(tabId);
  const key = normalizeKey(info.url);
  const idx = list.findIndex((m) => m.key === key);
  const entry = { ...info, key, name: withExtension(info.name, info.url, info.type) };
  if (idx >= 0) {
    if ((info.size || 0) > (list[idx].size || 0)) list[idx] = { ...list[idx], ...entry };
    return;
  }
  list.unshift(entry);
  if (list.length > 40) list.length = 40;
  await writeMedia(tabId, list);
}

function guessName(url, type) {
  try {
    const u = new URL(url);
    let n = decodeURIComponent(u.pathname.split('/').filter(Boolean).pop() || '') || u.hostname;
    if (!/\.[a-z0-9]{2,5}$/i.test(n) && type) {
      const ext = { 'video/mp4': 'mp4', 'video/webm': 'webm', 'audio/mpeg': 'mp3', 'audio/mp4': 'm4a', 'video/quicktime': 'mov' }[type.toLowerCase()];
      if (ext) n += '.' + ext;
    }
    return n;
  } catch {
    return url;
  }
}

// Page-provided names (e.g. document.title) carry no extension: take it from the URL or the MIME type
function withExtension(name, url, type) {
  const fallback = guessName(url, type);
  if (!name) return fallback;
  const clean = String(name).replace(/[\/:*?"<>|\x00-\x1f]+/g, ' ').trim().slice(0, 120);
  if (!clean) return fallback;
  if (/\.[a-z0-9]{2,5}$/i.test(clean)) return clean;
  const m = fallback.match(/\.([a-z0-9]{2,5})$/i);
  return m ? clean + '.' + m[1] : clean;
}

function headerVal(headers, name) {
  const h = (headers || []).find((x) => x.name.toLowerCase() === name);
  return h ? h.value : '';
}
function classify(url, ct) {
  ct = (ct || '').toLowerCase();
  if (/\.m3u8(\?|#|$)/i.test(url) || /mpegurl/.test(ct)) return 'hls';
  if (/\.mpd(\?|#|$)/i.test(url) || /dash\+xml/.test(ct)) return 'dash';
  if (FRAGMENT.test(url)) return null;
  if (/^(video|audio)\//.test(ct)) {
    if (/mp2t|iso\.segment|x-mpegurl/.test(ct)) return null;
    return 'media';
  }
  if (/octet-stream/.test(ct) && MEDIA_EXT.test(url)) return 'media';
  return null;
}

if (api.webRequest?.onHeadersReceived) {
  api.webRequest.onHeadersReceived.addListener(
    (d) => {
      if (d.tabId < 0 || d.statusCode >= 400) return;
      let host = '';
      try { host = new URL(d.url).hostname; } catch {}
      if (IGNORED_HOSTS.test(host)) return;
      const ct = headerVal(d.responseHeaders, 'content-type');
      const kind = classify(d.url, ct);
      if (!kind) return;
      let size = parseInt(headerVal(d.responseHeaders, 'content-length'), 10) || 0;
      const cr = headerVal(d.responseHeaders, 'content-range').match(/\/(\d+)/);
      if (cr) size = parseInt(cr[1], 10);
      if (kind === 'media' && size > 0 && size < 300 * 1024) return;
      addMedia(d.tabId, { url: d.url, type: ct.split(';')[0], size, kind, time: Date.now() });
    },
    { urls: ['<all_urls>'], types: ['media', 'xmlhttprequest', 'other', 'object', 'main_frame'] },
    ['responseHeaders']
  );
}

api.tabs.onUpdated.addListener((tabId, info) => {
  if (info.status === 'loading' && info.url) clearMedia(tabId);
});
api.tabs.onRemoved.addListener((tabId) => clearMedia(tabId));

// ---------- Context menu ----------
const menus = api.contextMenus || api.menus;
function createMenus() {
  if (!menus) return;
  let created = false;
  const create = () => {
    if (created) return;
    created = true;
    try {
      menus.create({ id: 'dm-link', title: t('menuDownloadLink'), contexts: ['link', 'video', 'audio', 'image'] });
      menus.create({ id: 'dm-page-video', title: t('menuPageVideo'), contexts: ['page', 'video', 'frame', 'action'] });
    } catch {}
  };
  // Chrome/Safari take a callback; Firefox's browser.menus returns a promise.
  try {
    const r = IS_FIREFOX ? menus.removeAll() : menus.removeAll(create);
    if (r && typeof r.then === 'function') r.then(create, create);
  } catch { create(); }
}
async function initServiceWorker() {
  createMenus();
  if (HAS_SESSION_STORAGE) {
    // Older versions kept the token in storage.local, which content scripts can read
    try { await api.storage.local.remove(['bridgeToken', 'token']); } catch {}
  }
  const token = await getAuthToken();
  if (!token) {
    await requestTokenFromNativeHost();
  }
  await ping();
}

api.runtime.onInstalled.addListener(() => { initServiceWorker(); });
api.runtime.onStartup?.addListener(() => { initServiceWorker(); });
initServiceWorker();
menus?.onClicked.addListener((info, tab) => {
  if (info.menuItemId === 'dm-link') {
    const url = info.linkUrl || info.srcUrl;
    if (url) sendToApp({ url, referer: info.pageUrl || tab?.url, kind: 'file' });
  } else if (info.menuItemId === 'dm-page-video') {
    const url = info.pageUrl || tab?.url;
    if (url) sendToApp({ url, referer: url, kind: 'video' });
  }
});

// ---------- Popup / content script messages ----------
api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  (async () => {
    try {
      if (sender.id && sender.id !== api.runtime.id) return;
      switch (msg?.type) {
        case 'status': {
          const settings = await getSettings();
          const app = await ping(settings);
          const cached = app ? null : (await api.storage.local.get('captureExtensions')).captureExtensions;
          sendResponse({
            online: !!app, authState: lastPingStatus, app, settings, clickIntercept: !api.downloads?.onDeterminingFilename, isSafari: IS_SAFARI,
            canLaunch: !!api.runtime.sendNativeMessage, captureExtensions: (app && app.captureExtensions) || cached || [],
          });
          break;
        }
        case 'getMedia': sendResponse(await readMedia(msg.tabId)); break;
        case 'downloads': {
          if (sender.tab) throw new Error('Unauthorized');
          sendResponse(await getDownloads());
          break;
        }
        case 'action': {
          if (sender.tab) throw new Error('Unauthorized');
          sendResponse(await downloadAction(msg.id, msg.action));
          break;
        }
        case 'download': sendResponse(await sendToApp(msg)); break;
        case 'setSettings': {
          if (sender.tab) throw new Error('Unauthorized');
          await setSettings(msg.settings || {});
          sendResponse({ ok: true });
          break;
        }
        case 'launchApp': {
          if (sender.tab) throw new Error('Unauthorized');
          sendResponse({ ok: await launchApp() });
          break;
        }
        case 'pageMedia': {
          if (sender.tab) {
            for (const m of msg.items || []) {
              const kind = classify(m.url, m.type || '');
              if (kind || MEDIA_EXT.test(m.url)) {
                await addMedia(sender.tab.id, { url: m.url, type: m.type || '', size: 0, kind: kind || 'media', time: Date.now(), name: m.name });
              }
            }
          }
          sendResponse({ ok: true });
          break;
        }
        default: sendResponse(null);
      }
    } catch (e) {
      sendResponse({ ok: false, error: String(e) });
    }
  })();
  return true;
});
