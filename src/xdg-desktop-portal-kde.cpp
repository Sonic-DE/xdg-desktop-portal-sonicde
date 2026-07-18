/*
 * SPDX-FileCopyrightText: 2016 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2016 Jan Grulich <jgrulich@redhat.com>
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include <KAboutData>
#include <KCrash>
#include <KLocalizedString>

#include <memory>

#include "../version.h"
#include "debug.h"
#include "desktopportal.h"
#include "x11portalbootstrap.h"

#include <signal.h>

#ifndef XDPSONICDE_SERVICE_NAME
#define XDPSONICDE_SERVICE_NAME "org.freedesktop.impl.portal.desktop.sonicde"
#endif

#ifndef XDPSONICDE_OBJECT_PATH
#define XDPSONICDE_OBJECT_PATH "/org/freedesktop/portal/desktop"
#endif

#ifndef XDPSONICDE_DESKTOP_FILE
#define XDPSONICDE_DESKTOP_FILE "org.freedesktop.impl.portal.desktop.sonicde.desktop"
#endif

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);
    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(false);
    a.setQuitLockEnabled(false);

    if (QGuiApplication::platformName() != QLatin1String("xcb")) {
        qCCritical(XdgDesktopPortalKde) << "xdg-desktop-portal requires the xcb Qt platform plugin";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    QCommandLineParser parser;
    QCommandLineOption replaceOption(u"replace"_s, u"Replace running instance"_s);
    parser.addOption(replaceOption);

    const QLatin1String serviceName(QDPSONICDE_SERVICE_NAME_LITERAL);
    const QLatin1String objectPath(QDPSONICDE_OBJECT_PATH_LITERAL);
    const QLatin1String desktopFileName(QDPSONICDE_DESKTOP_FILE_LITERAL);

    KAboutData about(serviceName, QString(), QStringLiteral(XDPK_VERSION_STRING));
    about.setDesktopFileName(desktopFileName);
    about.setupCommandLine(&parser);
    KAboutData::setApplicationData(about);

    parser.process(a);
    about.processCommandLine(&parser);

    KCrash::initialize();

    PortalBootstrap bootstrap(&a);
    QString readyError;
    if (!bootstrap.waitForReady(5000, &readyError)) {
        qCCritical(XdgDesktopPortalKde) << "X11 portal backend failed to become ready:" << readyError;
        return 1;
    }
    std::unique_ptr<DesktopPortal> desktopPortal;

    const auto dbusQueueOption = parser.isSet(replaceOption) ? QDBusConnectionInterface::ReplaceExistingService : QDBusConnectionInterface::DontQueueService;

    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (!sessionBus.interface()->registerService(serviceName, dbusQueueOption, QDBusConnectionInterface::AllowReplacement)) {
        qCCritical(XdgDesktopPortalKde) << "Failed to register" << serviceName << "service";
        return 1;
    }
    desktopPortal = std::make_unique<DesktopPortal>(&bootstrap, serviceName);
    if (!sessionBus.registerObject(QString(objectPath), desktopPortal.get(), QDBusConnection::ExportAdaptors)) {
        qCCritical(XdgDesktopPortalKde) << "Failed to register desktop portal at" << objectPath;
        return 1;
    }
    qCDebug(XdgDesktopPortalKde) << "Desktop portal registered successfully as" << serviceName << "at" << objectPath;

    const int result = a.exec();
    sessionBus.unregisterObject(QString(objectPath));
    desktopPortal.reset();
    return result;
}
