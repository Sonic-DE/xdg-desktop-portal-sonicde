/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "screenshotdialog.h"
#include "screenshotdialog_debug.h"
#include "x11/x11capture.h"

#include <QGuiApplication>
#include <QScreen>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

ScreenshotCapture::ScreenshotCapture(QObject* parent)
    : QObject(parent)
{
}

ScreenshotCapture::~ScreenshotCapture() = default;

QImage ScreenshotCapture::captureWorkspace(bool includeCursor)
{
    X11Capture capture;
    return capture.captureWorkspace(includeCursor);
}

QImage ScreenshotCapture::captureActiveWindow(bool includeCursor, bool includeDecorations)
{
    Q_UNUSED(includeDecorations)
    X11Capture capture;
    QImage image = capture.captureActiveWindow(includeCursor);
    return image.isNull() ? capture.captureWorkspace(includeCursor) : image;
}

ScreenshotDialog::ScreenshotDialog(QObject* parent)
    : ScreenshotDialog(new ScreenshotCapture, parent)
{
}

ScreenshotDialog::ScreenshotDialog(ScreenshotCapture* capture, QObject* parent)
    : QuickDialog(parent)
    , m_capture(capture)
{
    if (m_capture) {
        m_capture->setParent(this);
    }
    QStandardItemModel *model = new QStandardItemModel(this);
    model->appendRow(new QStandardItem(i18n("Full Screen")));
    model->appendRow(new QStandardItem(i18n("Current Screen")));
    model->appendRow(new QStandardItem(i18n("Active Window")));
    QVariantMap props;
    props.insert(u"app"_s, QVariant::fromValue<QObject*>(this));
    props.insert(u"screenshotTypesModel"_s, QVariant::fromValue<QObject*>(model));
    create(QStringLiteral("ScreenshotDialog"), props);
}

QImage ScreenshotDialog::image() const
{
    return m_image;
}

void ScreenshotDialog::takeScreenshotNonInteractive()
{
    m_image = takeScreenshot(FullScreen, true, true);
    if (m_image.isNull()) {
        Q_EMIT failed();
    }
}

void ScreenshotDialog::takeScreenshotInteractive()
{
    if (!windowHandle()) {
        m_image = takeScreenshot(FullScreen, true, true);
    } else {
        const auto type = static_cast<ScreenshotType>(windowHandle()->property("screenshotType").toInt());
        const bool includeCursor = windowHandle()->property("withCursor").toBool();
        const bool includeDecorations = windowHandle()->property("withBorders").toBool();
        m_image = takeScreenshot(type, includeCursor, includeDecorations);
        if (!m_image.isNull()) {
            const QString tempPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + u"/xdg-desktop-portal-screenshot-preview.png"_s;
            m_image.save(tempPath, "PNG");
            windowHandle()->setProperty("screenshotImage", QUrl::fromLocalFile(tempPath));
        }
    }
    if (m_image.isNull()) {
        Q_EMIT failed();
    }
}

QImage ScreenshotDialog::takeScreenshot(ScreenshotType type, bool includeCursor, bool includeDecorations) const
{
    if (!m_capture) {
        return {};
    }
    switch (type) {
    case FullScreen:
    case CurrentScreen:
        return m_capture->captureWorkspace(includeCursor);
    case ActiveWindow:
        return m_capture->captureActiveWindow(includeCursor, includeDecorations);
    }
    return {};
}

#include "moc_screenshotdialog.cpp"
