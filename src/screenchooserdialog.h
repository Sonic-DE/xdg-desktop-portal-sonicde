/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_SCREENCHOOSER_DIALOG_H
#define XDG_DESKTOP_PORTAL_KDE_SCREENCHOOSER_DIALOG_H

#include "outputsmodel.h"
#include "quickdialog.h"
#include "screencast.h"
#include "x11/x11types.h"
#include <QAbstractListModel>
#include <QRect>

class X11Controller;

class ScreenChooserDialog : public QuickDialog
{
    Q_OBJECT
public:
    ScreenChooserDialog(const QString& appName, bool multiple, ScreenCastPortal::SourceTypes types, X11Controller* controller = nullptr);
    ~ScreenChooserDialog() override;

    QList<Output> selectedOutputs() const;
    QList<X11Types::WindowDescriptor> selectedWindows() const;
    bool allowRestore() const;
    QRect selectedRegion() const;

public Q_SLOTS:
    void accept() override;

Q_SIGNALS:
    void clearSelection();
    void finished(DialogResult result);

private:
    void setRegion(const QRect region);

    QRect m_region;
};

#endif // XDG_DESKTOP_PORTAL_KDE_SCREENCHOOSER_DIALOG_H
