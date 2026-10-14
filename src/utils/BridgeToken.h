#pragma once

#include <QString>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <cerrno>

// Shared by the app and the native messaging host: both must end up with the same token.
// Only the app process and the native host read it; it never travels to the bridge in clear
// (requests are HMAC-signed with it).
namespace BridgeToken {

inline QString configDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + "/.config";
    }
    QString dir = base + "/hb-download-manager";
    // Create it 0700 directly; mkpath() would leave a 0755 window
    if (::mkdir(dir.toLocal8Bit().constData(), 0700) != 0 && errno != EEXIST) {
        QDir().mkpath(dir);
    }
    ::chmod(dir.toLocal8Bit().constData(), 0700);
    return dir;
}

inline QString tokenFilePath() {
    return configDir() + "/bridge-token";
}

inline bool isValidToken(const QString &token) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(token).hasMatch();
}

// Reads the token without following a symlink; empty when missing or not a valid token
inline QString readTokenFile(const QString &path) {
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return QString();
    }
    QByteArray buf(128, 0);
    ssize_t total = 0;
    while (total < buf.size()) {
        const ssize_t n = ::read(fd, buf.data() + total, static_cast<size_t>(buf.size() - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += n;
    }
    ::close(fd);
    const QString token = QString::fromLatin1(buf.left(static_cast<int>(total))).trimmed();
    return isValidToken(token) ? token : QString();
}

inline bool writeAll(int fd, const QByteArray &data) {
    qint64 off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + off, static_cast<size_t>(data.size() - off));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += n;
    }
    return true;
}

inline QString generateToken() {
    QByteArray bytes(32, 0);
    bool ok = false;
    const int urandomFd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (urandomFd >= 0) {
        ok = (::read(urandomFd, bytes.data(), 32) == 32);
        ::close(urandomFd);
    }
    if (!ok) {
        quint32 words[8];
        for (int i = 0; i < 8; ++i) {
            words[i] = QRandomGenerator::system()->generate();
        }
        bytes = QByteArray(reinterpret_cast<const char*>(words), 32);
    }
    return QString::fromLatin1(bytes.toHex());
}

// Returns the shared token, creating it atomically when absent. Returns an EMPTY string on
// failure: a token that was never saved would make the app and the native host disagree.
inline QString getOrCreateToken(const QString &overrideDir = QString()) {
    const QString dir = overrideDir.isEmpty() ? configDir() : overrideDir;
    if (!overrideDir.isEmpty()) {
        if (::mkdir(dir.toLocal8Bit().constData(), 0700) != 0 && errno != EEXIST) {
            QDir().mkpath(dir);
        }
        ::chmod(dir.toLocal8Bit().constData(), 0700);
    }
    const QString finalPath = dir + "/bridge-token";
    const QByteArray finalPathLocal = finalPath.toLocal8Bit();

    // Fast path: the usual case is a valid token that already exists
    const QString ready = readTokenFile(finalPath);
    if (!ready.isEmpty()) {
        return ready;
    }

    // The app and the native host may both get here at the same moment. Without a lock one of
    // them could take the other's freshly linked token for a broken file and unlink it, and the
    // two would end up with different tokens. flock() also serialises threads (separate open()s).
    const QByteArray lockPath = (dir + "/.bridge-token.lock").toLocal8Bit();
    const int lockFd = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lockFd >= 0) {
        while (::flock(lockFd, LOCK_EX) != 0 && errno == EINTR) {
        }
    }

    QString token;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const QString existing = readTokenFile(finalPath);
        if (!existing.isEmpty()) {
            token = existing;
            break;  // never return here: the lock below must be released
        }

        struct stat st;
        if (::lstat(finalPathLocal.constData(), &st) == 0) {
            // Present but unusable (empty, truncated by a full disk, garbage or a symlink)
            ::unlink(finalPathLocal.constData());
        }

        const QByteArray tokenBytes = generateToken().toLatin1();

        // Write a private temp file completely, then publish it with link(): link() fails with
        // EEXIST when another process won the race, and we then read the winner's token.
        const QString tmp = dir + "/.bridge-token." + QString::number(::getpid()) + "."
                            + QString::number(QRandomGenerator::system()->generate());
        const QByteArray tmpLocal = tmp.toLocal8Bit();
        const int fd = ::open(tmpLocal.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            continue;
        }
        const bool written = writeAll(fd, tokenBytes) && (::fsync(fd) == 0);
        ::close(fd);
        if (!written) {
            ::unlink(tmpLocal.constData());
            continue;
        }

        if (::link(tmpLocal.constData(), finalPathLocal.constData()) != 0 && errno != EEXIST) {
            // File system without hard links: exclusive create of the final name instead
            const int fd2 = ::open(finalPathLocal.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd2 >= 0) {
                const bool ok2 = writeAll(fd2, tokenBytes) && (::fsync(fd2) == 0);
                ::close(fd2);
                if (!ok2) {
                    ::unlink(finalPathLocal.constData());
                }
            }
        }
        ::unlink(tmpLocal.constData());

        const int dirFd = ::open(dir.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd >= 0) {
            ::fsync(dirFd);
            ::close(dirFd);
        }
    }

    if (token.isEmpty()) {
        token = readTokenFile(finalPath);
    }
    if (lockFd >= 0) {
        ::flock(lockFd, LOCK_UN);
        ::close(lockFd);
    }
    return token;
}

} // namespace BridgeToken
