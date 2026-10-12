#pragma once

#include "SegmentInfo.h"
#include <QList>
#include <QString>
#include <QRegularExpression>

class RangePlanner {
public:
    // Plans byte ranges for exact requested segment count, clamped to [1, totalBytes].
    // Produces non-overlapping, contiguous ranges that cover [0, totalBytes - 1].
    static QList<SegmentInfo> planRanges(qint64 totalBytes, int requestedSegments);

    // Adaptive range planning respecting minSegmentSize (default 512 KiB) and range support.
    static QList<SegmentInfo> planAdaptive(qint64 totalBytes, int targetSegments = 8,
                                           qint64 minSegmentSize = 512 * 1024, bool supportsRanges = true);

    // Verifies that a list of segments covers [0, totalBytes - 1] exactly with no gaps or overlaps.
    static bool verifyCoverage(const QList<SegmentInfo> &segments, qint64 totalBytes);

    // Validates Content-Range header from HTTP 206 Partial Content responses.
    static bool validateContentRange(int httpStatus, const QString &contentRangeHeader,
                                     qint64 expectedStart, qint64 segEnd, qint64 totalBytes);
};
