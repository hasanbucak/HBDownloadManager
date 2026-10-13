#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QJsonObject>
#include <QByteArray>
#include <QTimer>
#include <QHash>

class LocalServer : public QObject {
    Q_OBJECT
public:
    explicit LocalServer(quint16 port = 48213, QObject *parent = NULL);
    virtual ~LocalServer();

    bool start();
    void stop();
    bool isListening() const;
    quint16 serverPort() const;
    QString errorString() const;
    QString token() const;

signals:
    void atDownloadRequested(const QString &url, const QString &fileName, const QString &kind);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void onClientTimeout();

private:
    struct HttpRequest {
        QString m_method;
        QString m_path;
        QMap<QString, QString> m_headers;
        QByteArray m_body;
    };

    enum ParseResult {
        ParseSuccess,
        ParseIncomplete,
        ParsePayloadTooLarge
    };

    void handleRequest(QTcpSocket *socket, const HttpRequest &req);
    void sendResponse(QTcpSocket *socket, int statusCode, const QJsonObject &jsonBody, const QString &origin = QString());
    static ParseResult parseHttpRequest(const QByteArray &data, HttpRequest &req);
    static bool isOriginAllowed(const QString &origin, const QMap<QString, QString> &headers);
    bool isHostAllowed(const QString &host) const;
    bool verifySignedRequest(const HttpRequest &req);
    quint16 effectivePort() const;
    void ensureToken();

    QTcpServer *m_server;
    quint16 m_port;
    QString m_token;
    QMap<QTcpSocket*, QByteArray> m_clientBuffers;
    QMap<QTcpSocket*, QTimer*> m_clientTimers;
    QHash<QString, qint64> m_seenNonces;  // request nonce -> unix seconds (replay protection)
};
