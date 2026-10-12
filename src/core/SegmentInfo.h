#pragma once

#include <QtGlobal>
#include <QJsonObject>

struct SegmentInfo {
    int m_index;
    qint64 m_start;
    qint64 m_end;       // Dahil (inclusive)
    qint64 m_received;

    SegmentInfo()
        : m_index(0), m_start(0), m_end(0), m_received(0) {}

    SegmentInfo(int index, qint64 start, qint64 end, qint64 received = 0)
        : m_index(index), m_start(start), m_end(end), m_received(received) {}

    qint64 length() const {
        return (m_end >= m_start) ? (m_end - m_start + 1) : 0;
    }

    bool isDone() const {
        return length() > 0 && m_received >= length();
    }

    double progress() const {
        qint64 len = length();
        if (len <= 0) return 0.0;
        return qBound(0.0, static_cast<double>(m_received) / static_cast<double>(len), 1.0);
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["index"] = m_index;
        obj["start"] = m_start;
        obj["end"] = m_end;
        obj["received"] = m_received;
        return obj;
    }

    static SegmentInfo fromJson(const QJsonObject &obj) {
        SegmentInfo s;
        s.m_index = obj["index"].toInt();
        s.m_start = obj["start"].toVariant().toLongLong();
        s.m_end = obj["end"].toVariant().toLongLong();
        s.m_received = obj["received"].toVariant().toLongLong();
        return s;
    }
};
