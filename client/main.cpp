// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

// qmlcoverage: runs a QML program with the CodeCoverage debug service and writes lcov.
//
//   qmlcoverage -o coverage.info [--plugins dir] <program> [args...]
//
// For a program that runs until stopped, send qmlcoverage SIGINT or SIGTERM (Ctrl+C): it takes
// a final snapshot from every engine, stops the program and writes the report.

#include "coverageprotocol.h"

#include <private/qqmldebugclient_p.h>
#include <private/qqmldebugconnection_p.h>

#include <QtCore/qcommandlineparser.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qprocess.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qset.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qtimer.h>
#include <QtCore/qurl.h>

#include <csignal>
#include <cstdio>
#include <functional>
#include <unistd.h>

using CoverageProtocol::File;
using CoverageProtocol::Report;

namespace {

// Lines and functions summed over every report that mentioned a file.
struct MergedFile
{
    QString source; // only for files the client cannot read itself
    QMap<int, quint64> lines;
    QMap<QPair<int, QString>, quint64> functions; // (line, name) -> calls
};

class CoverageClient : public QQmlDebugClient
{
public:
    explicit CoverageClient(QQmlDebugConnection *connection)
        : QQmlDebugClient(QStringLiteral("CodeCoverage"), connection)
    {
    }

    // Asks every engine for its hits so far; onSnapshotComplete runs once all of them answered.
    void requestSnapshot()
    {
        ++m_round;
        m_roundSize = -1;
        m_answered = 0;
        sendMessage(CoverageProtocol::encode(m_round));
    }

    int reports = 0;
    int malformed = 0;
    QMap<QString, MergedFile> files;
    std::function<void()> onSnapshotComplete;

private:
    void messageReceived(const QByteArray &message) override
    {
        Report report;
        if (!CoverageProtocol::decode(message, &report)) {
            ++malformed;
            return;
        }
        ++reports;
        merge(report.files);
        if (m_round == 0)
            return;
        // An engine destroyed while the request was in flight answers with its final report.
        if (report.round == m_round) {
            m_roundSize = report.roundSize;
            ++m_answered;
        } else if (report.round == 0) {
            ++m_answered;
        }
        if (m_roundSize >= 0 && m_answered >= m_roundSize && onSnapshotComplete)
            onSnapshotComplete();
    }

    void merge(const QList<File> &received)
    {
        for (const File &file : received) {
            MergedFile &merged = files[file.url];
            if (merged.source.isEmpty())
                merged.source = file.source;
            for (auto it = file.lines.cbegin(); it != file.lines.cend(); ++it)
                merged.lines[it.key()] += it.value();
            for (const CoverageProtocol::Function &function : file.functions)
                merged.functions[{function.line, function.name}] += function.calls;
        }
    }

    quint32 m_round = 0;
    qint32 m_roundSize = -1;
    qint32 m_answered = 0;
};

// lcov separates the function name with a comma and ends it at the line break.
QString lcovFunctionName(QString name, int line)
{
    if (name.isEmpty())
        name = QStringLiteral("<anonymous>:%1").arg(line);
    return name.replace(QLatin1Char(','), QLatin1Char(';'))
               .replace(QLatin1Char('\n'), QLatin1Char(' '));
}

// Line numbers (1-based) whose source holds nothing but closing punctuation. The compiler puts a
// function's epilogue on its closing brace, and that code is unreachable after a final return, so
// such a line would show as never executed. Like statement-based JS coverage tools, report
// statements only.
QSet<int> punctuationOnlyLines(const QString &source)
{
    static const QRegularExpression closingOnly(QStringLiteral("^[\\s})\\];,]*$"));
    QSet<int> result;
    const QStringList lines = source.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (closingOnly.match(lines.at(i)).hasMatch())
            result.insert(int(i) + 1);
    }
    return result;
}

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(file.readAll())
                                                            : QString();
}

bool writeText(const QString &path, const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    QFile out(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && out.open(QIODevice::WriteOnly)
            && out.write(bytes) == bytes.size() && out.flush();
}

// Writes the report. An lcov record points at the original of a file: URL, and at a copy below
// sourceDir of a source the program sent (qrc:). Files with neither are listed in skipped. On
// failure, failedPath is the file that could not be written.
bool writeLcov(const QMap<QString, MergedFile> &files, const QString &output,
               const QString &sourceDir, QStringList *skipped, QString *failedPath)
{
    *failedPath = output;
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream s(&out);
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        const QUrl url(file.key());
        QString path;
        QString source;
        if (url.isLocalFile()) {
            path = url.toLocalFile();
            source = readText(path);
        } else if (!file->source.isEmpty()) {
            path = QDir(sourceDir).absoluteFilePath(url.scheme() + url.path());
            source = file->source;
            if (!writeText(path, source)) {
                *failedPath = path;
                return false;
            }
        } else {
            skipped->append(file.key());
            continue;
        }
        s << "TN:\nSF:" << path << '\n';
        for (auto f = file->functions.cbegin(); f != file->functions.cend(); ++f)
            s << "FN:" << f.key().first << ',' << lcovFunctionName(f.key().second, f.key().first) << '\n';
        int functionsHit = 0;
        for (auto f = file->functions.cbegin(); f != file->functions.cend(); ++f) {
            s << "FNDA:" << f.value() << ',' << lcovFunctionName(f.key().second, f.key().first) << '\n';
            if (f.value() > 0)
                ++functionsHit;
        }
        s << "FNF:" << file->functions.size() << "\nFNH:" << functionsHit << '\n';
        const QSet<int> punctuation = punctuationOnlyLines(source);
        int linesFound = 0;
        int linesHit = 0;
        for (auto l = file->lines.cbegin(); l != file->lines.cend(); ++l) {
            if (punctuation.contains(l.key()))
                continue;
            s << "DA:" << l.key() << ',' << l.value() << '\n';
            ++linesFound;
            if (l.value() > 0)
                ++linesHit;
        }
        s << "LF:" << linesFound << "\nLH:" << linesHit << "\nend_of_record\n";
    }
    s.flush(); // writes and flushes the file: a full disk shows only now
    return s.status() == QTextStream::Ok;
}

int stopPipe[2] = {-1, -1};

void requestStop(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(stopPipe[1], &byte, 1);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qmlcoverage"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "Runs a QML program and writes the QML/JS lines and functions it executed as lcov.\n"
            "Send SIGINT or SIGTERM to stop a long-running program and write what it covered."));
    parser.addHelpOption();
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    const QCommandLineOption outputOption({QStringLiteral("o"), QStringLiteral("output")},
                                          QStringLiteral("lcov file to write."),
                                          QStringLiteral("file"), QStringLiteral("coverage.info"));
    const QCommandLineOption pluginsOption(
            QStringLiteral("plugins"),
            QStringLiteral("Directory containing qmltooling/ with the CodeCoverage plugin; "
                           "prepended to the program's QT_PLUGIN_PATH."),
            QStringLiteral("dir"));
    const QCommandLineOption sourcesOption(
            QStringLiteral("sources"),
            QStringLiteral("Directory for sources the program sent (qrc:). Default: <output>.src"),
            QStringLiteral("dir"));
    parser.addOption(outputOption);
    parser.addOption(pluginsOption);
    parser.addOption(sourcesOption);
    parser.addPositionalArgument(QStringLiteral("program"), QStringLiteral("Program to run."));
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("Its arguments."),
                                 QStringLiteral("[args...]"));
    parser.process(app);

    QStringList command = parser.positionalArguments();
    if (command.isEmpty())
        parser.showHelp(2);
    const QString program = command.takeFirst();
    const QString output = parser.value(outputOption);
    const QString sourceDir = parser.isSet(sourcesOption) ? parser.value(sourcesOption)
                                                          : output + QStringLiteral(".src");

    QTemporaryDir socketDir;
    if (!socketDir.isValid() || ::pipe(stopPipe) != 0) {
        std::fprintf(stderr, "qmlcoverage: cannot set up the socket or signal pipe\n");
        return 2;
    }
    const QString socket = socketDir.filePath(QStringLiteral("coverage.sock"));

    QQmlDebugConnection connection;
    CoverageClient client(&connection); // must exist before the handshake lists clients
    connection.startLocalServer(socket);

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    // Own process group, so a terminal's Ctrl+C reaches only us and we can snapshot first.
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // With stderr on a pipe, Qt may log to the journal instead; we forward stderr, so keep it.
    if (!env.contains(QStringLiteral("QT_FORCE_STDERR_LOGGING")))
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    if (parser.isSet(pluginsOption)) {
        const QString existing = env.value(QStringLiteral("QT_PLUGIN_PATH"));
        env.insert(QStringLiteral("QT_PLUGIN_PATH"),
                   existing.isEmpty() ? parser.value(pluginsOption)
                                      : parser.value(pluginsOption) + QLatin1Char(':') + existing);
    }
    process.setProcessEnvironment(env);
    command.prepend(QStringLiteral("-qmljsdebugger=file:%1,block,services:CodeCoverage").arg(socket));

    bool stopping = false;
    bool snapshotTimedOut = false;
    QTimer snapshotTimeout;
    snapshotTimeout.setSingleShot(true);
    snapshotTimeout.setInterval(10000);
    const auto stopProgram = [&] {
        snapshotTimeout.stop();
        process.terminate();
    };
    client.onSnapshotComplete = stopProgram;
    QObject::connect(&snapshotTimeout, &QTimer::timeout, &app, [&] {
        snapshotTimedOut = true;
        stopProgram();
    });

    QSocketNotifier stopNotifier(stopPipe[0], QSocketNotifier::Read);
    QObject::connect(&stopNotifier, &QSocketNotifier::activated, &app, [&] {
        char byte;
        [[maybe_unused]] const ssize_t got = ::read(stopPipe[0], &byte, 1);
        if (stopping)
            return;
        stopping = true;
        if (client.state() != QQmlDebugClient::Enabled) {
            process.terminate();
            return;
        }
        client.requestSnapshot();
        snapshotTimeout.start();
    });
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    int programExit = 0;
    bool crashed = false;
    QObject::connect(&process, &QProcess::finished, &app,
                     [&](int code, QProcess::ExitStatus status) {
                         programExit = code;
                         crashed = status != QProcess::NormalExit;
                         // Reports already sent are queued on the socket; drain them first.
                         QMetaObject::invokeMethod(&app, &QCoreApplication::quit,
                                                   Qt::QueuedConnection);
                     });
    process.start(program, command);
    if (!process.waitForStarted()) {
        std::fprintf(stderr, "qmlcoverage: cannot run %s: %s\n", qPrintable(program),
                     qPrintable(process.errorString()));
        return 2;
    }
    app.exec();

    if (crashed && !stopping) {
        std::fprintf(stderr, "qmlcoverage: %s crashed; no coverage written\n", qPrintable(program));
        return 3;
    }
    // No report means the service never ran: a silent empty lcov would read as "nothing covered".
    if (client.reports == 0 || client.malformed > 0) {
        std::fprintf(stderr,
                     "qmlcoverage: %d coverage report(s), %d malformed; is the CodeCoverage "
                     "plugin on the program's plugin path (--plugins), built for this Qt?\n",
                     client.reports, client.malformed);
        return 4;
    }

    QStringList skipped;
    QString failedPath;
    if (!writeLcov(client.files, output, sourceDir, &skipped, &failedPath)) {
        std::fprintf(stderr, "qmlcoverage: cannot write %s\n", qPrintable(failedPath));
        return 2;
    }
    for (const QString &url : std::as_const(skipped))
        std::fprintf(stderr, "qmlcoverage: no source for %s; not written\n", qPrintable(url));
    std::fprintf(stderr, "qmlcoverage: %d report(s), %lld file(s) -> %s\n", client.reports,
                 qlonglong(client.files.size() - skipped.size()), qPrintable(output));
    if (snapshotTimedOut) {
        std::fprintf(stderr, "qmlcoverage: not every engine answered the final snapshot\n");
        return 5;
    }
    return stopping ? 0 : programExit;
}
