#include "RangePlanner.h"
#include <QtGlobal>

QList<SegmentInfo> RangePlanner::planRanges(qint64 totalBytes, int requestedSegments) {
    QList<SegmentInfo> segments;
    if (totalBytes <= 0) {
        return segments;
    }

    int count = requestedSegments;
    if (count < 1) {
        count = 1;
    }
    // Cannot create more segments than available bytes
    if (static_cast<qint64>(count) > totalBytes) {
        count = static_cast<int>(totalBytes);
    }

    segments.reserve(count);
    const qint64 segSize = totalBytes / count;
    for (int i = 0; i < count; ++i) {
        SegmentInfo s;
        s.m_index = i;
        s.m_start = i * segSize;
        s.m_end = (i == count - 1) ? (totalBytes - 1) : (s.m_start + segSize - 1);
        s.m_received = 0;
        segments.append(s);
    }

    return segments;
}

QList<SegmentInfo> RangePlanner::planAdaptive(qint64 totalBytes, int targetSegments,
                                              qint64 minSegmentSize, bool supportsRanges) {
    if (totalBytes <= 0) {
        return QList<SegmentInfo>();
    }

    if (!supportsRanges) {
        return planRanges(totalBytes, 1);
    }

    if (minSegmentSize <= 0) {
        minSegmentSize = 512 * 1024;
    }

    const int maxSegments = static_cast<int>(totalBytes / minSegmentSize);
    const int target = (targetSegments > 0) ? targetSegments : 8;
    const int count = qMax(1, qMin(target, maxSegments));

    return planRanges(totalBytes, count);
}

bool RangePlanner::verifyCoverage(const QList<SegmentInfo> &segments, qint64 totalBytes) {
    if (totalBytes <= 0) {
        return segments.isEmpty();
    }
    if (segments.isEmpty()) {
        return false;
    }

    if (segments.first().m_start != 0) {
        return false;
    }
    if (segments.last().m_end != totalBytes - 1) {
        return false;
    }

    qint64 totalCovered = 0;
    for (int i = 0; i < segments.size(); ++i) {
        const SegmentInfo &s = segments.at(i);
        if (s.m_start > s.m_end) {
            return false;
        }
        if (i > 0) {
            if (s.m_start != segments.at(i - 1).m_end + 1) {
                return false; // Gap or overlap detected
            }
        }
        totalCovered += s.length();
    }

    return totalCovered == totalBytes;
}

bool RangePlanner::validateContentRange(int httpStatus, const QString &contentRangeHeader,
                                        qint64 expectedStart, qint64 segEnd, qint64 totalBytes) {
    if (httpStatus != 206) {
        return false; // 200 = Range ignored, 4xx/5xx = error
    }

    // Content-Range: bytes <first>-<last>/<total|*>
    static const QRegularExpression re(QStringLiteral("^bytes\\s+(\\d+)-(\\d+)/(\\d+|\\*)$"));
    const QRegularExpressionMatch m = re.match(contentRangeHeader.trimmed());
    if (!m.hasMatch()) {
        return false;
    }

    const qint64 first = m.captured(1).toLongLong();
    const qint64 last  = m.captured(2).toLongLong();

    if (first != expectedStart) {
        return false;
    }
    if (last > segEnd) {
        return false;
    }
    if (first > last) {
        return false;
    }
    if (m.captured(3) != QLatin1String("*") && totalBytes > 0 && m.captured(3).toLongLong() != totalBytes) {
        return false;
    }

    return true;
}
