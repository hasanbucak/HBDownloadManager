#include <QTest>
#include <QTcpSocket>
#include <QHostAddress>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <sys/stat.h>
#include "LocalServer.h"
#include "BridgeToken.h"

class ServerThread : public QThread {
    Q_OBJECT
public:
    explicit ServerThread(quint16 port, QObject *parent = nullptr)
        : QThread(parent), m_port(port), m_started(false) {}
    ~ServerThread() {
        quit();
        wait();
    }

    void run() override {
        LocalServer server(m_port);
        if (!server.start()) {
            return;
        }
        m_token = server.token();
        m_started = true;
        exec();
        server.stop();
    }

    QString token() const { return m_token; }
    bool isStarted() const { return m_started; }

private:
    quint16 m_port;
    QString m_token;
    std::atomic<bool> m_started;
};

class TestBridgeAuth : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testTokenCreationAndPermissions();
    void testPingWithoutToken();
    void testOptionsCORS();
    void testDownloadsWithoutTokenReturns403();
    void testDownloadsWithWrongTokenReturns403();
    void testDownloadsWithCorrectTokenReturns200();
    void testDnsRebindingBlocked();
    void testPayloadTooLargeReturns413();
    void testIdleSocketTimeout();

private:
    QByteArray sendRawHttpRequest(const QByteArray &rawRequest, int waitMs = 2000);

    ServerThread *m_thread;
    quint16 m_port;
};

void TestBridgeAuth::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hb-download-manager";
    QVERIFY2(!configDir.startsWith(QDir::homePath() + "/.config/"), "Safety assertion failed: test mode not isolating config path!");

    m_port = 49213; // Use distinct port for testing
    m_thread = new ServerThread(m_port, this);
    m_thread->start();

    // Wait until server is listening
    int tries = 0;
    while (!m_thread->isStarted() && tries < 50) {
        QThread::msleep(50);
        tries++;
    }
    QVERIFY2(m_thread->isStarted(), "ServerThread failed to start LocalServer");
}

void TestBridgeAuth::cleanupTestCase() {
    if (m_thread != NULL) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = NULL;
    }
}

QByteArray TestBridgeAuth::sendRawHttpRequest(const QByteArray &rawRequest, int waitMs) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    if (!socket.waitForConnected(2000)) {
        return QByteArray();
    }
    socket.write(rawRequest);
    socket.flush();

    QByteArray response;
    while (socket.waitForReadyRead(waitMs)) {
        response.append(socket.readAll());
        if (response.contains("\r\n\r\n")) {
            int headerEnd = response.indexOf("\r\n\r\n");
            QByteArray head = response.left(headerEnd);
            int clIdx = head.toLower().indexOf("content-length:");
            if (clIdx != -1) {
                int clEnd = head.indexOf("\r\n", clIdx);
                int length = head.mid(clIdx + 15, clEnd - (clIdx + 15)).trimmed().toInt();
                if (response.size() >= headerEnd + 4 + length) {
                    break;
                }
            }
        }
    }
    socket.disconnectFromHost();
    return response;
}

void TestBridgeAuth::testTokenCreationAndPermissions() {
    QString tokenPath = BridgeToken::tokenFilePath();
    QVERIFY2(QFile::exists(tokenPath), "bridge-token file must exist");

    QFile file(tokenPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString fileToken = QString::fromUtf8(file.readAll()).trimmed();
    file.close();

    QVERIFY2(!fileToken.isEmpty(), "Token in file must not be empty");
    QCOMPARE(fileToken.length(), 64); // 32 bytes in hex = 64 characters
    QCOMPARE(m_thread->token(), fileToken);

    // Verify 0600 permissions
    struct stat st;
    int res = ::stat(tokenPath.toLocal8Bit().constData(), &st);
    QCOMPARE(res, 0);
    mode_t mode = st.st_mode & 0777;
    QCOMPARE(mode, static_cast<mode_t>(0600));
}

void TestBridgeAuth::testPingWithoutToken() {
    QByteArray req = QString("GET /ping HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "Connection: close\r\n\r\n").arg(m_port).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY(resp.contains("\"ok\":true"));
}

void TestBridgeAuth::testOptionsCORS() {
    QByteArray req = QString("OPTIONS /download HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "Origin: chrome-extension://afdfemednfkkmmbpliapapodafgbbeme\r\n"
                             "Connection: close\r\n\r\n").arg(m_port).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY2(resp.contains("Access-Control-Allow-Headers:"), "Missing CORS headers");
    QVERIFY2(resp.contains("X-HBDM-Token"), "CORS headers must allow X-HBDM-Token");
}

void TestBridgeAuth::testDownloadsWithoutTokenReturns403() {
    QByteArray req = QString("GET /downloads HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "Connection: close\r\n\r\n").arg(m_port).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
    QVERIFY(resp.contains("\"forbidden\""));
}

void TestBridgeAuth::testDownloadsWithWrongTokenReturns403() {
    QByteArray req = QString("GET /downloads HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "X-HBDM-Token: invalid_token_1234567890\r\n"
                             "Connection: close\r\n\r\n").arg(m_port).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
    QVERIFY(resp.contains("\"forbidden\""));
}

void TestBridgeAuth::testDownloadsWithCorrectTokenReturns200() {
    QByteArray req = QString("GET /downloads HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "X-HBDM-Token: %2\r\n"
                             "Connection: close\r\n\r\n").arg(m_port).arg(m_thread->token()).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY(resp.contains("\"ok\":true"));
}

void TestBridgeAuth::testDnsRebindingBlocked() {
    // 1. Host header: evil.com should be rejected on GET /ping
    QByteArray req1 = QString("GET /ping HTTP/1.1\r\n"
                              "Host: evil.com:%1\r\n"
                              "Connection: close\r\n\r\n").arg(m_port).toUtf8();
    QByteArray resp1 = sendRawHttpRequest(req1);
    QVERIFY2(resp1.startsWith("HTTP/1.1 403 Forbidden"), resp1.constData());
    QVERIFY(resp1.contains("\"invalid host\""));

    // 2. Host header: evil.com should be rejected even with valid token
    QByteArray req2 = QString("GET /downloads HTTP/1.1\r\n"
                              "Host: evil.com\r\n"
                              "X-HBDM-Token: %1\r\n"
                              "Connection: close\r\n\r\n").arg(m_thread->token()).toUtf8();
    QByteArray resp2 = sendRawHttpRequest(req2);
    QVERIFY2(resp2.startsWith("HTTP/1.1 403 Forbidden"), resp2.constData());
    QVERIFY(resp2.contains("\"invalid host\""));

    // 3. Host localhost:<port> must be allowed with valid token
    QByteArray req3 = QString("GET /downloads HTTP/1.1\r\n"
                              "Host: localhost:%1\r\n"
                              "X-HBDM-Token: %2\r\n"
                              "Connection: close\r\n\r\n").arg(m_port).arg(m_thread->token()).toUtf8();
    QByteArray resp3 = sendRawHttpRequest(req3);
    QVERIFY2(resp3.startsWith("HTTP/1.1 200 OK"), resp3.constData());
}

void TestBridgeAuth::testPayloadTooLargeReturns413() {
    // Request with declared Content-Length > 1MB (e.g. 2MB)
    QByteArray req = QString("POST /download HTTP/1.1\r\n"
                             "Host: 127.0.0.1:%1\r\n"
                             "X-HBDM-Token: %2\r\n"
                             "Content-Length: 2000000\r\n"
                             "Content-Type: application/json\r\n\r\n").arg(m_port).arg(m_thread->token()).toUtf8();
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 413 Payload Too Large"), resp.constData());
    QVERIFY(resp.contains("\"payload too large\""));
}

void TestBridgeAuth::testIdleSocketTimeout() {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    QVERIFY2(socket.waitForConnected(2000), "Failed to connect for idle test");

    // Don't send anything; wait for server to close socket after 10 seconds
    QElapsedTimer timer;
    timer.start();

    bool disconnected = false;
    while (timer.elapsed() < 13000) {
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            disconnected = true;
            break;
        }
        if (socket.waitForDisconnected(500)) {
            disconnected = true;
            break;
        }
    }

    qint64 elapsed = timer.elapsed();
    QVERIFY2(disconnected, "Socket was not closed by server timeout");
    QVERIFY2(elapsed >= 9500 && elapsed <= 13000,
             qPrintable(QString("Timeout occurred at %1 ms, expected ~10000 ms").arg(elapsed)));
}

QTEST_MAIN(TestBridgeAuth)
#include "test_bridge_auth.moc"
