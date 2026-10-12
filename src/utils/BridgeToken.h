#pragma once

#include <QString>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>

namespace BridgeToken {

inline QString configDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + "/.config";
    }
    QString dir = base + "/hb-download-manager";
    QDir().mkpath(dir);
    ::chmod(dir.toLocal8Bit().constData(), 0700);
    return dir;
}

inline QString tokenFilePath() {
    return configDir() + "/bridge-token";
}

inline QString getOrCreateToken(const QString &overrideDir = QString()) {
    QString dir = overrideDir.isEmpty() ? configDir() : overrideDir;
    QDir().mkpath(dir);
    ::chmod(dir.toLocal8Bit().constData(), 0700);
    QString finalPath = dir + "/bridge-token";

    // Try reading existing token first
    {
        QFile file(finalPath);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            QString token = QString::fromUtf8(file.readAll()).trimmed();
            file.close();
            if (!token.isEmpty()) {
                return token;
            }
        }
    }

    // Generate a secure 32-byte hex token
    QByteArray bytes(32, 0);
    int urandomFd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (urandomFd >= 0) {
        ssize_t r = ::read(urandomFd, bytes.data(), 32);
        (void)r;
        ::close(urandomFd);
    }
    if (bytes.size() != 32 || bytes == QByteArray(32, 0)) {
        quint32 words[8];
        for (int i = 0; i < 8; ++i) {
            words[i] = QRandomGenerator::system()->generate();
        }
        bytes = QByteArray(reinterpret_cast<const char*>(words), 32);
    }
    QString newToken = QString::fromLatin1(bytes.toHex());

    // S9: atomic creation with O_EXCL temp file and hard link
    QString tmp = dir + "/.bridge-token." + QString::number(::getpid()) + "." + QString::number(QRandomGenerator::system()->generate());
    int fd = ::open(tmp.toLocal8Bit().constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        QByteArray tokenUtf8 = newToken.toUtf8();
        ssize_t written = ::write(fd, tokenUtf8.constData(), tokenUtf8.size());
        (void)written;
        ::fsync(fd);
        ::close(fd);

        if (::link(tmp.toLocal8Bit().constData(), finalPath.toLocal8Bit().constData()) != 0 && errno != EEXIST) {
            // Link failed due to an error other than file already existing
        }
        ::unlink(tmp.toLocal8Bit().constData());
    }

    // Always read back from finalPath so both processes agree
    QFile readFile(finalPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        QString token = QString::fromUtf8(readFile.readAll()).trimmed();
        readFile.close();
        if (!token.isEmpty()) {
            return token;
        }
    }

    return newToken;
}

} // namespace BridgeToken
