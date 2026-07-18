/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_DIALOG_H
#define XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_DIALOG_H

#include "quickdialog.h"
#include <QImage>
#include <QObject>

class ScreenshotCapture : public QObject {
    Q_OBJECT
public:
    explicit ScreenshotCapture(QObject* parent = nullptr);
    ~ScreenshotCapture() override;

    virtual QImage captureWorkspace(bool includeCursor);
    virtual QImage captureActiveWindow(bool includeCursor, bool includeDecorations);
};

class ScreenshotDialog : public QuickDialog
{
    Q_OBJECT
public:
    explicit ScreenshotDialog(QObject *parent = nullptr);
    explicit ScreenshotDialog(ScreenshotCapture* capture, QObject* parent = nullptr);

    enum Flags {
        Borders = 1,
        Cursor = 1 << 1,
    };
    Q_ENUM(Flags)
    enum ScreenshotType {
        FullScreen,
        CurrentScreen,
        ActiveWindow,
    };
    Q_ENUM(ScreenshotType)

    QImage image() const;
    void takeScreenshotNonInteractive();

public Q_SLOTS:
    void takeScreenshotInteractive();

Q_SIGNALS:
    void failed();

private:
    QImage takeScreenshot(ScreenshotType type, bool includeCursor, bool includeDecorations) const;
    ScreenshotCapture* m_capture = nullptr;
    QImage m_image;
};

#endif // XDG_DESKTOP_PORTAL_KDE_SCREENSHOT_DIALOG_H
