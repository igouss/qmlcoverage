// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQMLCOVERAGESERVICE_H
#define QQMLCOVERAGESERVICE_H

#include <private/qqmldebugservice_p.h>

#include <QtCore/qlist.h>
#include <QtCore/qmutex.h>

QT_BEGIN_NAMESPACE

class CoverageCollector;

// Records which lines and functions of QML/JS code an engine executes. It installs a
// QV4::Debugging::Debugger that never pauses: attaching any debugger makes the compiler emit
// one Debug instruction per source line and disables the JIT, AOT units and the disk cache, so
// every executed line passes through the hook. Each engine reports when it goes away, and on
// request from the client (for programs that run until killed).
class QQmlCoverageServiceImpl : public QQmlDebugService
{
    Q_OBJECT
public:
    static const QString s_key;

    explicit QQmlCoverageServiceImpl(QObject *parent = nullptr);

    void engineAboutToBeAdded(QJSEngine *engine) override;
    void engineAboutToBeRemoved(QJSEngine *engine) override;
    void messageReceived(const QByteArray &message) override;

private:
    QMutex m_mutex; // m_collectors: engine threads add and remove, the server thread reads
    QList<CoverageCollector *> m_collectors;
};

QT_END_NAMESPACE

#endif // QQMLCOVERAGESERVICE_H
