#include "PathUtils.h"
#include <QRegularExpression>

namespace {
// Removes n characters from the end without leaving half of a surrogate pair behind
QString chopSafe(QString s, int n) {
    s.chop(n);
    if (!s.isEmpty() && s.at(s.size() - 1).isHighSurrogate()) {
        s.chop(1);
    }
    return s;
}
}

QString PathUtils::sanitizeFileName(const QString &raw) {
    // D7: Do not percent-decode here (RFC 5987 branch already decodes in header parser)
    QString name = raw;

    // Normalize Windows backslashes to forward slashes
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // Keep basename only (strips path traversals like ../ and absolute paths like /etc/)
    name = name.section(QLatin1Char('/'), -1);

    // Replace ASCII control characters and DEL (0x7F) with underscore
    QString out;
    out.reserve(name.size());
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        out += (c.unicode() < 0x20 || c == QChar(0x7f)) ? QChar('_') : c;
    }

    out = out.trimmed();

    // Strip leading dots to prevent hidden files and relative traversal ("..")
    while (out.startsWith(QLatin1Char('.'))) {
        out.remove(0, 1);
    }

    if (out.isEmpty()) {
        out = QStringLiteral("download");
    }

    // Truncate to maximum 240 UTF-8 bytes (ext4 filename limit is 255 bytes), preserving extension
    while (out.toUtf8().size() > 240) {
        const int dot = out.lastIndexOf(QLatin1Char('.'));
        const QString ext = (dot > 0 && out.size() - dot <= 10) ? out.mid(dot) : QString();
        out = chopSafe(out.left(out.size() - ext.size()), 1) + ext;
    }

    if (out.isEmpty()) {
        out = QStringLiteral("download");
    }

    return out;
}

QString PathUtils::safeJoin(const QString &dir, const QString &name) {
    if (dir.isEmpty()) {
        return QString();
    }

    const QString base = QDir(dir).canonicalPath();
    if (base.isEmpty()) {
        return QString();
    }

    const QString sanitized = sanitizeFileName(name);
    if (sanitized.isEmpty()) {
        return QString();
    }

    const QDir baseDir(base);
    const QString path = baseDir.filePath(sanitized);
    return (QFileInfo(path).absolutePath() == base) ? path : QString();
}

QString PathUtils::uniqueTargetPath(const QString &baseDir, const QString &fileName, const QString &id) {
    const QString cleanName = sanitizeFileName(fileName);
    const QFileInfo info(cleanName);
    // Keep the stem as it is: "Movie (2019).mkv" must stay "Movie (2019).mkv"
    const QString stem = info.completeBaseName();
    const QString ext = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();

    for (int n = 0; n < 10000; ++n) {
        const QString tag = (n == 0) ? QString() : QStringLiteral(" (%1)").arg(n);
        QString s = stem;
        while (!s.isEmpty() && (s + tag + ext).toUtf8().size() > 240) {
            s = chopSafe(s, 1);
        }
        const QString path = QDir(baseDir).filePath(s + tag + ext);
        const QFileInfo fi(path);
        if (!fi.exists() && !fi.isSymLink()) {
            return path;
        }
    }
    return QDir(baseDir).filePath(QStringLiteral("download-%1%2").arg(id.isEmpty() ? QStringLiteral("file") : id, ext));
}

bool PathUtils::isInsideDir(const QString &path, const QString &dir) {
    if (path.isEmpty() || dir.isEmpty()) {
        return false;
    }
    const QString base = QDir(dir).canonicalPath();
    if (base.isEmpty()) {
        return false;
    }
    // Resolve the parent, not the file: a symlinked file is judged by where the link itself lives
    const QString parent = QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath();
    if (parent.isEmpty()) {
        return false;
    }
    return parent == base || parent.startsWith(base + QLatin1Char('/'));
}
