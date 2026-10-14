#include "LocalServer.h"

#ifndef HBDM_VERSION
#define HBDM_VERSION "0.0.0-dev"
#endif
#include "PathUtils.h"
#include "MediaHosts.h"
#include "../core/DownloadManager.h"
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QMessageAuthenticationCode>
#include <QUrlQuery>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <sys/stat.h>
#include "BridgeToken.h"

LocalServer::LocalServer(quint16 port, QObject *parent)
    : QObject(parent),
      m_server(new QTcpServer(this)),
      m_port(port) {
    ensureToken();
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
}

LocalServer::~LocalServer() {
    stop();
}

bool LocalServer::start() {
    stop();
    if (m_token.isEmpty()) {
        ensureToken();
    }
    return m_server->listen(QHostAddress::LocalHost, m_port);
}

void LocalServer::stop() {
    if (m_server->isListening()) {
        m_server->close();
    }
    for (QMap<QTcpSocket*, QTimer*>::iterator it = m_clientTimers.begin(); it != m_clientTimers.end(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
    }
    m_clientTimers.clear();

    for (QMap<QTcpSocket*, QByteArray>::iterator it = m_clientBuffers.begin(); it != m_clientBuffers.end(); ++it) {
        it.key()->disconnectFromHost();
        it.key()->deleteLater();
    }
    m_clientBuffers.clear();
}

bool LocalServer::isListening() const {
    return m_server != NULL && m_server->isListening();
}

quint16 LocalServer::serverPort() const {
    return m_port;
}

QString LocalServer::errorString() const {
    return m_server != NULL ? m_server->errorString() : QString();
}

QString LocalServer::token() const {
    return m_token;
}

void LocalServer::ensureToken() {
    m_token = BridgeToken::getOrCreateToken();
}

quint16 LocalServer::effectivePort() const {
    return (m_server && m_server->isListening()) ? m_server->serverPort() : m_port;
}

bool LocalServer::isHostAllowed(const QString &rawHost) const {
    QString host = rawHost.trimmed().toLower();
    if (host.isEmpty()) {
        return false;
    }

    quint16 p = effectivePort();
    if (host == QString("127.0.0.1:%1").arg(p) || host == QString("localhost:%1").arg(p)) {
        return true;
    }
    if (m_port != 0 && (host == QString("127.0.0.1:%1").arg(m_port) || host == QString("localhost:%1").arg(m_port))) {
        return true;
    }
    return false;
}

static bool constantTimeCompare(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty() || a.size() != b.size()) {
        return false;
    }
    const QByteArray baA = a.toUtf8();
    const QByteArray baB = b.toUtf8();
    if (baA.size() != baB.size()) {
        return false;
    }
    int diff = 0;
    for (int i = 0; i < baA.size(); ++i) {
        diff |= (baA.at(i) ^ baB.at(i));
    }
    return (diff == 0);
}

// Every request except GET /ping must carry an HMAC signature made with the shared token:
//   X-HBDM-Sig = hex(HMAC-SHA256(token, "hbdm-req:v1\n" + METHOD + "\n" + path + "\n" + ts + "\n" + nonce + "\n" + hex(SHA-256(body))))
// The token itself never travels, a timestamp older than 30 s is rejected and a nonce is accepted once.
bool LocalServer::verifySignedRequest(const HttpRequest &req) {
    if (m_token.isEmpty()) {
        return false;
    }

    static const QRegularExpression nonceRe(QStringLiteral("^[0-9a-f]{32}$"));
    static const QRegularExpression sigRe(QStringLiteral("^[0-9a-f]{64}$"));

    const QString tsStr = req.m_headers.value("x-hbdm-ts");
    const QString nonce = req.m_headers.value("x-hbdm-nonce");
    const QString sig = req.m_headers.value("x-hbdm-sig");
    bool tsOk = false;
    const qint64 ts = tsStr.toLongLong(&tsOk);
    if (!tsOk || !nonceRe.match(nonce).hasMatch() || !sigRe.match(sig).hasMatch()) {
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (qAbs(now - ts) > 30) {
        return false;
    }

    for (QHash<QString, qint64>::iterator it = m_seenNonces.begin(); it != m_seenNonces.end();) {
        if (now - it.value() > 120) {
            it = m_seenNonces.erase(it);
        } else {
            ++it;
        }
    }
    if (m_seenNonces.contains(nonce)) {
        return false;
    }

    const QByteArray bodyHash = QCryptographicHash::hash(req.m_body, QCryptographicHash::Sha256).toHex();
    QByteArray message("hbdm-req:v1\n");
    message += req.m_method.toUtf8() + '\n';
    message += req.m_path.toUtf8() + '\n';
    message += tsStr.toUtf8() + '\n';
    message += nonce.toUtf8() + '\n';
    message += bodyHash;
    const QString expected = QString::fromLatin1(
        QMessageAuthenticationCode::hash(message, m_token.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (!constantTimeCompare(sig, expected)) {
        return false;
    }

    if (m_seenNonces.size() < 100000) {
        m_seenNonces.insert(nonce, now);
    }
    return true;
}

void LocalServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (socket != NULL) {
            if (m_clientBuffers.size() >= 16) {
                socket->disconnectFromHost();
                socket->deleteLater();
                continue;
            }

            m_clientBuffers[socket] = QByteArray();

            QTimer *timer = new QTimer(this);
            timer->setSingleShot(true);
            timer->setInterval(10000);
            connect(timer, SIGNAL(timeout()), this, SLOT(onClientTimeout()));
            m_clientTimers[socket] = timer;
            timer->start();

            connect(socket, SIGNAL(readyRead()), this, SLOT(onClientReadyRead()));
            connect(socket, SIGNAL(disconnected()), this, SLOT(onClientDisconnected()));
        }
    }
}

void LocalServer::onClientTimeout() {
    QTimer *timer = qobject_cast<QTimer*>(sender());
    if (timer == NULL) return;
    QTcpSocket *socket = m_clientTimers.key(timer, NULL);
    if (socket != NULL) {
        m_clientTimers.remove(socket);
        m_clientBuffers.remove(socket);
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    timer->deleteLater();
}

void LocalServer::onClientDisconnected() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket != NULL) {
        m_clientBuffers.remove(socket);
        if (m_clientTimers.contains(socket)) {
            QTimer *timer = m_clientTimers.take(socket);
            timer->stop();
            timer->deleteLater();
        }
        socket->deleteLater();
    }
}

void LocalServer::onClientReadyRead() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == NULL) return;

    m_clientBuffers[socket].append(socket->readAll());
    const QByteArray &buf = m_clientBuffers[socket];

    HttpRequest req;
    ParseResult res = parseHttpRequest(buf, req);
    if (res == ParseSuccess) {
        m_clientBuffers.remove(socket);
        handleRequest(socket, req);
    } else if (res == ParsePayloadTooLarge) {
        m_clientBuffers.remove(socket);
        QJsonObject errObj;
        errObj["ok"] = false;
        errObj["error"] = "payload too large";
        sendResponse(socket, 413, errObj, req.m_headers.value("origin"));
    } else if (buf.size() > 1024 * 1024 + 16384) {
        m_clientBuffers.remove(socket);
        QJsonObject errObj;
        errObj["ok"] = false;
        errObj["error"] = "payload too large";
        sendResponse(socket, 413, errObj);
    }
}

bool LocalServer::isOriginAllowed(const QString &rawOrigin, const QMap<QString, QString> &headers) {
    Q_UNUSED(headers);
    if (rawOrigin.isEmpty()) {
        return true;
    }
    QString origin = rawOrigin.trimmed();
    if (origin.endsWith('/')) origin.chop(1);

    static const QString s_chromeOrigin = QStringLiteral("chrome-extension://fmlcahmlpfmfnnemigpbbbnmkikepjko");
    static const QString s_chromeDevOrigin = QStringLiteral("chrome-extension://afdfemednfkkmmbpliapapodafgbbeme");
    if (origin == s_chromeOrigin ||
        origin == s_chromeDevOrigin ||
        origin.startsWith("moz-extension://") ||
        origin.startsWith("safari-web-extension://")) {
        return true;
    }
    return false;
}

LocalServer::ParseResult LocalServer::parseHttpRequest(const QByteArray &data, HttpRequest &req) {
    int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd == -1) {
        if (data.size() > 1024 * 1024) {
            return ParsePayloadTooLarge;
        }
        return ParseIncomplete;
    }

    QByteArray headBytes = data.left(headerEnd);
    QString headStr = QString::fromUtf8(headBytes);
    QStringList lines = headStr.split("\r\n");
    if (lines.isEmpty()) return ParseIncomplete;

    QString requestLine = lines.takeFirst();
    QStringList reqParts = requestLine.split(' ');
    if (reqParts.size() < 2) return ParseIncomplete;

    req.m_method = reqParts.at(0).toUpper();
    req.m_path = reqParts.at(1);

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        int colonIdx = line.indexOf(':');
        if (colonIdx != -1) {
            QString key = line.left(colonIdx).trimmed().toLower();
            QString val = line.mid(colonIdx + 1).trimmed();
            req.m_headers[key] = val;
        }
    }

    qint64 contentLength = req.m_headers.value("content-length", "0").toLongLong();
    if (contentLength > 1024 * 1024) {
        return ParsePayloadTooLarge;
    }

    int bodyStart = headerEnd + 4;
    if (data.size() - bodyStart < contentLength) {
        return ParseIncomplete;
    }

    req.m_body = data.mid(bodyStart, static_cast<int>(contentLength));
    if (req.m_body.size() > 1024 * 1024) {
        return ParsePayloadTooLarge;
    }

    return ParseSuccess;
}

void LocalServer::handleRequest(QTcpSocket *socket, const HttpRequest &req) {
    if (!isHostAllowed(req.m_headers.value("host"))) {
        QJsonObject errObj;
        errObj["ok"] = false;
        errObj["error"] = "invalid host";
        sendResponse(socket, 403, errObj, req.m_headers.value("origin"));
        return;
    }

    QString origin = req.m_headers.value("origin");
    if (!isOriginAllowed(origin, req.m_headers)) {
        QJsonObject errObj;
        errObj["ok"] = false;
        errObj["error"] = "forbidden";
        sendResponse(socket, 403, errObj, origin);
        return;
    }

    if (req.m_method == "OPTIONS") {
        QJsonObject okObj;
        okObj["ok"] = true;
        sendResponse(socket, 200, okObj, origin);
        return;
    }

    QString pathOnly = req.m_path.split('?').first();

    if (!(req.m_method == "GET" && pathOnly == "/ping")) {
        if (!verifySignedRequest(req)) {
            QJsonObject errObj;
            errObj["ok"] = false;
            errObj["error"] = "forbidden";
            sendResponse(socket, 403, errObj, origin);
            return;
        }
    }

    if (req.m_method == "GET" && pathOnly == "/ping") {
        QJsonObject pingObj;
        pingObj["ok"] = true;
        pingObj["app"] = "HBDownloadManager";
        pingObj["version"] = QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral(HBDM_VERSION) : QCoreApplication::applicationVersion();
        pingObj["ytdlp"] = VideoDownloader::isAvailable();
        pingObj["ytdlpVersion"] = VideoDownloader::ytDlpVersion();
        pingObj["ffmpeg"] = VideoDownloader::isFfmpegAvailable();
        pingObj["active"] = DownloadManager::instance().activeCount();

        QUrl queryUrl("http://localhost" + req.m_path);
        QUrlQuery query(queryUrl);
        QString nonce = query.queryItemValue("nonce");
        static const QRegularExpression pingNonceRe(QStringLiteral("^[0-9a-f]{32}$"));
        if (pingNonceRe.match(nonce).hasMatch() && !m_token.isEmpty()) {
            // Bound to the port we really listen on: a listener on another port cannot relay it
            QByteArray proof = QMessageAuthenticationCode::hash(
                QString("hbdm-ping:v1:%1:%2").arg(effectivePort()).arg(nonce).toUtf8(),
                m_token.toUtf8(),
                QCryptographicHash::Sha256
            ).toHex();
            pingObj["proof"] = QString::fromLatin1(proof);
        }

        QJsonArray extArr;
        QStringList exts = DownloadManager::instance().captureExtensions();
        for (int i = 0; i < exts.size(); ++i) {
            extArr.append(exts.at(i));
        }
        pingObj["captureExtensions"] = extArr;
        sendResponse(socket, 200, pingObj, origin);
        return;
    }

    if (req.m_method == "GET" && pathOnly == "/downloads") {
        QJsonArray itemsArr;
        const QList<DownloadItem> &list = DownloadManager::instance().items();
        int maxItems = qMin(list.size(), 50);
        for (int i = 0; i < maxItems; ++i) {
            const DownloadItem &it = list.at(i);
            QJsonObject itObj;
            itObj["id"] = it.m_id;
            itObj["name"] = it.m_fileName;

            QString stStr;
            switch (it.m_state) {
            case DownloadStateQueued: stStr = "queued"; break;
            case DownloadStateConnecting: stStr = "probing"; break;
            case DownloadStateDownloading: stStr = "downloading"; break;
            case DownloadStatePaused: stStr = "paused"; break;
            case DownloadStateCompleted: stStr = "completed"; break;
            case DownloadStateError: stStr = "failed"; break;
            case DownloadStateCancelled: stStr = "cancelled"; break;
            }
            itObj["state"] = stStr;
            itObj["progress"] = it.progress();
            itObj["speed"] = it.m_speed;
            itObj["total"] = it.m_totalBytes;
            itObj["received"] = it.m_receivedBytes;
            itObj["stage"] = it.m_stage;
            if (it.m_state == DownloadStateError && !it.m_errorMessage.isEmpty()) {
                itObj["error"] = it.m_errorMessage;
            }
            itemsArr.append(itObj);
        }

        QJsonObject respObj;
        respObj["ok"] = true;
        respObj["items"] = itemsArr;
        sendResponse(socket, 200, respObj, origin);
        return;
    }

    if (req.m_method == "POST" && pathOnly == "/download") {
        QJsonDocument doc = QJsonDocument::fromJson(req.m_body);
        if (!doc.isObject()) {
            QJsonObject errObj;
            errObj["ok"] = false;
            errObj["error"] = "invalid json";
            sendResponse(socket, 400, errObj, origin);
            return;
        }

        QJsonObject obj = doc.object();
        QString urlStr = obj["url"].toString();
        QUrl url(urlStr);
        if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
            QJsonObject errObj;
            errObj["ok"] = false;
            errObj["error"] = "invalid url";
            sendResponse(socket, 400, errObj, origin);
            return;
        }

        // Browser downloads follow the same settings as the ones added in the app
        DownloadItem item(url, DownloadManager::instance().defaultDirectory());
        item.m_requestedSegments = DownloadManager::instance().defaultSegments();
        if (obj.contains("filename") && !obj["filename"].toString().isEmpty()) {
            item.m_fileName = PathUtils::sanitizeFileName(obj["filename"].toString());
            item.m_fileNameExplicit = true;  // the browser already resolved Content-Disposition
        }
        item.m_cookieHeader = obj["cookieHeader"].toString();
        item.m_cookies = obj["cookies"].toArray();
        QString kindStr = obj["kind"].toString().trimmed().toLower();
        if (kindStr == "audio") {
            item.m_kind = DownloadKindAudio;
        } else if (kindStr == "video") {
            item.m_kind = DownloadKindVideo;
        } else if (kindStr == "file") {
            item.m_kind = DownloadKindFile;
        } else {
            if (MediaHosts::isStreamingSite(url.host())) {
                item.m_kind = DownloadKindVideo;
            } else {
                item.m_kind = DownloadKindFile;
            }
        }

        if (obj.contains("referer")) {
            item.m_headers["Referer"] = obj["referer"].toString();
        }
        if (obj.contains("userAgent")) {
            item.m_headers["User-Agent"] = obj["userAgent"].toString();
        }
        if (obj.contains("segments")) {
            item.m_requestedSegments = qBound(1, obj["segments"].toInt(8), 16);
        }
        if (obj.contains("quality")) {
            item.m_quality = obj["quality"].toString();
        }
        if (obj.contains("estimatedBytes")) {
            qint64 est = obj["estimatedBytes"].toVariant().toLongLong();
            if (est > 0) {
                item.m_totalBytes = est;
            }
        }

        const int countBefore = DownloadManager::instance().items().size();
        QString id = DownloadManager::instance().addDownload(item, true);
        const bool duplicate = (DownloadManager::instance().items().size() == countBefore);
        if (!duplicate) {
            emit atDownloadRequested(urlStr, item.m_fileName, obj["kind"].toString());
        }

        QJsonObject respObj;
        respObj["ok"] = true;
        respObj["id"] = id;
        respObj["duplicate"] = duplicate;  // the same link is already being downloaded
        sendResponse(socket, 200, respObj, origin);
        return;
    }

    if (req.m_method == "POST" && pathOnly == "/action") {
        QJsonDocument doc = QJsonDocument::fromJson(req.m_body);
        if (!doc.isObject()) {
            QJsonObject errObj;
            errObj["ok"] = false;
            errObj["error"] = "invalid json";
            sendResponse(socket, 400, errObj, origin);
            return;
        }

        QJsonObject obj = doc.object();
        QString id = obj["id"].toString();
        QString action = obj["action"].toString();

        if (action == "pause") {
            DownloadManager::instance().pauseDownload(id);
        } else if (action == "resume") {
            DownloadManager::instance().resumeDownload(id);
        } else if (action == "cancel") {
            DownloadManager::instance().cancelDownload(id);
        } else if (action == "remove") {
            DownloadManager::instance().removeDownload(id, false);
        } else {
            QJsonObject errObj;
            errObj["ok"] = false;
            errObj["error"] = "unknown action";
            sendResponse(socket, 400, errObj, origin);
            return;
        }

        QJsonObject respObj;
        respObj["ok"] = true;
        sendResponse(socket, 200, respObj, origin);
        return;
    }

    QJsonObject notFound;
    notFound["ok"] = false;
    notFound["error"] = "not found";
    sendResponse(socket, 404, notFound, origin);
}

void LocalServer::sendResponse(QTcpSocket *socket, int statusCode, const QJsonObject &jsonBody, const QString &origin) {
    QByteArray body = QJsonDocument(jsonBody).toJson(QJsonDocument::Compact);
    QString statusText;
    switch (statusCode) {
    case 200: statusText = "OK"; break;
    case 400: statusText = "Bad Request"; break;
    case 403: statusText = "Forbidden"; break;
    case 404: statusText = "Not Found"; break;
    case 413: statusText = "Payload Too Large"; break;
    default:  statusText = "Error"; break;
    }

    QByteArray response;
    response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append("Content-Type: application/json; charset=utf-8\r\n");
    response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    response.append("Cache-Control: no-store\r\n");
    response.append("X-Content-Type-Options: nosniff\r\n");

    if (!origin.isEmpty() && isOriginAllowed(origin, QMap<QString, QString>())) {
        response.append(QString("Access-Control-Allow-Origin: %1\r\n").arg(origin).toUtf8());
        response.append("Vary: Origin\r\n");
        response.append("Access-Control-Allow-Headers: Content-Type, X-HBDM-Extension, X-HBDM-Extension-Id, X-HBDM-Ts, X-HBDM-Nonce, X-HBDM-Sig\r\n");
        response.append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
        response.append("Access-Control-Allow-Private-Network: true\r\n");
        response.append("Access-Control-Allow-Local-Network: true\r\n");
    }

    response.append("Connection: close\r\n\r\n");
    response.append(body);

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}
