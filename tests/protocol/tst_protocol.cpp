// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

// The client decodes whatever arrives on the CodeCoverage channel: a report from a mismatched
// service, or a corrupt one, must be refused, never thrown on.

#include "coverageprotocol.h"

#include <QtCore/qbytearray.h>
#include <QtCore/qdatastream.h>
#include <QtCore/qiodevice.h>
#include <QtTest/qtest.h>

#include <new>

using namespace CoverageProtocol;

namespace {

enum class Outcome { Decoded, Refused, Threw };

Outcome decodeWithoutThrowing(const QByteArray &bytes, Report *report)
{
    try {
        return decode(bytes, report) ? Outcome::Decoded : Outcome::Refused;
    } catch (const std::bad_alloc &) {
        return Outcome::Threw;
    }
}

// A stream positioned where a report's fields start, for hand-built reports.
struct Writer
{
    QByteArray bytes;
    QDataStream stream{&bytes, QIODevice::WriteOnly};
    Writer() { stream.setVersion(streamVersion); }
};

constexpr quint32 hugeCount = 0x7ffffffe;

} // namespace

class tst_Protocol : public QObject
{
    Q_OBJECT
private slots:
    void decodesAnEmptyReport()
    {
        Report report;
        QCOMPARE(decodeWithoutThrowing(encode(Report{3, 2, {}}), &report), Outcome::Decoded);
        QCOMPARE(report.round, 3u);
        QCOMPARE(report.roundSize, 2);
        QCOMPARE(report.files.size(), 0);
    }

    void decodesOneFile()
    {
        File file{QStringLiteral("file:///a.qml"), QString(), {{QStringLiteral("f"), 2, 5}}, {{2, 7}}};
        Report report;
        QCOMPARE(decodeWithoutThrowing(encode(Report{0, 0, {file}}), &report), Outcome::Decoded);
        QCOMPARE(report.files.size(), 1);
        QCOMPARE(report.files.at(0).url, file.url);
        QCOMPARE(report.files.at(0).functions.at(0).calls, 5u);
        QCOMPARE(report.files.at(0).lines.value(2), 7u);
    }

    void decodesManyFiles()
    {
        File a{QStringLiteral("file:///a.qml"), QString(), {}, {{1, 1}, {2, 0}}};
        File b{QStringLiteral("qrc:/b.qml"), QStringLiteral("source"), {}, {}};
        Report report;
        QCOMPARE(decodeWithoutThrowing(encode(Report{1, 1, {a, b}}), &report), Outcome::Decoded);
        QCOMPARE(report.files.size(), 2);
        QCOMPARE(report.files.at(0).lines.size(), 2);
        QCOMPARE(report.files.at(1).source, QStringLiteral("source"));
    }

    void refusesNothing()
    {
        Report report;
        QCOMPARE(decodeWithoutThrowing(QByteArray(), &report), Outcome::Refused);
    }

    void refusesTrailingBytes()
    {
        Report report;
        QCOMPARE(decodeWithoutThrowing(encode(Report{1, 1, {}}) + '\0', &report), Outcome::Refused);
    }

    // What the in-tree CodeCoverage service of Qt dev sends: a different wire format.
    void refusesTheInTreeServiceReport()
    {
        Writer w;
        w.stream << QByteArray("REPORT") << quint32(1) << qint32(1) << qint32(0);
        Report report;
        QCOMPARE(decodeWithoutThrowing(w.bytes, &report), Outcome::Refused);
    }

    void refusesAFileCountTheBytesCannotHold()
    {
        Writer w;
        w.stream << quint32(1) << qint32(1) << hugeCount;
        Report report;
        QCOMPARE(decodeWithoutThrowing(w.bytes, &report), Outcome::Refused);
    }

    void refusesAFunctionCountTheBytesCannotHold()
    {
        Writer w;
        w.stream << quint32(1) << qint32(1) << quint32(1) << QString() << QString() << hugeCount;
        Report report;
        QCOMPARE(decodeWithoutThrowing(w.bytes, &report), Outcome::Refused);
    }

    void refusesALineCountTheBytesCannotHold()
    {
        Writer w;
        w.stream << quint32(1) << qint32(1) << quint32(1) << QString() << QString() << quint32(0)
                 << hugeCount;
        Report report;
        QCOMPARE(decodeWithoutThrowing(w.bytes, &report), Outcome::Refused);
    }
};

QTEST_APPLESS_MAIN(tst_Protocol)

#include "tst_protocol.moc"
