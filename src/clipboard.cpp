/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "clipboard.h"
#include "clipboard_debug.h"
#include "inputcapture.h"
#include "remotedesktop.h"
#include "session.h"
#include "x11/x11clipboard.h"

#include <QDBusConnection>
#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

bool isClipboardEnabledSession(const QDBusObjectPath &path)
{
    if (auto remoteDesktopSession = Session::getSession<RemoteDesktopSession>(path.path())) {
        return remoteDesktopSession->started() && remoteDesktopSession->clipboardEnabled();
    }
    if (auto inputCaptureSession = Session::getSession<InputCaptureSession>(path.path())) {
        return inputCaptureSession->started() && inputCaptureSession->clipboardEnabled();
    }
    return false;
}

ClipboardPortal::ClipboardPortal(QObject* parent, X11Clipboard* backend)
    : QDBusAbstractAdaptor(parent)
    , m_backend(backend ? backend : new X11Clipboard(parent))
{
    QString error;
    if (!m_backend->initialize(&error)) {
        qCWarning(XdgDesktopPortalKdeClipboard) << "Failed to initialize X11 clipboard:" << error;
    }
    connect(m_backend, &X11Clipboard::selectionOwnerChanged, this, [this](const QDBusObjectPath& session, const QStringList& mimeTypes, bool isOwner) {
        QVariantMap options;
        options.insert(u"mime_types"_s, mimeTypes);
        options.insert(u"session_is_owner"_s, isOwner);
        Q_EMIT SelectionOwnerChanged(session, options);
    });
}

ClipboardPortal::~ClipboardPortal() = default;

QVariant ClipboardPortal::fetchData(Session* session, const QString& mimetype)
{
    if (!session || !m_backend) {
        return {};
    }
    const QDBusObjectPath handle(session->handle());
    if (!isClipboardEnabledSession(handle))
        return {};
    activateBackendSession(handle);
    QString error;
    const auto fd = m_backend->requestRead(handle, mimetype, &error);
    return fd.isValid() ? QVariant::fromValue(fd) : QVariant{};
}

void ClipboardPortal::RequestClipboard(const QDBusObjectPath &session_handle, const QVariantMap &options)
{
    qCDebug(XdgDesktopPortalKdeClipboard) << "RequestClipboard called with parameters:";
    qCDebug(XdgDesktopPortalKdeClipboard) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeClipboard) << "    options: " << options;

    Q_UNUSED(options)

    auto session = Session::getSession(session_handle.path());
    if (!session) {
        qCWarning(XdgDesktopPortalKdeClipboard) << "Tried enabling clipboard on unknown session" << session_handle.path();
        return;
    }

    if (auto remoteDesktopSession = qobject_cast<RemoteDesktopSession *>(session)) {
        remoteDesktopSession->setClipboardEnabled(true);
    } else if (auto inputCaptureSession = qobject_cast<InputCaptureSession *>(session)) {
        inputCaptureSession->setClipboardEnabled(true);
    } else {
        qCWarning(XdgDesktopPortalKdeClipboard) << "Tried enabling clipboard on unsupported session" << session_handle.path();
        return;
    }
    connect(session, &Session::closed, this, [this, session_handle] {
        m_backend->setSessionActive(session_handle, false);
        m_backend->authorizeSession(session_handle, false);
        m_sessionMime.remove(session_handle.path());
        for (auto it = m_serialSession.begin(); it != m_serialSession.end();) {
            if (it.value() == session_handle.path()) {
                m_serialMime.remove(it.key());
                it = m_serialSession.erase(it);
            } else {
                ++it;
            }
        }
    });
}

void ClipboardPortal::activateBackendSession(const QDBusObjectPath& session)
{
    m_backend->authorizeSession(session, true);
    m_backend->setSessionActive(session, true);
}

QDBusUnixFileDescriptor ClipboardPortal::SelectionRead(const QDBusObjectPath &session_handle, const QString &mime_type, const QDBusMessage &message)
{
    qCDebug(XdgDesktopPortalKdeClipboard) << "SelectionRead called with parameters:";
    qCDebug(XdgDesktopPortalKdeClipboard) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeClipboard) << "    mime_type: " << mime_type;

    if (!isClipboardEnabledSession(session_handle)) {
        message.setDelayedReply(true);
        QDBusMessage error = message.createErrorReply(QDBusError::InvalidArgs, u"Not a clipboard enabled session"_s);
        QDBusConnection::sessionBus().send(error);
        return {};
    }

    activateBackendSession(session_handle);
    QString errorString;
    auto fd = m_backend->requestRead(session_handle, mime_type, &errorString);
    if (!fd.isValid()) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::Failed, errorString));
    }
    return fd;
}

void ClipboardPortal::SetSelection(const QDBusObjectPath &session_handle, const QVariantMap &options)
{
    qCDebug(XdgDesktopPortalKdeClipboard) << "SetSelection called with parameters:";
    qCDebug(XdgDesktopPortalKdeClipboard) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeClipboard) << "    options: " << options;

    if (!isClipboardEnabledSession(session_handle)) {
        qCWarning(XdgDesktopPortalKdeClipboard) << "Not a clipboard enabled session" << session_handle.path();
        return;
    }
    activateBackendSession(session_handle);
    const QStringList mimeTypes = options.value(u"mime_types"_s).toStringList();
    if (mimeTypes.isEmpty()) {
        qCWarning(XdgDesktopPortalKdeClipboard) << "SetSelection without mime_types";
        return;
    }
    const QString mime = mimeTypes.constFirst();
    m_sessionMime.insert(session_handle.path(), mime);
    uint serial = m_nextSerial++;
    if (serial == 0) {
        serial = m_nextSerial++;
    }
    m_serialMime.insert(serial, mime);
    m_serialSession.insert(serial, session_handle.path());
    Q_EMIT SelectionTransfer(session_handle, mime, serial);
}

QDBusUnixFileDescriptor ClipboardPortal::SelectionWrite(const QDBusObjectPath &session_handle, uint serial, const QDBusMessage &message)
{
    qCDebug(XdgDesktopPortalKdeClipboard) << "SelectionWrite called with parameters:";
    qCDebug(XdgDesktopPortalKdeClipboard) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeClipboard) << "    serial: " << serial;

    if (!isClipboardEnabledSession(session_handle)) {
        message.setDelayedReply(true);
        QDBusMessage error = message.createErrorReply(QDBusError::InvalidArgs, u"Not a clipboard enabled session"_s);
        QDBusConnection::sessionBus().send(error);
        return {};
    }

    activateBackendSession(session_handle);
    if (m_serialSession.value(serial) != session_handle.path()) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::InvalidArgs, u"Unknown clipboard transfer serial"_s));
        return {};
    }
    const QString mime = m_serialMime.value(serial);
    QString errorString;
    auto fd = m_backend->beginWrite(session_handle, mime, serial, &errorString);
    if (!fd.isValid()) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::Failed, errorString));
    }
    return fd;
}

void ClipboardPortal::SelectionWriteDone(const QDBusObjectPath &session_handle, uint serial, bool success, const QDBusMessage &message)
{
    qCDebug(XdgDesktopPortalKdeClipboard) << "SelectionWriteDone called with parameters:";
    qCDebug(XdgDesktopPortalKdeClipboard) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeClipboard) << "    serial: " << serial;
    qCDebug(XdgDesktopPortalKdeClipboard) << "    success: " << success;

    if (!isClipboardEnabledSession(session_handle)) {
        message.setDelayedReply(true);
        QDBusMessage error = message.createErrorReply(QDBusError::InvalidArgs, u"Not a clipboard enabled session"_s);
        QDBusConnection::sessionBus().send(error);
        return;
    }
    activateBackendSession(session_handle);
    if (m_serialSession.value(serial) != session_handle.path()) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::InvalidArgs, u"Unknown clipboard transfer serial"_s));
        return;
    }
    QString errorString;
    if (!m_backend->completeWrite(session_handle, serial, success, &errorString)) {
        qCWarning(XdgDesktopPortalKdeClipboard) << "SelectionWriteDone failed:" << errorString;
    } else {
        m_serialMime.remove(serial);
        m_serialSession.remove(serial);
    }
}

#include "moc_clipboard.cpp"
