/*
 * SPDX-FileCopyrightText: 2016 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2016 Jan Grulich <jgrulich@redhat.com>
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_DESKTOP_PORTAL_H
#define XDG_DESKTOP_PORTAL_KDE_DESKTOP_PORTAL_H

#include <QDBusContext>
#include <QObject>
#include <QString>

class AccessPortal;
class AccountPortal;
class AppChooserPortal;
class BackgroundPortal;
class EmailPortal;
class FileChooserPortal;
class InhibitPortal;
class PrintPortal;
class ScreenshotPortal;
class SettingsPortal;
class ScreenCastPortal;
class RemoteDesktopPortal;
class DynamicLauncherPortal;
class UsbPortal;
class WallpaperPortal;
class GlobalShortcutsPortal;
class InputCapturePortal;
class ClipboardPortal;
class PortalBootstrap;
class ScreenSelectionProvider;

class DesktopPortal : public QObject, public QDBusContext
{
    Q_OBJECT
public:
    explicit DesktopPortal(PortalBootstrap* bootstrap, const QString& serviceName, QObject* parent = nullptr, ScreenSelectionProvider* selectionProvider = nullptr);

    QString serviceName() const
    {
        return m_serviceName;
    }

private:
    AccessPortal *const m_access;
    AccountPortal *const m_account;
    AppChooserPortal *const m_appChooser;
    BackgroundPortal *m_background = nullptr;
    EmailPortal *const m_email;
    FileChooserPortal *const m_fileChooser;
    InhibitPortal *const m_inhibit;
    PrintPortal *const m_print;
    ScreenshotPortal *m_screenshot = nullptr;
    SettingsPortal *const m_settings;
    ScreenCastPortal *m_screenCast = nullptr;
    RemoteDesktopPortal *m_remoteDesktop = nullptr;
    DynamicLauncherPortal *const m_dynamicLauncher;
    UsbPortal* const m_usb;
    WallpaperPortal* m_wallpaper = nullptr;
    GlobalShortcutsPortal* m_globalShortcuts = nullptr;
    InputCapturePortal* m_inputCapture = nullptr;
    ClipboardPortal* m_clipboard = nullptr;
    QString m_serviceName;
    ScreenSelectionProvider* m_selectionProvider = nullptr;
};

#endif // XDG_DESKTOP_PORTAL_KDE_DESKTOP_PORTAL_H
