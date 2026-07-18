/*
 * SPDX-FileCopyrightText: 2016 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2016 Jan Grulich <jgrulich@redhat.com>
 */

#include "desktopportal.h"
#include "desktopportal_debug.h"

#include "access.h"
#include "account.h"
#include "appchooser.h"
#include "background.h"
#include "clipboard.h"
#include "dynamiclauncher.h"
#include "email.h"
#include "filechooser.h"
#include "globalshortcuts.h"
#include "inhibit.h"
#include "inputcapture.h"
#include "print.h"
#include "remotedesktop.h"
#include "screencast.h"
#include "screenselectionprovider.h"
#include "screenshot.h"
#include "settings.h"
#include "usb.h"
#include "wallpaper.h"
#include "x11portalbootstrap.h"

#include <QGuiApplication>

namespace {
bool isSupportedDesktop()
{
    const QByteArray current = qgetenv("XDG_CURRENT_DESKTOP");
    const QList<QByteArray> tokens = current.split(':');
    for (const QByteArray& token : tokens) {
        if (token.compare("KDE", Qt::CaseInsensitive) == 0 || token.compare("SONICDE", Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace

DesktopPortal::DesktopPortal(PortalBootstrap* bootstrap, const QString& serviceName, QObject* parent, ScreenSelectionProvider* selectionProvider)
    : QObject(parent)
    , QDBusContext()
    , m_access(new AccessPortal(this))
    , m_account(new AccountPortal(this))
    , m_appChooser(new AppChooserPortal(this))
    , m_email(new EmailPortal(this))
    , m_fileChooser(new FileChooserPortal(this))
    , m_inhibit(new InhibitPortal(this))
    , m_print(new PrintPortal(this))
    , m_settings(new SettingsPortal(this))
    , m_dynamicLauncher(new DynamicLauncherPortal(this))
    , m_usb(new UsbPortal(this))
    , m_serviceName(serviceName)
    , m_selectionProvider(selectionProvider ? selectionProvider : new DialogScreenSelectionProvider(this))
{
    if (isSupportedDesktop() && QGuiApplication::platformName() == QLatin1String("xcb")) {
        QObject* context = parent ? parent : this;
        X11Controller* controller = bootstrap ? bootstrap->controller() : nullptr;
        m_globalShortcuts = new GlobalShortcutsPortal(context);
        m_background = new BackgroundPortal(context, this, controller);
        m_screenCast = new ScreenCastPortal(context, controller, m_selectionProvider);
        m_remoteDesktop = new RemoteDesktopPortal(context, controller);
        m_screenshot = new ScreenshotPortal(context);
        m_inputCapture = new InputCapturePortal(context, controller);
        m_clipboard = new ClipboardPortal(context);
        m_wallpaper = new WallpaperPortal(context);
    }
    Q_UNUSED(bootstrap);
}

#include "moc_desktopportal.cpp"
