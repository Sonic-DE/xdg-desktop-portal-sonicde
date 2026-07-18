/*
 * SPDX-FileCopyrightText: 2020 Red Hat Inc
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2020 Jan Grulich <jgrulich@redhat.com>
 */

#include "background.h"
#include "background_debug.h"
#include "dbushelpers.h"
#include "ksharedconfig.h"
#include "x11/x11controller.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>

#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>

#include <KConfigGroup>
#include <KDesktopFile>
#include <KLocalizedString>
#include <KNotification>
#include <KService>
#include <KShell>

using namespace Qt::StringLiterals;

BackgroundPortal::BackgroundPortal(QObject* parent, QDBusContext* context, X11Controller* controller)
    : QDBusAbstractAdaptor(parent)
    , m_context(context)
    , m_controller(controller)
{
    if (m_controller) {
        connect(m_controller, &X11Controller::windowsUpdated, this, &BackgroundPortal::setWindowDescriptors);
        m_refreshTimer.setInterval(std::chrono::seconds(5));
        connect(&m_refreshTimer, &QTimer::timeout, m_controller, &X11Controller::requestWindows);
        m_refreshTimer.start();
        m_controller->requestWindows();
    }
}

BackgroundPortal::~BackgroundPortal() = default;

QVariantMap BackgroundPortal::GetAppState()
{
    qCDebug(XdgDesktopPortalKdeBackground) << "GetAppState called: no parameters";
    if (m_controller) {
        m_controller->requestWindows();
    }
    return m_appStates;
}

QVariantMap BackgroundPortal::appStatesFromWindows(const QList<X11Types::WindowDescriptor>& windows, const QString& activeAppId)
{
    QVariantMap appStates;
    for (const X11Types::WindowDescriptor& window : windows) {
        if (!window.mapped || window.skipTaskbar || window.skipSwitcher) {
            continue;
        }

        QString appId = window.appId.trimmed();
        if (appId.isEmpty()) {
            appId = window.wmClass.trimmed();
        }
        if (appId.isEmpty()) {
            continue;
        }

        const bool active = !activeAppId.isEmpty() && QString::compare(appId, activeAppId, Qt::CaseInsensitive) == 0;
        const uint state = active ? Active : Running;
        const uint existing = appStates.value(appId, QVariant::fromValue<uint>(Background)).toUInt();
        appStates.insert(appId, QVariant::fromValue<uint>(qMax(existing, state)));
    }
    return appStates;
}

void BackgroundPortal::setWindowDescriptors(const QList<X11Types::WindowDescriptor>& windows)
{
    if (m_windows == windows) {
        return;
    }
    m_windows = windows;
    m_activeAppId.clear();
    for (const auto& window : windows) {
        if (window.active) {
            m_activeAppId = window.appId.isEmpty() ? window.wmClass : window.appId;
            break;
        }
    }
    updateAppStates();
}

void BackgroundPortal::setActiveApplicationId(const QString& appId)
{
    if (m_activeAppId == appId) {
        return;
    }
    m_activeAppId = appId;
    updateAppStates();
}

uint BackgroundPortal::NotifyBackground(const QDBusObjectPath &handle, const QString &app_id, const QString &name, QVariantMap &results)
{
    Q_UNUSED(results);

    qCDebug(XdgDesktopPortalKdeBackground) << "NotifyBackground called with parameters:";
    qCDebug(XdgDesktopPortalKdeBackground) << "    handle: " << handle.path();
    qCDebug(XdgDesktopPortalKdeBackground) << "    app_id: " << app_id;
    qCDebug(XdgDesktopPortalKdeBackground) << "    name: " << name;

    QDBusMessage message = m_context->message();
    auto allow = [message]() {
        const QVariantMap map = {{QStringLiteral("result"), static_cast<uint>(BackgroundPortal::Allow)}};
        QDBusMessage reply = message.createReply({PortalResponse::Success, map});
        if (!QDBusConnection::sessionBus().send(reply)) {
            qCWarning(XdgDesktopPortalKdeBackground) << "Failed to send response";
        }
    };

    auto allowOnce = [message]() {
        const QVariantMap map = {{QStringLiteral("result"), static_cast<uint>(BackgroundPortal::AllowOnce)}};
        QDBusMessage reply = message.createReply({PortalResponse::Success, map});
        if (!QDBusConnection::sessionBus().send(reply)) {
            qCWarning(XdgDesktopPortalKdeBackground) << "Failed to send response";
        }
    };

    auto forbid = [message]() {
        const QVariantMap map = {{QStringLiteral("result"), static_cast<uint>(BackgroundPortal::Forbid)}};
        QDBusMessage reply = message.createReply({PortalResponse::Success, map});
        if (!QDBusConnection::sessionBus().send(reply)) {
            qCWarning(XdgDesktopPortalKdeBackground) << "Failed to send response";
        }
    };

    if (KSharedConfig::openConfig()->group(QStringLiteral("Background")).readEntry("NotifyBackgroundApps", true)) {
        allowOnce();
        return PortalResponse::Success;
    }

    const KService::Ptr app = KService::serviceByDesktopName(app_id);

    QObject *obj = QObject::parent();

    if (!obj) {
        qCWarning(XdgDesktopPortalKdeBackground) << "Failed to get dbus context";
        return PortalResponse::OtherError;
    }

    const QString appName = app ? app->name() : app_id;
    if (m_backgroundAppWarned.contains(app_id)) {
        const QVariantMap map = {
            {QStringLiteral("result"), static_cast<uint>(BackgroundPortal::AllowOnce)},
        };
        QDBusMessage reply = message.createReply({PortalResponse::Success, map});
        if (!QDBusConnection::sessionBus().send(reply)) {
            qCWarning(XdgDesktopPortalKdeBackground) << "Failed to send response";
        }

        return PortalResponse::Success;
    }

    KNotification *notify = new KNotification(QStringLiteral("notification"), KNotification::Persistent | KNotification::DefaultEvent, this);
    notify->setTitle(i18n("Background Activity"));
    notify->setText(
        i18nc("@info %1 is the name of an application",
              "%1 wants to remain running when it has no visible windows. If you forbid this, the application will quit when its last window is closed.",
              appName));
    notify->setProperty("activated", false);

    message.setDelayedReply(true);

    auto allowAction = notify->addAction(i18nc("@action:button Allow the app to keep running with no open windows", "Allow"));

    connect(allowAction, &KNotificationAction::activated, this, [allow, notify] {
        allow();
        notify->setProperty("activated", true);
    });

    auto forbidAction = notify->addAction(i18nc("@action:button Don't allow the app to keep running without any open windows", "Forbid"));

    connect(forbidAction, &KNotificationAction::activated, this, [this, appName, allow, forbid, notify] {
        const QString title =
            i18nc("@title title of a dialog to confirm whether to allow an app to remain running with no visible windows", "Background App Usage");
        const QString text = i18nc("%1 is the name of an application",
                                   "Note that this will force %1 to quit when its last window is closed. This could cause data loss if the application has "
                                   "any unsaved changes when it happens.",
                                   appName);
        auto messageBox = new QMessageBox(QMessageBox::Question, title, text);
        messageBox->addButton(i18nc("@action:button Allow the app to keep running with no open windows", "Allow"), QMessageBox::AcceptRole);
        messageBox->addButton(i18nc("@action:button Don't allow the app to keep running without any open windows", "Forbid Anyway"), QMessageBox::RejectRole);
        messageBox->show();

        connect(messageBox, &QMessageBox::accepted, this, [messageBox, allow]() {
            allow();
            messageBox->deleteLater();
        });
        connect(messageBox, &QMessageBox::rejected, this, [messageBox, forbid]() {
            forbid();
            messageBox->deleteLater();
        });

        notify->setProperty("activated", true);
    });

    connect(notify, &KNotification::closed, this, [notify, allowOnce]() {
        if (notify->property("activated").toBool()) {
            return;
        }

        allowOnce();
    });

    notify->sendEvent();

    m_backgroundAppWarned.insert(app_id);
    connect(notify, &KNotification::closed, this, [this, app_id] {
        m_backgroundAppWarned.remove(app_id);
    });

    Q_UNUSED(name)
    return PortalResponse::Success;
}

bool BackgroundPortal::EnableAutostart(const QString &app_id, bool enable, const QStringList &commandline, uint flags)
{
    qCDebug(XdgDesktopPortalKdeBackground) << "EnableAutostart called with parameters:";
    qCDebug(XdgDesktopPortalKdeBackground) << "    app_id: " << app_id;
    qCDebug(XdgDesktopPortalKdeBackground) << "    enable: " << enable;
    qCDebug(XdgDesktopPortalKdeBackground) << "    commandline: " << commandline;
    qCDebug(XdgDesktopPortalKdeBackground) << "    flags: " << flags;

    const QString fileName = app_id + QStringLiteral(".desktop");
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/");
    const QString fullPath = directory + fileName;
    const AutostartFlags autostartFlags = static_cast<AutostartFlags>(flags);

    if (!enable) {
        QFile file(fullPath);
        if (!file.remove()) {
            qCDebug(XdgDesktopPortalKdeBackground) << "Failed to remove " << fileName << " to disable autostart.";
        }
        return false;
    }

    QDir dir(directory);
    if (!dir.mkpath(dir.absolutePath())) {
        qCDebug(XdgDesktopPortalKdeBackground) << "Failed to create autostart directory.";
        return false;
    }

    KDesktopFile desktopFile(fullPath);
    KConfigGroup desktopEntryConfigGroup = desktopFile.desktopGroup();
    desktopEntryConfigGroup.writeEntry(QStringLiteral("Type"), QStringLiteral("Application"));
    desktopEntryConfigGroup.writeEntry(QStringLiteral("Name"), app_id);
    desktopEntryConfigGroup.writeEntry(QStringLiteral("Exec"), KShell::joinArgs(commandline));
    if (autostartFlags.testFlag(AutostartFlag::Activatable)) {
        desktopEntryConfigGroup.writeEntry(QStringLiteral("DBusActivatable"), true);
    }
    desktopEntryConfigGroup.writeEntry(QStringLiteral("X-Flatpak"), app_id);

    return true;
}

void BackgroundPortal::updateAppStates()
{
    QVariantMap nextStates = appStatesFromWindows(m_windows, m_activeAppId);
    if (nextStates == m_appStates) {
        return;
    }
    m_appStates = nextStates;
    m_apps.clear();
    m_activeApps.clear();
    for (auto it = m_appStates.cbegin(); it != m_appStates.cend(); ++it) {
        m_apps.insert(it.key());
        if (it.value().toUInt() == Active) {
            m_activeApps.insert(it.key());
        }
    }
    Q_EMIT RunningApplicationsChanged();
}

#include "moc_background.cpp"
