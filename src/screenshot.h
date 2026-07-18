/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_H
#define XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_H

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QDateTime>
#include <QImage>

class QDBusMessage;
class ScreenshotCapture;

class ScreenshotPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Screenshot")
    Q_PROPERTY(uint version READ version CONSTANT)
public:
    struct ColorRGB {
        double red;
        double green;
        double blue;
    };

    explicit ScreenshotPortal(QObject *parent);
    explicit ScreenshotPortal(QObject* parent, ScreenshotCapture* capture);
    ~ScreenshotPortal() override;

    uint version() const
    {
        return 2;
    }

public Q_SLOTS:
    void Screenshot(const QDBusObjectPath &handle,
                    const QString &app_id,
                    const QString &parent_window,
                    const QVariantMap &options,
                    const QDBusMessage &message,
                    uint &replyResponse,
                    QVariantMap &replyResults);

    uint PickColor(const QDBusObjectPath &handle, const QString &app_id, const QString &parent_window, const QVariantMap &options, QVariantMap &results);

    static QString screenshotFilePath(const QString& baseDirectory, const QDateTime& timestamp);
    static ColorRGB colorAtImagePosition(const QImage& image, const QPoint& position);

private:
    QImage captureImage(bool interactive, bool includeCursor, bool includeDecorations) const;
    bool saveImage(const QImage& image, QString* uri, QString* error) const;

    ScreenshotCapture* m_capture = nullptr;
};

#endif // XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_H
