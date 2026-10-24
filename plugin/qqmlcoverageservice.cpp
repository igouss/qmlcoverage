// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qqmlcoverageservice.h"
#include "coverageprotocol.h"

#include <private/qv4debugging_p.h>
#include <private/qv4engine_p.h>
#include <private/qv4executablecompilationunit_p.h>
#include <private/qv4function_p.h>
#include <private/qv4stackframe_p.h>

#include <QtQml/qjsengine.h>
#include <QtCore/qfile.h>
#include <QtCore/qhash.h>
#include <QtCore/qmutex.h>
#include <QtCore/qset.h>
#include <QtCore/qurl.h>

QT_BEGIN_NAMESPACE

const QString QQmlCoverageServiceImpl::s_key = QStringLiteral("CodeCoverage");

namespace {

struct FunctionHits
{
    quint64 calls = 0;
    QHash<int, quint64> lines;
};

using CompiledFunction = QV4::CompiledData::Function;
using UnitPtr = QQmlRefPointer<QV4::ExecutableCompilationUnit>;

// The client reads file: sources itself. Anything else it can only see if we send it.
QString sourceTheClientCannotRead(const QUrl &url)
{
    if (url.scheme() != QLatin1String("qrc"))
        return QString();
    QFile file(QLatin1Char(':') + url.path());
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

} // namespace

class CoverageCollector : public QV4::Debugging::Debugger
{
public:
    explicit CoverageCollector(QJSEngine *engine) : m_jsEngine(engine), m_engine(engine->handle())
    {
    }

    QJSEngine *jsEngine() const { return m_jsEngine; }
    QV4::ExecutionEngine *engine() const { return m_engine; }

    // Debug instructions only reach maybeBreakAtInstruction() when this returns true.
    bool pauseAtNextOpportunity() const override { return true; }

    void maybeBreakAtInstruction() override
    {
        const QV4::CppStackFrame *frame = m_engine->currentStackFrame;
        const int line = frame->lineNumber();
        // The Debug instruction before a return carries the negated line number.
        if (line > 0)
            ++hitsFor(frame->v4Function).lines[line];
    }

    void enteringFunction() override
    {
        ++hitsFor(m_engine->currentStackFrame->v4Function).calls;
    }

    void leavingFunction(const QV4::ReturnedValue &) override {}
    void aboutToThrow() override {}

    // Hits since the previous report; resets the counts. Call on the engine's thread.
    QByteArray takeReport(quint32 round, qint32 roundSize);

private:
    FunctionHits &hitsFor(QV4::Function *function)
    {
        const auto it = m_hits.find(function->compiledFunction);
        if (it != m_hits.end())
            return *it;
        // Hold the unit so the compiled function stays valid until the report.
        QV4::ExecutableCompilationUnit *unit = function->executableCompilationUnit();
        m_units.insert(unit, UnitPtr(unit));
        return m_hits[function->compiledFunction];
    }

    QJSEngine *m_jsEngine;
    QV4::ExecutionEngine *m_engine;
    QHash<const CompiledFunction *, FunctionHits> m_hits;
    QHash<const QV4::ExecutableCompilationUnit *, UnitPtr> m_units;
    QSet<const QV4::ExecutableCompilationUnit *> m_sourceSent;
};

QByteArray CoverageCollector::takeReport(quint32 round, qint32 roundSize)
{
    // Units the engine loaded but never ran still report their lines, with zero hits.
    QHash<const QV4::ExecutableCompilationUnit *, UnitPtr> units = m_units;
    const auto loaded = m_engine->compilationUnits();
    for (const UnitPtr &unit : loaded)
        units.insert(unit.data(), unit);

    QList<CoverageProtocol::File> files;
    for (const UnitPtr &unit : std::as_const(units)) {
        CoverageProtocol::File file;
        file.url = unit->finalUrlString();
        if (!m_sourceSent.contains(unit.data())) {
            file.source = sourceTheClientCannotRead(unit->finalUrl());
            m_sourceSent.insert(unit.data());
        }
        const QV4::CompiledData::Unit *data = unit->unitData();
        for (uint i = 0; i < data->functionTableSize; ++i) {
            const CompiledFunction *compiled = data->functionAt(int(i));
            const FunctionHits hits = m_hits.value(compiled);
            file.functions.append({unit->stringAt(compiled->nameIndex),
                                   int(compiled->location.line()), hits.calls});
            // The table has an entry per code offset, so a line usually appears several times.
            QSet<int> lines;
            const QV4::CompiledData::CodeOffsetToLineAndStatement *table
                    = compiled->lineAndStatementNumberTable();
            for (quint16 j = 0; j < compiled->nLineAndStatementNumbers; ++j) {
                if (table[j].line > 0)
                    lines.insert(table[j].line);
            }
            for (int line : std::as_const(lines))
                file.lines[line] += hits.lines.value(line);
        }
        files.append(file);
    }
    m_hits.clear(); // the units stay held, so loaded-but-idle code keeps reporting zeros
    return CoverageProtocol::encode(CoverageProtocol::Report{round, roundSize, files});
}

QQmlCoverageServiceImpl::QQmlCoverageServiceImpl(QObject *parent)
    : QQmlDebugService(s_key, 1.0f, parent)
{
}

// Only engines created while a client is connected are covered: whether code is compiled with
// Debug instructions is decided when the engine is set up, so attaching to an engine that already
// loaded code would report its executed lines as never hit. Clients therefore start the program
// with "block". An engine we cannot cover produces no report, so the client sees the gap.
void QQmlCoverageServiceImpl::engineAboutToBeAdded(QJSEngine *engine)
{
    QV4::ExecutionEngine *ee = engine ? engine->handle() : nullptr;
    if (ee && state() == Enabled && !ee->debugger()) {
        CoverageCollector *collector = new CoverageCollector(engine);
        ee->setDebugger(collector); // the engine owns it from here on
        QMutexLocker lock(&m_mutex);
        m_collectors.append(collector);
    }
    QQmlDebugService::engineAboutToBeAdded(engine);
}

void QQmlCoverageServiceImpl::engineAboutToBeRemoved(QJSEngine *engine)
{
    QV4::ExecutionEngine *ee = engine ? engine->handle() : nullptr;
    {
        QMutexLocker lock(&m_mutex);
        for (qsizetype i = 0; i < m_collectors.size(); ++i) {
            CoverageCollector *collector = m_collectors.at(i);
            if (collector->engine() != ee)
                continue;
            m_collectors.removeAt(i);
            if (state() == Enabled)
                emit messageToClient(s_key, collector->takeReport(0, 0));
            break;
        }
    }
    QQmlDebugService::engineAboutToBeRemoved(engine);
}

// Runs on the debug server's thread. The counts belong to each engine's thread, so the report
// is taken there; an engine destroyed before it gets to run sends its final report instead.
void QQmlCoverageServiceImpl::messageReceived(const QByteArray &message)
{
    quint32 round = 0;
    if (!CoverageProtocol::decodeRequest(message, &round))
        return;
    QMutexLocker lock(&m_mutex);
    const qint32 roundSize = qint32(m_collectors.size());
    for (CoverageCollector *collector : std::as_const(m_collectors)) {
        QMetaObject::invokeMethod(collector->jsEngine(), [this, collector, round, roundSize] {
            emit messageToClient(s_key, collector->takeReport(round, roundSize));
        }, Qt::QueuedConnection);
    }
    if (m_collectors.isEmpty())
        emit messageToClient(s_key, CoverageProtocol::encode(CoverageProtocol::Report{round, 0, {}}));
}

QT_END_NAMESPACE

#include "moc_qqmlcoverageservice.cpp"
