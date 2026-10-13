#include <QTest>
#include <QTcpSocket>
#include <QTcpServer>
#include <QHostAddress>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QDateTime>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <atomic>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include "LocalServer.h"
#include "BridgeToken.h"
#include "test_guard.h"

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

namespace {

// Everything the signing scheme needs to produce (or deliberately break) one request.
// Scheme: X-HBDM-Sig = hex(HMAC-SHA256(token, "hbdm-req:v1\n" + METHOD + "\n" + path + "\n" + ts + "\n" + nonce + "\n" + hex(SHA-256(body))))
struct ReqOpts {
    ReqOpts()
        : m_method("GET"), m_hasSigBody(false), m_tsOffset(0), m_sign(true) {}
    QByteArray m_method;
    QByteArray m_path;
    QByteArray m_body;
    QByteArray m_sigBody;      // body that is hashed into the signature when m_hasSigBody (tamper tests)
    bool m_hasSigBody;
    qint64 m_tsOffset;         // seconds added to "now" for the timestamp
    QByteArray m_nonce;        // empty = random
    QByteArray m_host;         // empty = 127.0.0.1:<port>
    bool m_sign;
    QByteArray m_extraHeaders; // raw header lines, each ending in CRLF
};

QByteArray randomNonce() {
    QByteArray raw(16, 0);
    for (int i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
    return raw.toHex();   // 32 lowercase hex characters
}

QByteArray signatureFor(const QByteArray &token, const QByteArray &method, const QByteArray &path,
                        const QByteArray &ts, const QByteArray &nonce, const QByteArray &body) {
    QByteArray message("hbdm-req:v1\n");
    message += method + '\n';
    message += path + '\n';
    message += ts + '\n';
    message += nonce + '\n';
    message += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
    return QMessageAuthenticationCode::hash(message, token, QCryptographicHash::Sha256).toHex();
}

QByteArray pingProof(const QByteArray &token, int port, const QByteArray &nonce) {
    const QByteArray message = "hbdm-ping:v1:" + QByteArray::number(port) + ":" + nonce;
    return QMessageAuthenticationCode::hash(message, token, QCryptographicHash::Sha256).toHex();
}

quint16 findFreePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

class TestBridgeAuth : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testTokenCreationAndPermissions();
    void testTokenHelperReplacesInvalidContent();
    void testTokenCreationRace();
    void testPingWithoutNonceHasNoProof();
    void testPingProofBoundToPortAndNonce();
    void testPingProofOnlyForValidNonce();
    void testOptionsCORS();
    void testUnsignedRequestReturns403();
    void testRawTokenHeaderRejected();
    void testBadSignatureRejected();
    void testSignedRequestAccepted();
    void testStaleTimestampRejected();
    void testReplayedNonceRejected();
    void testTamperedBodyRejected();
    void testSignedPostPassesAuth();
    void testDnsRebindingBlocked();
    void testPayloadTooLargeReturns413();
    void testIdleSocketTimeout();

private:
    QByteArray buildRequest(const ReqOpts &o) const;
    QByteArray sendRawHttpRequest(const QByteArray &rawRequest, int waitMs = 2000);
    QByteArray token() const { return m_thread->token().toUtf8(); }

    ServerThread *m_thread;
    quint16 m_port;
};

void TestBridgeAuth::initTestCase() {
    m_thread = NULL;
    m_port = 0;

    QString why;
    QVERIFY2(TestGuard::enterSandbox(&why), qPrintable(why));

    // A free port chosen by the OS (never a fixed one). LocalServer::serverPort() returns the
    // configured port, so the test keeps the port it asked for.
    for (int attempt = 0; attempt < 5 && m_thread == NULL; ++attempt) {
        const quint16 port = findFreePort();
        QVERIFY2(port != 0, "could not find a free port");
        ServerThread *t = new ServerThread(port, this);
        t->start();

        int tries = 0;
        while (!t->isStarted() && !t->isFinished() && tries < 100) {
            QThread::msleep(50);
            tries++;
        }
        if (t->isStarted()) {
            m_thread = t;
            m_port = port;
        } else {
            t->quit();
            t->wait();
            delete t;   // port taken in the meantime: try another one
        }
    }
    QVERIFY2(m_thread != NULL, "ServerThread failed to start LocalServer");
}

void TestBridgeAuth::cleanupTestCase() {
    if (m_thread != NULL) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = NULL;
    }
}

QByteArray TestBridgeAuth::buildRequest(const ReqOpts &o) const {
    const QByteArray ts = QByteArray::number(QDateTime::currentSecsSinceEpoch() + o.m_tsOffset);
    const QByteArray nonce = o.m_nonce.isEmpty() ? randomNonce() : o.m_nonce;
    const QByteArray host = o.m_host.isEmpty() ? QByteArray("127.0.0.1:") + QByteArray::number(m_port) : o.m_host;

    QByteArray req = o.m_method + " " + o.m_path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    if (o.m_sign) {
        const QByteArray sigBody = o.m_hasSigBody ? o.m_sigBody : o.m_body;
        req += "X-HBDM-Ts: " + ts + "\r\n";
        req += "X-HBDM-Nonce: " + nonce + "\r\n";
        req += "X-HBDM-Sig: " + signatureFor(m_thread->token().toUtf8(), o.m_method, o.m_path, ts, nonce, sigBody) + "\r\n";
    }
    req += o.m_extraHeaders;
    if (!o.m_body.isEmpty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + QByteArray::number(o.m_body.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += o.m_body;
    return req;
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
    QVERIFY(tokenPath.startsWith(TestGuard::sandboxPath()));
    QVERIFY2(QFile::exists(tokenPath), "bridge-token file must exist");

    QFile file(tokenPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString fileToken = QString::fromUtf8(file.readAll()).trimmed();
    file.close();

    QVERIFY2(!fileToken.isEmpty(), "Token in file must not be empty");
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(fileToken).hasMatch());
    QCOMPARE(m_thread->token(), fileToken);

    // Verify 0600 permissions
    struct stat st;
    int res = ::stat(tokenPath.toLocal8Bit().constData(), &st);
    QCOMPARE(res, 0);
    mode_t mode = st.st_mode & 0777;
    QCOMPARE(mode, static_cast<mode_t>(0600));
}

void TestBridgeAuth::testTokenHelperReplacesInvalidContent() {
    const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
    const QByteArray badContents[] = {
        QByteArray("not-a-token"),
        QByteArray(),                                         // empty file
        QByteArray(64, 'A'),                                  // right length, uppercase
        QByteArray(63, 'a'),                                  // one character short
        QByteArray(65, 'a'),                                  // one character long
    };

    for (size_t i = 0; i < sizeof(badContents) / sizeof(badContents[0]); ++i) {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dir = tempDir.path() + "/hb-download-manager";
        QVERIFY(QDir().mkpath(dir));
        const QString file = dir + "/bridge-token";
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(badContents[i]);
        }

        const QString token = BridgeToken::getOrCreateToken(dir);
        QVERIFY2(hex64.match(token).hasMatch(), qPrintable(QStringLiteral("case %1 gave '%2'").arg(i).arg(token)));
        QVERIFY(BridgeToken::isValidToken(token));
        QVERIFY(token.toUtf8() != badContents[i]);

        // The replacement was written to disk, so a second call returns the very same token
        QCOMPARE(BridgeToken::getOrCreateToken(dir), token);
        QCOMPARE(BridgeToken::readTokenFile(file), token);
    }
}

void TestBridgeAuth::testTokenCreationRace() {
    const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));

    for (int round = 0; round < 3; ++round) {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString emptyDir = tempDir.path() + "/hb-download-manager";

        QString token1, token2;
        std::thread t1([&]() { token1 = BridgeToken::getOrCreateToken(emptyDir); });
        std::thread t2([&]() { token2 = BridgeToken::getOrCreateToken(emptyDir); });
        t1.join();
        t2.join();

        QVERIFY(!token1.isEmpty());
        QVERIFY(!token2.isEmpty());
        QCOMPARE(token1, token2);
        QVERIFY(hex64.match(token1).hasMatch());
    }
}

void TestBridgeAuth::testPingWithoutNonceHasNoProof() {
    QByteArray req = QByteArray("GET /ping HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(m_port)
                     + "\r\nConnection: close\r\n\r\n";
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY(resp.contains("\"ok\":true"));
    QVERIFY(!resp.contains("\"proof\""));
}

void TestBridgeAuth::testPingProofBoundToPortAndNonce() {
    const QByteArray nonce = "abcdef0123456789abcdef0123456789";
    QByteArray req = QByteArray("GET /ping?nonce=") + nonce + " HTTP/1.1\r\nHost: 127.0.0.1:"
                     + QByteArray::number(m_port) + "\r\nConnection: close\r\n\r\n";
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());

    // proof = HMAC(token, "hbdm-ping:v1:<listening port>:<nonce>")
    QVERIFY2(resp.contains(pingProof(token(), m_port, nonce)), resp.constData());
    // the old, port-less format and a proof for another port must not appear
    QVERIFY(!resp.contains(pingProof(token(), m_port + 1, nonce)));
    const QByteArray legacy = QMessageAuthenticationCode::hash("hbdm-ping:" + nonce, token(), QCryptographicHash::Sha256).toHex();
    QVERIFY(!resp.contains(legacy));
}

void TestBridgeAuth::testPingProofOnlyForValidNonce() {
    const QByteArray badNonces[] = {
        QByteArray("abcdef"),                                          // too short
        QByteArray("ABCDEF0123456789ABCDEF0123456789"),                // uppercase
        QByteArray("abcdef0123456789abcdef01234567890"),               // 33 characters
        QByteArray("zzzzzz0123456789abcdef0123456789"),                // not hex
    };
    for (size_t i = 0; i < sizeof(badNonces) / sizeof(badNonces[0]); ++i) {
        QByteArray req = QByteArray("GET /ping?nonce=") + badNonces[i] + " HTTP/1.1\r\nHost: 127.0.0.1:"
                         + QByteArray::number(m_port) + "\r\nConnection: close\r\n\r\n";
        QByteArray resp = sendRawHttpRequest(req);
        QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
        QVERIFY2(!resp.contains("\"proof\""), qPrintable(QStringLiteral("proof issued for nonce %1").arg(QString::fromLatin1(badNonces[i]))));
    }
}

void TestBridgeAuth::testOptionsCORS() {
    QByteArray req = QByteArray("OPTIONS /download HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(m_port)
                     + "\r\nOrigin: chrome-extension://fmlcahmlpfmfnnemigpbbbnmkikepjko\r\nConnection: close\r\n\r\n";
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY2(resp.contains("Access-Control-Allow-Headers:"), "Missing CORS headers");
    QVERIFY2(resp.contains("X-HBDM-Ts"), "CORS headers must allow X-HBDM-Ts");
    QVERIFY2(resp.contains("X-HBDM-Nonce"), "CORS headers must allow X-HBDM-Nonce");
    QVERIFY2(resp.contains("X-HBDM-Sig"), "CORS headers must allow X-HBDM-Sig");
    QVERIFY2(!resp.contains("X-HBDM-Token"), "the raw token header must not be advertised any more");
}

void TestBridgeAuth::testUnsignedRequestReturns403() {
    ReqOpts o;
    o.m_path = "/downloads";
    o.m_sign = false;
    QByteArray resp = sendRawHttpRequest(buildRequest(o));
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
    QVERIFY(resp.contains("\"forbidden\""));
}

void TestBridgeAuth::testRawTokenHeaderRejected() {
    // X1: the raw token header alone is no longer accepted, even when the token is correct
    ReqOpts o;
    o.m_path = "/downloads";
    o.m_sign = false;
    o.m_extraHeaders = "X-HBDM-Token: " + token() + "\r\n";
    QByteArray resp = sendRawHttpRequest(buildRequest(o));
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
    QVERIFY(resp.contains("\"forbidden\""));
}

void TestBridgeAuth::testBadSignatureRejected() {
    const QByteArray ts = QByteArray::number(QDateTime::currentSecsSinceEpoch());
    QByteArray req = QByteArray("GET /downloads HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(m_port) + "\r\n";
    req += "X-HBDM-Ts: " + ts + "\r\n";
    req += "X-HBDM-Nonce: " + randomNonce() + "\r\n";
    req += "X-HBDM-Sig: " + QByteArray(64, '0') + "\r\n";
    req += "Connection: close\r\n\r\n";
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
}

void TestBridgeAuth::testSignedRequestAccepted() {
    ReqOpts o;
    o.m_path = "/downloads";
    QByteArray resp = sendRawHttpRequest(buildRequest(o));
    QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
    QVERIFY(resp.contains("\"ok\":true"));

    // a query string is part of the signed path
    ReqOpts q;
    q.m_path = "/downloads?x=1";
    QByteArray resp2 = sendRawHttpRequest(buildRequest(q));
    QVERIFY2(!resp2.startsWith("HTTP/1.1 403"), resp2.constData());
}

void TestBridgeAuth::testStaleTimestampRejected() {
    ReqOpts old;
    old.m_path = "/downloads";
    old.m_tsOffset = -120;
    QByteArray respOld = sendRawHttpRequest(buildRequest(old));
    QVERIFY2(respOld.startsWith("HTTP/1.1 403 Forbidden"), respOld.constData());

    ReqOpts future;
    future.m_path = "/downloads";
    future.m_tsOffset = 120;
    QByteArray respFuture = sendRawHttpRequest(buildRequest(future));
    QVERIFY2(respFuture.startsWith("HTTP/1.1 403 Forbidden"), respFuture.constData());

    // a little clock skew is fine
    ReqOpts skew;
    skew.m_path = "/downloads";
    skew.m_tsOffset = -10;
    QByteArray respSkew = sendRawHttpRequest(buildRequest(skew));
    QVERIFY2(respSkew.startsWith("HTTP/1.1 200 OK"), respSkew.constData());
}

void TestBridgeAuth::testReplayedNonceRejected() {
    ReqOpts o;
    o.m_path = "/downloads";
    const QByteArray raw = buildRequest(o);   // one request, sent twice byte for byte

    QByteArray first = sendRawHttpRequest(raw);
    QVERIFY2(first.startsWith("HTTP/1.1 200 OK"), first.constData());

    QByteArray second = sendRawHttpRequest(raw);
    QVERIFY2(second.startsWith("HTTP/1.1 403 Forbidden"), second.constData());
}

void TestBridgeAuth::testTamperedBodyRejected() {
    ReqOpts o;
    o.m_method = "POST";
    o.m_path = "/download";
    o.m_sigBody = "not json";
    o.m_hasSigBody = true;
    o.m_body = "not json!!";   // differs from what was signed
    QByteArray resp = sendRawHttpRequest(buildRequest(o));
    QVERIFY2(resp.startsWith("HTTP/1.1 403 Forbidden"), resp.constData());
}

void TestBridgeAuth::testSignedPostPassesAuth() {
    // Same body that was signed: authentication passes and the handler answers 400 (not JSON)
    ReqOpts o;
    o.m_method = "POST";
    o.m_path = "/download";
    o.m_body = "not json";
    QByteArray resp = sendRawHttpRequest(buildRequest(o));
    QVERIFY2(resp.startsWith("HTTP/1.1 400"), resp.constData());
    QVERIFY(resp.contains("invalid json"));
}

void TestBridgeAuth::testDnsRebindingBlocked() {
    // 1. Host header: evil.com is rejected on GET /ping
    ReqOpts p1;
    p1.m_path = "/ping";
    p1.m_sign = false;
    p1.m_host = "evil.com:" + QByteArray::number(m_port);
    QByteArray resp1 = sendRawHttpRequest(buildRequest(p1));
    QVERIFY2(resp1.startsWith("HTTP/1.1 403 Forbidden"), resp1.constData());
    QVERIFY(resp1.contains("\"invalid host\""));

    // 2. Host header: evil.com is rejected even with a valid signature
    ReqOpts p2;
    p2.m_path = "/downloads";
    p2.m_host = "evil.com";
    QByteArray resp2 = sendRawHttpRequest(buildRequest(p2));
    QVERIFY2(resp2.startsWith("HTTP/1.1 403 Forbidden"), resp2.constData());
    QVERIFY(resp2.contains("\"invalid host\""));

    // 3. Host localhost:<port> is allowed with a valid signature
    ReqOpts p3;
    p3.m_path = "/downloads";
    p3.m_host = "localhost:" + QByteArray::number(m_port);
    QByteArray resp3 = sendRawHttpRequest(buildRequest(p3));
    QVERIFY2(resp3.startsWith("HTTP/1.1 200 OK"), resp3.constData());
}

void TestBridgeAuth::testPayloadTooLargeReturns413() {
    // Declared Content-Length of 2 MB: refused before the body is even read
    const QByteArray ts = QByteArray::number(QDateTime::currentSecsSinceEpoch());
    const QByteArray nonce = randomNonce();
    QByteArray req = QByteArray("POST /download HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(m_port) + "\r\n";
    req += "X-HBDM-Ts: " + ts + "\r\n";
    req += "X-HBDM-Nonce: " + nonce + "\r\n";
    req += "X-HBDM-Sig: " + signatureFor(token(), "POST", "/download", ts, nonce, QByteArray()) + "\r\n";
    req += "Content-Length: 2000000\r\nContent-Type: application/json\r\n\r\n";
    QByteArray resp = sendRawHttpRequest(req);
    QVERIFY2(resp.startsWith("HTTP/1.1 413 Payload Too Large"), resp.constData());
    QVERIFY(resp.contains("\"payload too large\""));
}

void TestBridgeAuth::testIdleSocketTimeout() {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    QVERIFY2(socket.waitForConnected(2000), "Failed to connect for idle test");

    // Don't send anything; the server must close the socket after about 10 seconds
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
