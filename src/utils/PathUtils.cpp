#include "PathUtils.h"

QString PathUtils::sanitizeFileName(const QString &raw) {
    // Percent-decode first to handle RFC 5987 / URL encoded names (e.g. %2F -> /)
    QString name = QUrl::fromPercentEncoding(raw.toUtf8());

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
        out = out.left(out.size() - ext.size() - 1) + ext;
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

    const QString path = QDir(base).filePath(sanitized);
    return (QFileInfo(path).absolutePath() == base) ? path : QString();
}
