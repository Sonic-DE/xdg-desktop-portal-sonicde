/*
 * SPDX-FileCopyrightText: 2016 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef XDG_DESKTOP_PORTAL_KDE_PRINT_H
#define XDG_DESKTOP_PORTAL_KDE_PRINT_H

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QStringList>

class PrintPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Print")
public:
    explicit PrintPortal(QObject *parent);
    ~PrintPortal() override;

    struct PrintCommand {
        QString program;
        QStringList arguments;
        QString error;
    };

    static PrintCommand commandForPdfPrint(const QString& printerName, const QString& title, const QString& filePath, const QVariantMap& options);

public Q_SLOTS:
    uint Print(const QDBusObjectPath &handle,
               const QString &app_id,
               const QString &parent_window,
               const QString &title,
               const QDBusUnixFileDescriptor &fd,
               const QVariantMap &options,
               QVariantMap &results);

    void PreparePrint(const QDBusObjectPath& handle,
        const QString& app_id,
        const QString& parent_window,
        const QString& title,
        const QVariantMap& settings,
        const QVariantMap& page_setup,
        const QVariantMap& options,
        const QDBusMessage& message,
        uint& replyResponse,
        QVariantMap& replyResults);
};

#endif
