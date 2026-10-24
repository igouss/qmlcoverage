// Copyright (C) 2026 Iouri Goussev
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QQMLCOVERAGESERVICEFACTORY_H
#define QQMLCOVERAGESERVICEFACTORY_H

#include <private/qqmldebugservicefactory_p.h>

QT_BEGIN_NAMESPACE

class QQmlCoverageServiceFactory : public QQmlDebugServiceFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlDebugServiceFactory_iid FILE "qqmlcoverageservice.json")
public:
    QQmlDebugService *create(const QString &key) override;
};

QT_END_NAMESPACE

#endif // QQMLCOVERAGESERVICEFACTORY_H
