// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "wallpaper.h"

#include "dbushelpers.h"
#include "quickdialog.h"
#include "request.h"
#include "utils.h"
#include "wallpaper_debug.h"

#include <KConfig>
#include <KConfigGroup>
#include <QDBusConnection>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QUrl>

#include <xcb/xcb.h>

using namespace Qt::StringLiterals;

namespace
{
static xcb_atom_t internAtom(xcb_connection_t* connection, const QByteArray& name)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, false, name.size(), name.constData());
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (!reply) {
        return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

bool setDesktopBackground(const QUrl& url)
{
    if (!url.isLocalFile()) {
        return false;
    }
    QImage image(url.toLocalFile());
    QScreen* screen = QGuiApplication::primaryScreen();
    if (image.isNull() || !screen) {
        return false;
    }
    const QSize size = screen->geometry().size();
    image = image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    image = image.copy(QRect((image.width() - size.width()) / 2, (image.height() - size.height()) / 2, size.width(), size.height())).convertToFormat(QImage::Format_RGB32);

    int screenNumber = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screenNumber);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection) {
            xcb_disconnect(connection);
        }
        return false;
    }

    const xcb_setup_t* setup = xcb_get_setup(connection);
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNumber; ++i) {
        xcb_screen_next(&iterator);
    }
    xcb_screen_t* xcbScreen = iterator.data;
    if (!xcbScreen) {
        xcb_disconnect(connection);
        return false;
    }

    const xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, xcbScreen->root_depth, pixmap, xcbScreen->root, image.width(), image.height());
    const xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, pixmap, 0, nullptr);
    xcb_put_image(connection,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        pixmap,
        gc,
        image.width(),
        image.height(),
        0,
        0,
        0,
        xcbScreen->root_depth,
        image.sizeInBytes(),
        image.constBits());
    xcb_free_gc(connection, gc);

    const xcb_atom_t rootPixmap = internAtom(connection, "_XROOTPMAP_ID");
    const xcb_atom_t esetRootPixmap = internAtom(connection, "ESETROOT_PMAP_ID");
    if (rootPixmap != XCB_ATOM_NONE) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcbScreen->root, rootPixmap, XCB_ATOM_PIXMAP, 32, 1, &pixmap);
    }
    if (esetRootPixmap != XCB_ATOM_NONE) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, xcbScreen->root, esetRootPixmap, XCB_ATOM_PIXMAP, 32, 1, &pixmap);
    }

    const uint32_t values[] = {pixmap};
    xcb_change_window_attributes(connection, xcbScreen->root, XCB_CW_BACK_PIXMAP, values);
    xcb_clear_area(connection, false, xcbScreen->root, 0, 0, image.width(), image.height());
    xcb_set_close_down_mode(connection, XCB_CLOSE_DOWN_RETAIN_PERMANENT);
    xcb_flush(connection);
    xcb_disconnect(connection);
    return true;
}

bool setLockScreenBackground(const QUrl& url)
{
    KConfig screenLockerConfig("kscreenlockerrc"_L1);
    auto greeterGroup = screenLockerConfig.group(QString()).group("Greeter"_L1);
    greeterGroup.writeEntry("WallpaperPlugin", u"org.kde.image"_s);
    auto configGroup = greeterGroup.group(u"Wallpaper"_s).group(u"org.kde.image"_s).group(u"General"_s);
    configGroup.writeEntry("Image", url.toString());
    screenLockerConfig.sync();
    return true;
}

bool setWallpaper(const QUrl& url, WallpaperLocation::Location location)
{
    switch (location) {
    case WallpaperLocation::Desktop:
        return setDesktopBackground(url);
    case WallpaperLocation::Lockscreen:
        return setLockScreenBackground(url);
    case WallpaperLocation::Both:
        return setDesktopBackground(url) && setLockScreenBackground(url);
    }
    return false;
}
}

WallpaperLocation::Location WallpaperPortal::locationFromSetOn(const QString& setOn, bool* ok)
{
    if (ok) {
        *ok = true;
    }
    if (setOn == "background"_L1) {
        return WallpaperLocation::Desktop;
    }
    if (setOn == "lockscreen"_L1) {
        return WallpaperLocation::Lockscreen;
    }
    if (setOn == "both"_L1) {
        return WallpaperLocation::Both;
    }
    if (ok) {
        *ok = false;
    }
    return WallpaperLocation::Desktop;
}

void WallpaperPortal::SetWallpaperURI(const QDBusObjectPath &handle,
                                      const QString &app_id,
                                      const QString &parent_window,
                                      const QString &uri,
                                      const QVariantMap &options,
                                      const QDBusMessage &message,
                                      uint &replyResponse)
{
    qCDebug(XdgDesktopPortalKdeWallpaper) << "SetWallpaperURI called";
    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(parent_window)
    Q_UNUSED(message)

    const bool showPreview = options.value("show-preview"_L1, true).toBool();
    bool validLocation = false;
    const WallpaperLocation::Location location = locationFromSetOn(options.value("set-on"_L1).toString(), &validLocation);
    if (!validLocation) {
        replyResponse = PortalResponse::OtherError;
        return;
    }

    const QUrl url(uri);
    if (!url.isValid() || !url.isLocalFile()) {
        replyResponse = PortalResponse::OtherError;
        return;
    }

    if (!showPreview) {
        replyResponse = setWallpaper(url, location) ? PortalResponse::Success : PortalResponse::OtherError;
        return;
    }

    auto* dialog = new QuickDialog(parent());
    dialog->create(u"WallpaperDialog"_s, {{u"location"_s, int(location)}, {u"app"_s, app_id}, {u"image"_s, url}});
    Utils::setParentWindow(dialog->windowHandle(), parent_window);
    Request::makeClosableDialogRequest(handle, dialog);
    message.setDelayedReply(true);
    connect(dialog, &QuickDialog::finished, this, [dialog, message, url, location](DialogResult result) {
        const uint response = result == DialogResult::Accepted && setWallpaper(url, location)
            ? PortalResponse::Success
            : (result == DialogResult::Accepted ? PortalResponse::OtherError : PortalResponse::Cancelled);
        QDBusConnection::sessionBus().send(message.createReply(QVariantList{response}));
        dialog->deleteLater();
    });
    replyResponse = PortalResponse::Success;
}

#include "moc_wallpaper.cpp"
