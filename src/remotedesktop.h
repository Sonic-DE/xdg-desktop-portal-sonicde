/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_REMOTEDESKTOP_H
#define XDG_DESKTOP_PORTAL_KDE_REMOTEDESKTOP_H

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QHash>

#include "screencast.h"

class QDBusMessage;
class X11Controller;
class X11Input;
class XEisMounter;
class RemoteDesktopSession;

class RemoteDesktopPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.RemoteDesktop")
    Q_PROPERTY(uint version READ version CONSTANT)
    Q_PROPERTY(uint AvailableDeviceTypes READ AvailableDeviceTypes CONSTANT)
public:
    explicit RemoteDesktopPortal(QObject* parent, X11Controller* controller = nullptr);
    ~RemoteDesktopPortal() override;

    enum DeviceType {
        None = 0x0,
        Keyboard = 0x1,
        Pointer = 0x2,
        TouchScreen = 0x4,
        All = (Keyboard | Pointer | TouchScreen),
    };
    Q_DECLARE_FLAGS(DeviceTypes, DeviceType)

    uint version() const
    {
        return 2;
    }
    uint AvailableDeviceTypes() const;

public Q_SLOTS:
    uint CreateSession(const QDBusObjectPath &handle,
                       const QDBusObjectPath &session_handle,
                       const QString &app_id,
                       const QVariantMap &options,
                       QVariantMap &results);

    uint SelectDevices(const QDBusObjectPath &handle,
                       const QDBusObjectPath &session_handle,
                       const QString &app_id,
                       const QVariantMap &options,
                       QVariantMap &results);

    void Start(const QDBusObjectPath &handle,
               const QDBusObjectPath &session_handle,
               const QString &app_id,
               const QString &parent_window,
               const QVariantMap &options,
               const QDBusMessage &message,
               uint &replyResponse,
               QVariantMap &replyResults);

    void NotifyPointerMotion(const QDBusObjectPath &session_handle, const QVariantMap &options, double dx, double dy);

    void NotifyPointerMotionAbsolute(const QDBusObjectPath &session_handle, const QVariantMap &options, uint stream, double x, double y);

    void NotifyPointerButton(const QDBusObjectPath &session_handle, const QVariantMap &options, int button, uint state);

    void NotifyPointerAxis(const QDBusObjectPath &session_handle, const QVariantMap &options, double dx, double dy);

    void NotifyPointerAxisDiscrete(const QDBusObjectPath &session_handle, const QVariantMap &options, uint axis, int steps);

    void NotifyKeyboardKeycode(const QDBusObjectPath &session_handle, const QVariantMap &options, int keycode, uint state);

    void NotifyKeyboardKeysym(const QDBusObjectPath &session_handle, const QVariantMap &options, int keysym, uint state);

    void NotifyTouchDown(const QDBusObjectPath &session_handle, const QVariantMap &options, uint stream, uint slot, double x, double y);

    void NotifyTouchMotion(const QDBusObjectPath &session_handle, const QVariantMap &options, uint stream, uint slot, double x, double y);

    void NotifyTouchUp(const QDBusObjectPath &session_handle, const QVariantMap &options, uint slot);

    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, const QDBusMessage &message);

private:
    RemoteDesktopSession* validSession(const QDBusObjectPath& handle, DeviceType capability) const;
    XEisMounter* eisForSession(const QString& sessionPath);
    X11Controller* m_controller = nullptr;
    X11Input* m_input = nullptr;
    QHash<QString, XEisMounter*> m_eisBySession;
};
Q_DECLARE_OPERATORS_FOR_FLAGS(RemoteDesktopPortal::DeviceTypes)

class RemoteDesktopSession : public ScreenCastSession
{
    Q_OBJECT
public:
    explicit RemoteDesktopSession(QObject* parent, const QString& appId, const QString& path, X11Controller* controller = nullptr);
    ~RemoteDesktopSession() override;

    RemoteDesktopPortal::DeviceTypes deviceTypes() const;
    void setDeviceTypes(RemoteDesktopPortal::DeviceTypes deviceTypes);

    bool screenSharingEnabled() const;
    void setScreenSharingEnabled(bool enabled);

    bool clipboardEnabled() const;
    void setClipboardEnabled(bool enabled);

    void acquireStreamingInput();
    void refreshDescription() override;

    void setEisCookie(int cookie);
    int eisCookie() const;
    bool started() const
    {
        return m_started;
    }
    void setStarted(bool started)
    {
        m_started = started;
    }

    SessionType type() const override
    {
        return SessionType::RemoteDesktop;
    }

private:
    bool m_screenSharingEnabled;
    bool m_clipboardEnabled;
    RemoteDesktopPortal::DeviceTypes m_deviceTypes = RemoteDesktopPortal::None;
    bool m_acquired = false;
    int m_cookie = 0;
    bool m_started = false;
};

#endif // XDG_DESKTOP_PORTAL_KDE_REMOTEDESKTOP_H
