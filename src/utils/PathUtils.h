#pragma once

#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

class PathUtils {
public:
    // Sanitizes untrusted filename inputs (e.g. from HTTP headers, REST payloads, dialogs).
    // Decodes percent encoding, strips path traversal components (.. and slashes),
    // replaces control characters with underscores, strips leading dots to prevent hidden files,
    // limits length to 240 UTF-8 bytes (preserving extension if <= 10 chars),
    // and falls back to "download" if empty.
    static QString sanitizeFileName(const QString &raw);

    // Safely joins directory with a filename.
    // Rejects paths that escape directory (canonical parent must match directory canonical path).
    // Returns empty string if dir is invalid or path would escape.
    static QString safeJoin(const QString &dir, const QString &name);

    // Resolves unique non-colliding target path within 240 UTF-8 bytes limit.
    static QString uniqueTargetPath(const QString &baseDir, const QString &fileName, const QString &id = QString());
};
