/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QHash>

class Session;
class X11Clipboard;

class ClipboardPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Clipboard")
    Q_PROPERTY(uint version MEMBER version CONSTANT)
public:
    explicit ClipboardPortal(QObject* parent, X11Clipboard* backend = nullptr);
    ~ClipboardPortal() override;

    QVariant fetchData(Session *session, const QString &mimetype);

    static constexpr uint version = 1;

public Q_SLOTS:
    void RequestClipboard(const QDBusObjectPath &session_handle, const QVariantMap &options);
    void SetSelection(const QDBusObjectPath &session_handle, const QVariantMap &options);
    QDBusUnixFileDescriptor SelectionWrite(const QDBusObjectPath &session_handle, uint serial, const QDBusMessage &message);
    void SelectionWriteDone(const QDBusObjectPath &session_handle, uint serial, bool success, const QDBusMessage &message);
    QDBusUnixFileDescriptor SelectionRead(const QDBusObjectPath &session_handle, const QString &mime_type, const QDBusMessage &message);

Q_SIGNALS:
    void SelectionOwnerChanged(const QDBusObjectPath &session_handle, const QVariantMap &options);
    void SelectionTransfer(const QDBusObjectPath& session_handle, const QString& mimeType, uint serial);

private:
    void activateBackendSession(const QDBusObjectPath& session);
    X11Clipboard* m_backend = nullptr;
    QHash<QString, QString> m_sessionMime;
    QHash<uint, QString> m_serialMime;
    QHash<uint, QString> m_serialSession;
    uint m_nextSerial = 1;
};
