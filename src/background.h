/*
 * SPDX-FileCopyrightText: 2020 Red Hat Inc
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2020 Jan Grulich <jgrulich@redhat.com>
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_BACKGROUND_H
#define XDG_DESKTOP_PORTAL_KDE_BACKGROUND_H

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QSet>
#include <QTimer>

#include "x11/x11types.h"

class KNotification;
class X11Controller;

class BackgroundPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Background")
public:
    explicit BackgroundPortal(QObject* parent, QDBusContext* context, X11Controller* controller = nullptr);
    ~BackgroundPortal() override;

    enum ApplicationState {
        Background = 0,
        Running = 1,
        Active = 2,
    };

    enum AutostartFlag {
        None = 0x0,
        Activatable = 0x1,
    };
    Q_DECLARE_FLAGS(AutostartFlags, AutostartFlag)

    enum NotifyResult {
        Forbid = 0,
        Allow = 1,
        AllowOnce = 2,
    };

    static QVariantMap appStatesFromWindows(const QList<X11Types::WindowDescriptor>& windows, const QString& activeAppId);
    void setWindowDescriptors(const QList<X11Types::WindowDescriptor>& windows);
    void setActiveApplicationId(const QString& appId);

public Q_SLOTS:
    QVariantMap GetAppState();

    uint NotifyBackground(const QDBusObjectPath &handle, const QString &app_id, const QString &name, QVariantMap &results);

    bool EnableAutostart(const QString &app_id, bool enable, const QStringList &commandline, uint flags);
Q_SIGNALS:
    void RunningApplicationsChanged();

private:
    void updateAppStates();

    uint m_notificationCounter = 0;
    QSet<QString> m_apps;
    QSet<QString> m_activeApps;
    QList<X11Types::WindowDescriptor> m_windows;
    QString m_activeAppId;
    QVariantMap m_appStates;
    QSet<QString> m_backgroundAppWarned;
    QDBusContext *const m_context;
    X11Controller* m_controller = nullptr;
    QTimer m_refreshTimer;
};
Q_DECLARE_OPERATORS_FOR_FLAGS(BackgroundPortal::AutostartFlags)

#endif // XDG_DESKTOP_PORTAL_KDE_BACKGROUND_H
