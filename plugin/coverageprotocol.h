// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef COVERAGEPROTOCOL_H
#define COVERAGEPROTOCOL_H

#include <QtCore/qbytearray.h>
#include <QtCore/qdatastream.h>
#include <QtCore/qiodevice.h>
#include <QtCore/qlist.h>
#include <QtCore/qmap.h>
#include <QtCore/qstring.h>

QT_BEGIN_NAMESPACE

// Wire format of the CodeCoverage service, shared by the service and the client so the two
// cannot drift apart.
//
// Client -> service: a snapshot request carrying a round number (> 0).
// Service -> client: one Report per engine. Each report holds the hits since that engine's
// previous report, so the client sums every report it receives. An engine reports when it is
// removed (round 0) and once per snapshot request it answers (that request's round).
namespace CoverageProtocol {

constexpr QDataStream::Version streamVersion = QDataStream::Qt_6_0;

struct Function
{
    QString name;
    int line = 0;
    quint64 calls = 0;
};

struct File
{
    QString url;
    QString source; // set for files the client cannot read itself (qrc:), once per engine
    QList<Function> functions;
    QMap<int, quint64> lines; // every executable line, including those never hit
};

struct Report
{
    quint32 round = 0;
    qint32 roundSize = 0; // engines asked to answer this round
    QList<File> files;
};

inline QDataStream &operator<<(QDataStream &s, const Function &f)
{
    return s << f.name << qint32(f.line) << f.calls;
}

inline QDataStream &operator>>(QDataStream &s, Function &f)
{
    qint32 line = 0;
    s >> f.name >> line >> f.calls;
    f.line = line;
    return s;
}

inline QDataStream &operator<<(QDataStream &s, const File &f)
{
    return s << f.url << f.source << f.functions << f.lines;
}

// QDataStream reserves a list's stated length before reading a single element, so a corrupt or
// foreign report would throw std::bad_alloc. Lists are read here an element at a time instead: a
// length the bytes cannot back ends in ReadPastEnd.
template<typename T>
QDataStream &readList(QDataStream &s, QList<T> &list)
{
    quint32 length = 0;
    s >> length;
    for (quint32 i = 0; i < length && s.status() == QDataStream::Ok; ++i) {
        T value;
        s >> value;
        list.append(value);
    }
    return s;
}

inline QDataStream &operator>>(QDataStream &s, File &f)
{
    s >> f.url >> f.source;
    return readList(s, f.functions) >> f.lines; // QMap reserves nothing
}

template<typename T>
QByteArray encode(const T &value)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(streamVersion);
    stream << value;
    return bytes;
}

inline QByteArray encode(const Report &report)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(streamVersion);
    stream << report.round << report.roundSize << report.files;
    return bytes;
}

inline bool decode(const QByteArray &bytes, Report *report)
{
    QDataStream stream(bytes);
    stream.setVersion(streamVersion);
    stream >> report->round >> report->roundSize;
    readList(stream, report->files);
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

inline bool decodeRequest(const QByteArray &bytes, quint32 *round)
{
    QDataStream stream(bytes);
    stream.setVersion(streamVersion);
    stream >> *round;
    return stream.status() == QDataStream::Ok && stream.atEnd() && *round > 0;
}

} // namespace CoverageProtocol

QT_END_NAMESPACE

#endif // COVERAGEPROTOCOL_H
