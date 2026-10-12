#include "LocalServer.h"
#include "../core/DownloadManager.h"
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

LocalServer::LocalServer(quint16 port, QObject *parent)
    : QObject(parent),
      m_server(new QTcpServer(this)),
      m_port(port) {
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
}

LocalServer::~LocalServer() {
    stop();
}

bool LocalServer::start() {
    stop();
    return m_server->listen(QHostAddress::LocalHost, m_port);
}

void LocalServer::stop() {
    if (m_server->isListening()) {
        m_server->close();
    }
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

void LocalServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (socket != NULL) {
            m_clientBuffers[socket] = QByteArray();
            connect(socket, SIGNAL(readyRead()), this, SLOT(onClientReadyRead()));
            connect(socket, SIGNAL(disconnected()), this, SLOT(onClientDisconnected()));
        }
    }
}

void LocalServer::onClientDisconnected() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket != NULL) {
        m_clientBuffers.remove(socket);
        socket->deleteLater();
    }
}

void LocalServer::onClientReadyRead() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == NULL) return;

    m_clientBuffers[socket].append(socket->readAll());
    const QByteArray &buf = m_clientBuffers[socket];

    HttpRequest req;
    if (parseHttpRequest(buf, req)) {
        m_clientBuffers.remove(socket);
        handleRequest(socket, req);
    } else if (buf.size() > 16 * 1024 * 1024) {
        // Bellek taşmasını engelle
        socket->disconnectFromHost();
    }
}

bool LocalServer::isOriginAllowed(const QString &origin, const QMap<QString, QString> &headers) {
    if (origin.startsWith("chrome-extension://") ||
        origin.startsWith("moz-extension://") ||
        origin.startsWith("safari-web-extension://")) {
        return true;
    }
    // Tarayıcı eklentisi başlığı varsa izin ver
    if (!headers.value("x-hbdm-extension").isEmpty()) {
        return true;
    }
    return false;
}

bool LocalServer::parseHttpRequest(const QByteArray &data, HttpRequest &req) {
    int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd == -1) return false;

    QByteArray headBytes = data.left(headerEnd);
    QString headStr = QString::fromUtf8(headBytes);
    QStringList lines = headStr.split("\r\n");
    if (lines.isEmpty()) return false;

    QString requestLine = lines.takeFirst();
    QStringList reqParts = requestLine.split(' ');
    if (reqParts.size() < 2) return false;

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

    int contentLength = req.m_headers.value("content-length", "0").toInt();
    int bodyStart = headerEnd + 4;
    if (data.size() - bodyStart < contentLength) {
        return false; // Henüz gövde tamamlanmadı
    }

    req.m_body = data.mid(bodyStart, contentLength);
    return true;
}

void LocalServer::handleRequest(QTcpSocket *socket, const HttpRequest &req) {
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

    if (req.m_method == "GET" && pathOnly == "/ping") {
        QJsonObject pingObj;
        pingObj["ok"] = true;
        pingObj["app"] = "HBDownloadManager";
        pingObj["version"] = "1.3.0";
        pingObj["ytdlp"] = VideoDownloader::isAvailable();
        pingObj["active"] = DownloadManager::instance().activeCount();

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

        DownloadItem item(url);
        if (obj.contains("filename") && !obj["filename"].toString().isEmpty()) {
            item.m_fileName = obj["filename"].toString();
        }
        item.m_cookieHeader = obj["cookieHeader"].toString();
        item.m_cookies = obj["cookies"].toArray();
        QString kindStr = obj["kind"].toString();
        if (kindStr == "audio") {
            item.m_kind = DownloadKindAudio;
        } else if (kindStr == "video") {
            item.m_kind = DownloadKindVideo;
        } else {
            QString host = url.host().toLower();
            if (host.contains("youtube.com") || host.contains("youtu.be") ||
                host.contains("vimeo.com") || host.contains("dailymotion.com") ||
                host.contains("tiktok.com") || host.contains("instagram.com") ||
                host.contains("facebook.com") || host.contains("fb.watch") || host.contains("fb.com") ||
                host.contains("twitch.tv") || host.contains("x.com") || host.contains("twitter.com") ||
                obj.contains("quality")) {
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
            item.m_requestedSegments = obj["segments"].toInt(8);
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

        QString id = DownloadManager::instance().addDownload(item, true);
        emit atDownloadRequested(urlStr, item.m_fileName, obj["kind"].toString());

        QJsonObject respObj;
        respObj["ok"] = true;
        respObj["id"] = id;
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
    QString statusText = (statusCode == 200 ? "OK" : (statusCode == 403 ? "Forbidden" : (statusCode == 404 ? "Not Found" : "Error")));

    QByteArray response;
    response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append("Content-Type: application/json; charset=utf-8\r\n");
    response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    response.append("Cache-Control: no-store\r\n");
    response.append("X-Content-Type-Options: nosniff\r\n");

    if (!origin.isEmpty() && isOriginAllowed(origin, QMap<QString, QString>())) {
        response.append(QString("Access-Control-Allow-Origin: %1\r\n").arg(origin).toUtf8());
        response.append("Vary: Origin\r\n");
        response.append("Access-Control-Allow-Headers: Content-Type, X-HBDM-Extension, X-HBDM-Extension-Id\r\n");
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
