#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QJsonObject>
#include <QByteArray>

class LocalServer : public QObject {
    Q_OBJECT
public:
    explicit LocalServer(quint16 port = 48213, QObject *parent = NULL);
    virtual ~LocalServer();

    bool start();
    void stop();
    bool isListening() const;
    quint16 serverPort() const;

signals:
    void atDownloadRequested(const QString &url, const QString &fileName, const QString &kind);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

private:
    struct HttpRequest {
        QString m_method;
        QString m_path;
        QMap<QString, QString> m_headers;
        QByteArray m_body;
    };

    void handleRequest(QTcpSocket *socket, const HttpRequest &req);
    void sendResponse(QTcpSocket *socket, int statusCode, const QJsonObject &jsonBody, const QString &origin = QString());
    static bool parseHttpRequest(const QByteArray &data, HttpRequest &req);
    static bool isOriginAllowed(const QString &origin, const QMap<QString, QString> &headers);

    QTcpServer *m_server;
    quint16 m_port;
    QMap<QTcpSocket*, QByteArray> m_clientBuffers;
};
