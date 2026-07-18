/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "screenshot.h"
#include "dbushelpers.h"
#include "request.h"
#include "screenshot_debug.h"
#include "screenshotdialog.h"
#include "utils.h"

#include <QCursor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>

Q_DECLARE_METATYPE(ScreenshotPortal::ColorRGB)

QDBusArgument &operator<<(QDBusArgument &arg, const ScreenshotPortal::ColorRGB &color)
{
    arg.beginStructure();
    arg << color.red << color.green << color.blue;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, ScreenshotPortal::ColorRGB &color)
{
    double red, green, blue;
    arg.beginStructure();
    arg >> red >> green >> blue;
    color.red = red;
    color.green = green;
    color.blue = blue;
    arg.endStructure();

    return arg;
}

QDBusArgument &operator<<(QDBusArgument &argument, const QColor &color)
{
    argument.beginStructure();
    argument << color.rgba();
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, QColor &color)
{
    argument.beginStructure();
    QRgb rgba;
    argument >> rgba;
    argument.endStructure();
    color = QColor::fromRgba(rgba);

    return argument;
}

ScreenshotPortal::ScreenshotPortal(QObject* parent)
    : ScreenshotPortal(parent, new ScreenshotCapture)
{
}

ScreenshotPortal::ScreenshotPortal(QObject* parent, ScreenshotCapture* capture)
    : QDBusAbstractAdaptor(parent)
    , m_capture(capture)
{
    if (m_capture) {
        m_capture->setParent(parent ? parent : this);
    }
    qDBusRegisterMetaType<QColor>();
    qDBusRegisterMetaType<ColorRGB>();
}

ScreenshotPortal::~ScreenshotPortal() = default;

void ScreenshotPortal::Screenshot(const QDBusObjectPath &handle,
                                  const QString &app_id,
                                  const QString &parent_window,
                                  const QVariantMap &options,
                                  const QDBusMessage &message,
                                  uint &replyResponse,
                                  QVariantMap &replyResults)
{
    qCDebug(XdgDesktopPortalKdeScreenshot) << "Screenshot called";

    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(parent_window)

    const bool interactive = options.value(QStringLiteral("interactive"), false).toBool();
    const bool includeCursor = options.value(QStringLiteral("cursor"), true).toBool();
    const bool includeDecorations = options.value(QStringLiteral("include_decorations"), true).toBool();

    auto setResult = [this, &replyResponse, &replyResults](const QImage& image) {
        QString uri;
        QString error;
        if (saveImage(image, &uri, &error)) {
            replyResponse = PortalResponse::Success;
            replyResults.insert(QStringLiteral("uri"), uri);
            return;
        }

        qCWarning(XdgDesktopPortalKdeScreenshot) << "Failed to save screenshot:" << error;
        replyResponse = PortalResponse::OtherError;
        replyResults.clear();
    };

    if (!interactive) {
        setResult(captureImage(false, includeCursor, includeDecorations));
        return;
    }

    message.setDelayedReply(true);
    replyResponse = PortalResponse::Success;
    replyResults.clear();
    auto* dialog = new ScreenshotDialog(parent());
    Request::makeClosableDialogRequest(handle, dialog);
    connect(dialog, &ScreenshotDialog::finished, this, [dialog, message, this](DialogResult result) {
        uint response = PortalResponse::Cancelled;
        QVariantMap results;
        if (result != DialogResult::Accepted) {
            QDBusConnection::sessionBus().send(message.createReply({response, results}));
            return;
        }
        QString uri;
        QString error;
        if (saveImage(dialog->image(), &uri, &error)) {
            response = PortalResponse::Success;
            results.insert(QStringLiteral("uri"), uri);
        } else {
            qCWarning(XdgDesktopPortalKdeScreenshot) << "Failed to save screenshot:" << error;
            response = PortalResponse::OtherError;
        }
        QDBusConnection::sessionBus().send(message.createReply({response, results}));
    });
}

uint ScreenshotPortal::PickColor(const QDBusObjectPath &handle,
                                 const QString &app_id,
                                 const QString &parent_window,
                                 const QVariantMap &options,
                                 QVariantMap &results)
{
    qCDebug(XdgDesktopPortalKdeScreenshot) << "PickColor called";

    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(parent_window)
    Q_UNUSED(options)

    const QImage image = captureImage(false, true, true);
    if (image.isNull()) {
        return PortalResponse::OtherError;
    }

    const QPoint pos = QCursor::pos();
    QRect workspaceGeometry;
    for (QScreen* screen : QGuiApplication::screens()) {
        workspaceGeometry = workspaceGeometry.united(screen->geometry());
    }
    const QPoint imagePos = pos - workspaceGeometry.topLeft();
    results.insert(QStringLiteral("color"), QVariant::fromValue(colorAtImagePosition(image, imagePos)));
    return PortalResponse::Success;
}

QString ScreenshotPortal::screenshotFilePath(const QString& baseDirectory, const QDateTime& timestamp)
{
    return QDir(baseDirectory).filePath(QStringLiteral("Screenshot_%1.png").arg(timestamp.toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"))));
}

ScreenshotPortal::ColorRGB ScreenshotPortal::colorAtImagePosition(const QImage& image, const QPoint& position)
{
    if (image.isNull() || !image.rect().contains(position)) {
        return {0.0, 0.0, 0.0};
    }
    const QColor color = image.pixelColor(position);
    return {color.redF(), color.greenF(), color.blueF()};
}

QImage ScreenshotPortal::captureImage(bool interactive, bool includeCursor, bool includeDecorations) const
{
    Q_UNUSED(interactive)
    Q_UNUSED(includeDecorations)
    if (!m_capture) {
        return {};
    }
    return m_capture->captureWorkspace(includeCursor);
}

bool ScreenshotPortal::saveImage(const QImage& image, QString* uri, QString* error) const
{
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("capture produced an empty image");
        }
        return false;
    }
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("failed to create screenshot directory");
        }
        return false;
    }
    const QString path = screenshotFilePath(directory, QDateTime::currentDateTime());
    if (!image.save(path, "PNG")) {
        if (error) {
            *error = QStringLiteral("failed to encode PNG");
        }
        return false;
    }
    if (uri) {
        *uri = QUrl::fromLocalFile(path).toString();
    }
    return true;
}

#include "moc_screenshot.cpp"
