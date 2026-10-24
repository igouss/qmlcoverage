// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qqmlcoverageservicefactory.h"
#include "qqmlcoverageservice.h"

QT_BEGIN_NAMESPACE

// The service reaches into the engine's private structures, whose layout changes between
// releases. Refuse to run in any Qt but the one it was compiled against, instead of crashing.
#ifndef QMLCOVERAGE_BUILT_FOR_QT
#define QMLCOVERAGE_BUILT_FOR_QT QT_VERSION_STR
#endif

QQmlDebugService *QQmlCoverageServiceFactory::create(const QString &key)
{
    if (key != QQmlCoverageServiceImpl::s_key)
        return nullptr;
    if (qstrcmp(qVersion(), QMLCOVERAGE_BUILT_FOR_QT) != 0) {
        qWarning("CodeCoverage: built for Qt %s but running on Qt %s; coverage disabled",
                 QMLCOVERAGE_BUILT_FOR_QT, qVersion());
        return nullptr;
    }
    return new QQmlCoverageServiceImpl(this);
}

QT_END_NAMESPACE

#include "moc_qqmlcoverageservicefactory.cpp"
