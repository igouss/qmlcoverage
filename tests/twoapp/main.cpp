// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

// Test program: two QML engines alive at the same time, each loading one file. Both live on the
// main thread: Qt attaches debug services to no other engines.
//
//   twoapp -qmljsdebugger=... [--release] [--inline] <a.qml> <b.qml>
//
// Quits cleanly on SIGTERM, so both engines are destroyed and send their final reports.
//
//   --release  destroys both engines once loaded and keeps running without any.
//   --inline   loads b.qml from data under app:/<name>, a URL the coverage client cannot read.

#include <QtCore/qcommandlineparser.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qurl.h>
#include <QtQml/qqmlcomponent.h>
#include <QtQml/qqmldebug.h>
#include <QtQml/qqmlengine.h>

#include <csignal>
#include <cstdio>
#include <memory>
#include <unistd.h>

namespace {

int quitPipe[2] = {-1, -1};

void requestQuit(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t written = ::write(quitPipe[1], &byte, 1);
}

struct Loaded
{
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root; // declared last, so destroyed before its engine
};

Loaded load(const QString &path, bool inlined)
{
    Loaded loaded{std::make_unique<QQmlEngine>(), nullptr};
    QQmlComponent component(loaded.engine.get());
    if (inlined) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            component.setData(file.readAll(), QUrl(QLatin1String("app:/") + QFileInfo(path).fileName()));
    } else {
        component.loadUrl(QUrl::fromLocalFile(path));
    }
    if (component.isLoading()) { // imports of a non-file URL resolve asynchronously
        QEventLoop loop;
        QObject::connect(&component, &QQmlComponent::statusChanged, &loop, &QEventLoop::quit);
        loop.exec();
    }
    loaded.root.reset(component.create());
    if (!loaded.root)
        std::fprintf(stderr, "twoapp: %s: %s\n", qPrintable(path), qPrintable(component.errorString()));
    return loaded;
}

} // namespace

int main(int argc, char *argv[])
{
    QQmlDebuggingEnabler::enableDebugging(false);
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    const QCommandLineOption release(QStringLiteral("release"));
    const QCommandLineOption inlined(QStringLiteral("inline"));
    parser.addOptions({release, inlined});
    parser.process(app);
    const QStringList files = parser.positionalArguments();
    if (files.size() != 2 || ::pipe(quitPipe) != 0)
        return 2;

    QSocketNotifier quitNotifier(quitPipe[0], QSocketNotifier::Read);
    QObject::connect(&quitNotifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
    std::signal(SIGTERM, requestQuit);

    Loaded a = load(files.at(0), false);
    Loaded b = load(files.at(1), parser.isSet(inlined));
    if (!a.root || !b.root)
        return 1;
    if (parser.isSet(release)) {
        a = Loaded();
        b = Loaded();
    }
    return app.exec(); // a and b are destroyed here, before the application
}
