/*
 * DOS-XC /qt/qpa/main.cpp
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <qpa/qplatformintegrationplugin.h>
#include "qdosxc.h"

QT_BEGIN_NAMESPACE

class QDosxcIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "dosxc.json")
public:
    QPlatformIntegration *create(const QString &system, const QStringList &) override
    {
        return system.compare(QLatin1String("dosxc"), Qt::CaseInsensitive) ? nullptr : new QDosxcIntegration;
    }
};

QT_END_NAMESPACE

#include "main.moc"
