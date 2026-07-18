/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 * SPDX-FileCopyrightText: 2022 Harald Sitter <sitter@kde.org>
 */

#include "remotedesktop.h"
#include "remotedesktop_debug.h"
#include "remotedesktopdialog.h"
#include "request.h"
#include "restoredata.h"
#include "session.h"
#include "utils.h"
#include "x11/x11controller.h"
#include "x11/x11input.h"
#include "x11/xeismounter.h"

#include <KLocalizedString>
#include <KNotification>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QGuiApplication>

#include "permission_store.h"

using namespace Qt::StringLiterals;

namespace {
bool isAppMegaAuthorized(const QString& app_id)
{
    qDBusRegisterMetaType<AppIdPermissionsMap>();
    OrgFreedesktopImplPortalPermissionStoreInterface permissionStore(u"org.freedesktop.impl.portal.PermissionStore"_s,
        u"/org/freedesktop/impl/portal/PermissionStore"_s,
        QDBusConnection::sessionBus());
    permissionStore.setTimeout(1000);
    QDBusVariant data;
    auto reply = permissionStore.Lookup(u"kde-authorized"_s, u"remote-desktop"_s, data);
    if (reply.isValid()) {
        auto appIdPermissions = reply.value();
        if (!appIdPermissions.contains(app_id)) {
            return false;
        }
        auto permissions = appIdPermissions.value(app_id);
        if (permissions.contains("yes"_L1)) {
            return true;
        }
    }
    return false;
}
} // namespace

RemoteDesktopPortal::RemoteDesktopPortal(QObject* parent, X11Controller* controller)
    : QDBusAbstractAdaptor(parent)
    , m_controller(controller)
    , m_input(new X11Input(parent))
{
}

RemoteDesktopPortal::~RemoteDesktopPortal() = default;

uint RemoteDesktopPortal::AvailableDeviceTypes() const
{
    // X11/XTEST provides keyboard and pointer support only.
    uint devices = None;
    if (m_input->isAvailable()) {
        devices |= Keyboard | Pointer;
    }
    return devices;
}

uint RemoteDesktopPortal::CreateSession(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QVariantMap& options,
    QVariantMap& results)
{
    Q_UNUSED(results);
    qCDebug(XdgDesktopPortalKdeRemoteDesktop) << "CreateSession called";
    Q_UNUSED(handle)

    RemoteDesktopSession* session = new RemoteDesktopSession(this, app_id, session_handle.path(), m_controller);
    if (!session->isValid()) {
        delete session;
        return PortalResponse::OtherError;
    }
    connect(session, &Session::closed, this, [this, path = session_handle.path()] {
        if (auto* eis = m_eisBySession.take(path))
            eis->deleteLater();
    });

    Q_UNUSED(app_id)
    Q_UNUSED(options)

    return PortalResponse::Success;
}

uint RemoteDesktopPortal::SelectDevices(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QVariantMap& options,
    QVariantMap& results)
{
    Q_UNUSED(results);
    Q_UNUSED(handle)
    qCDebug(XdgDesktopPortalKdeRemoteDesktop) << "SelectDevices called";

    const auto types = static_cast<RemoteDesktopPortal::DeviceTypes>(options.value(QStringLiteral("types")).toUInt());
    if (types == None) {
        return PortalResponse::OtherError;
    }
    if (types & TouchScreen) {
        qCWarning(XdgDesktopPortalKdeRemoteDesktop) << "TouchScreen requested but not supported by X11 backend";
        return PortalResponse::OtherError;
    }
    if ((types & DeviceTypes(AvailableDeviceTypes())) != types) {
        qCWarning(XdgDesktopPortalKdeRemoteDesktop) << "Requested unavailable input device type" << types;
        return PortalResponse::OtherError;
    }

    RemoteDesktopSession* session = Session::getSession<RemoteDesktopSession>(session_handle.path());
    if (!session) {
        return PortalResponse::OtherError;
    }

    session->setDeviceTypes(types);
    session->setPersistMode(ScreenCastPortal::PersistMode(options.value(QStringLiteral("persist_mode")).toUInt()));
    session->setRestoreData(options.value(QStringLiteral("restore_data")));

    Q_UNUSED(app_id)
    return PortalResponse::Success;
}

std::pair<PortalResponse::Response, QVariantMap> continueStart(RemoteDesktopSession* session)
{
    QVariantMap results;
    if (session->screenSharingEnabled()) {
        std::vector<std::unique_ptr<ScreencastingStream>> streams;
        auto stream = std::make_unique<ScreencastingStream>(nullptr);
        const QRect geometry = session->controller() ? session->controller()->rootGeometry() : QRect{};
        stream->setGeometry(geometry);
        stream->setMetaData({{u"position"_s, geometry.topLeft()}, {u"size"_s, geometry.size()}, {u"source_type"_s, 1}});
        X11Controller* controller = session->controller();
        const bool includeCursor = session->cursorMode() == ScreenCastPortal::Embedded;
        if (!controller || !stream->start([controller, includeCursor]() -> PipeWireStream::FrameProviderResult {
                if (!controller->isReady()) {
                    return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 capture controller is not ready"));
                }
                QImage image = controller->grabWorkspace(includeCursor);
                if (image.isNull())
                    return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 workspace capture failed"));
                image = image.convertToFormat(QImage::Format_RGBX8888);
                PipeWireStream::Frame frame;
                frame.data = QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
                frame.size = image.size();
                frame.stride = image.bytesPerLine();
                frame.format = PipeWireStream::PixelFormat::RGBx;
                return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
            })) {
            return {PortalResponse::OtherError, {}};
        }
        streams.push_back(std::move(stream));
        QList<std::pair<uint, QVariantMap>> dbusResultForStreams;
        std::ranges::transform(streams, std::back_inserter(dbusResultForStreams), [](const std::unique_ptr<ScreencastingStream>& stream) {
            return std::pair{stream->nodeid(), stream->metaData()};
        });
        results.insert(QStringLiteral("streams"), QVariant::fromValue(dbusResultForStreams));
        session->setStreams(std::move(streams));
    }
    session->acquireStreamingInput();
    session->setStarted(true);
    if (session->controller()) {
        session->controller()->beginRemoteDesktopSession();
        QObject::connect(session, &Session::closed, session->controller(), [controller = session->controller()] {
            controller->endRemoteDesktopSession();
        });
    }

    results.insert(QStringLiteral("devices"), QVariant::fromValue<uint>(session->deviceTypes()));
    results.insert(QStringLiteral("clipboard_enabled"), session->clipboardEnabled());
    results.insert(u"persist_mode"_s, quint32(session->persistMode()));
    if (session->persistMode() != ScreenCastPortal::NoPersist) {
        const RestoreData restoreData = {u"KDE"_s,
            RestoreData::currentRestoreDataVersion(),
            QVariantMap{{u"screenShareEnabled"_s, session->screenSharingEnabled()},
                {u"devices"_s, static_cast<quint32>(session->deviceTypes())},
                {u"clipboardEnabled"_s, session->clipboardEnabled()}}};
        results.insert(u"restore_data"_s, QVariant::fromValue<RestoreData>(restoreData));
    }
    return {PortalResponse::Success, results};
}

void RemoteDesktopPortal::Start(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QString& parent_window,
    const QVariantMap& options,
    const QDBusMessage& message,
    uint& replyResponse,
    QVariantMap& replyResults)
{
    qCDebug(XdgDesktopPortalKdeRemoteDesktop) << "Start called";

    Q_UNUSED(options)
    Q_UNUSED(message)

    RemoteDesktopSession* session = Session::getSession<RemoteDesktopSession>(session_handle.path());
    if (!session || session->started() || (session->deviceTypes() == None && !session->screenSharingEnabled())) {
        replyResponse = PortalResponse::OtherError;
        return;
    }

    if (QGuiApplication::screens().isEmpty()) {
        replyResponse = PortalResponse::OtherError;
        return;
    }

    if (isAppMegaAuthorized(app_id)) {
        auto notification = new KNotification(QStringLiteral("remotedesktopstarted"), KNotification::CloseOnTimeout);
        notification->setTitle(i18nc("title of notification about input systems taken over", "Remote control session started"));
        notification->setText(RemoteDesktopDialog::buildNotificationDescription(app_id, session->deviceTypes(), session->screenSharingEnabled()));
        notification->setIconName(QStringLiteral("krfb"));
        notification->sendEvent();
    } else {
        auto remoteDesktopDialog = new RemoteDesktopDialog(app_id, session->deviceTypes(), session->screenSharingEnabled(), session->persistMode());
        Utils::setParentWindow(remoteDesktopDialog->windowHandle(), parent_window);
        Request::makeClosableDialogRequestWithSession(handle, remoteDesktopDialog, session);
        delayReply(message, remoteDesktopDialog, this, [session, remoteDesktopDialog](DialogResult dialogResult) {
            auto response = PortalResponse::fromDialogResult(dialogResult);
            QVariantMap results;
            if (dialogResult == DialogResult::Accepted) {
                if (!remoteDesktopDialog->allowRestore()) {
                    session->setPersistMode(ScreenCastPortal::PersistMode::NoPersist);
                }
                std::tie(response, results) = continueStart(session);
            }
            return QVariantList{response, results};
        });
        return;
    }

    std::tie(replyResponse, replyResults) = continueStart(session);
}

void RemoteDesktopPortal::NotifyPointerMotion(const QDBusObjectPath& session_handle, const QVariantMap& options, double dx, double dy)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Pointer))
        m_input->injectPointerMotion(QSizeF(dx, dy));
}

void RemoteDesktopPortal::NotifyPointerMotionAbsolute(const QDBusObjectPath& session_handle, const QVariantMap& options, uint stream, double x, double y)
{
    Q_UNUSED(options)
    auto* session = validSession(session_handle, Pointer);
    if (!session)
        return;
    QRect geometry = session->controller() ? session->controller()->rootGeometry() : QRect{};
    for (const auto& candidate : session->streams()) {
        if (candidate->nodeid() == stream)
            geometry = candidate->geometry();
    }
    m_input->injectPointerMotionAbsolute(geometry, geometry.topLeft() + QPointF(x, y));
}

void RemoteDesktopPortal::NotifyPointerButton(const QDBusObjectPath& session_handle, const QVariantMap& options, int button, uint state)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Pointer))
        m_input->injectPointerButton(button, state != 0);
}

void RemoteDesktopPortal::NotifyPointerAxis(const QDBusObjectPath& session_handle, const QVariantMap& options, double dx, double dy)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Pointer))
        m_input->injectPointerAxis(dx, dy);
}

void RemoteDesktopPortal::NotifyPointerAxisDiscrete(const QDBusObjectPath& session_handle, const QVariantMap& options, uint axis, int steps)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Pointer))
        m_input->injectPointerAxisDiscrete(axis == 0 ? Qt::Vertical : Qt::Horizontal, steps);
}

void RemoteDesktopPortal::NotifyKeyboardKeysym(const QDBusObjectPath& session_handle, const QVariantMap& options, int keysym, uint state)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Keyboard))
        m_input->injectKeySym(keysym, state != 0);
}

void RemoteDesktopPortal::NotifyKeyboardKeycode(const QDBusObjectPath& session_handle, const QVariantMap& options, int keycode, uint state)
{
    Q_UNUSED(options)
    if (validSession(session_handle, Keyboard))
        m_input->injectKeyCode(keycode, state != 0);
}

void RemoteDesktopPortal::NotifyTouchDown(const QDBusObjectPath& session_handle, const QVariantMap& options, uint stream, uint slot, double x, double y)
{
    Q_UNUSED(session_handle)
    Q_UNUSED(options)
    Q_UNUSED(stream)
    Q_UNUSED(slot)
    Q_UNUSED(x)
    Q_UNUSED(y)
}

void RemoteDesktopPortal::NotifyTouchMotion(const QDBusObjectPath& session_handle, const QVariantMap& options, uint stream, uint slot, double x, double y)
{
    Q_UNUSED(session_handle)
    Q_UNUSED(options)
    Q_UNUSED(stream)
    Q_UNUSED(slot)
    Q_UNUSED(x)
    Q_UNUSED(y)
}

void RemoteDesktopPortal::NotifyTouchUp(const QDBusObjectPath& session_handle, const QVariantMap& options, uint slot)
{
    Q_UNUSED(session_handle)
    Q_UNUSED(options)
    Q_UNUSED(slot)
}

QDBusUnixFileDescriptor
RemoteDesktopPortal::ConnectToEIS(const QDBusObjectPath& session_handle, const QString& app_id, const QVariantMap& options, const QDBusMessage& message)
{
    Q_UNUSED(app_id)
    Q_UNUSED(options)
    auto* session = Session::getSession<RemoteDesktopSession>(session_handle.path());
    if (!session || !session->started() || session->deviceTypes() == None) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::InvalidArgs, u"Invalid or unstarted session"_s));
        return {};
    }
    auto* eis = eisForSession(session_handle.path());
    return eis ? eis->attachReceiver() : QDBusUnixFileDescriptor{};
}

XEisMounter* RemoteDesktopPortal::eisForSession(const QString& sessionPath)
{
    if (auto* existing = m_eisBySession.value(sessionPath))
        return existing;
    auto* eis = new XEisMounter(this);
    if (!eis->isValid()) {
        eis->deleteLater();
        return nullptr;
    }
    QList<QRect> regions;
    auto* session = Session::getSession<RemoteDesktopSession>(sessionPath);
    if (!session) {
        eis->deleteLater();
        return nullptr;
    }
    eis->setCapabilities(session->deviceTypes().testFlag(Pointer), session->deviceTypes().testFlag(Keyboard));
    if (m_controller) {
        for (const auto& output : m_controller->outputs())
            regions.push_back(output.nativeGeometry);
    }
    eis->setRegions(regions);
    connect(eis, &XEisMounter::pointerMotionReceived, m_input, [this, sessionPath](int, const QSizeF& delta) { if (validSession(QDBusObjectPath(sessionPath), Pointer)) m_input->injectPointerMotion(delta); });
    connect(eis, &XEisMounter::pointerMotionAbsoluteReceived, m_input, [this, sessionPath](int, const QPointF& position) { if (validSession(QDBusObjectPath(sessionPath), Pointer)) m_input->injectPointerMotionAbsolute(m_controller ? m_controller->rootGeometry() : QRect{}, position); });
    connect(eis, &XEisMounter::pointerButtonReceived, m_input, [this, sessionPath](int, uint button, bool pressed) { if (validSession(QDBusObjectPath(sessionPath), Pointer)) m_input->injectPointerButton(int(button), pressed); });
    connect(eis, &XEisMounter::pointerAxisReceived, m_input, [this, sessionPath](int, const QPointF& delta) { if (validSession(QDBusObjectPath(sessionPath), Pointer)) m_input->injectPointerAxis(delta.x(), delta.y()); });
    connect(eis, &XEisMounter::pointerAxisDiscreteReceived, m_input, [this, sessionPath](int, const QPoint& delta) {
        if (!validSession(QDBusObjectPath(sessionPath), Pointer))
            return;
        if (delta.x())
            m_input->injectPointerAxisDiscrete(Qt::Horizontal, delta.x() / 120);
        if (delta.y())
            m_input->injectPointerAxisDiscrete(Qt::Vertical, delta.y() / 120);
    });
    connect(eis, &XEisMounter::keyReceived, m_input, [this, sessionPath](int, uint key, bool pressed) { if (validSession(QDBusObjectPath(sessionPath), Keyboard)) m_input->injectKeyCode(int(key), pressed); });
    connect(eis, &XEisMounter::keySymReceived, m_input, [this, sessionPath](int, uint key, bool pressed) { if (validSession(QDBusObjectPath(sessionPath), Keyboard)) m_input->injectKeySym(int(key), pressed); });
    m_eisBySession.insert(sessionPath, eis);
    return eis;
}

RemoteDesktopSession* RemoteDesktopPortal::validSession(const QDBusObjectPath& handle, DeviceType capability) const
{
    auto* session = Session::getSession<RemoteDesktopSession>(handle.path());
    if (!session || !session->started() || !session->deviceTypes().testFlag(capability))
        return nullptr;
    if (m_controller && m_controller->isInputCaptureActive())
        return nullptr;
    return session;
}

RemoteDesktopSession::RemoteDesktopSession(QObject* parent, const QString& appId, const QString& path, X11Controller* controller)
    : ScreenCastSession(parent, appId, path, QStringLiteral("krfb"), controller)
    , m_screenSharingEnabled(false)
    , m_clipboardEnabled(false)
{
}

RemoteDesktopSession::~RemoteDesktopSession() = default;

RemoteDesktopPortal::DeviceTypes RemoteDesktopSession::deviceTypes() const
{
    return m_deviceTypes;
}

void RemoteDesktopSession::setDeviceTypes(RemoteDesktopPortal::DeviceTypes deviceTypes)
{
    m_deviceTypes = deviceTypes;
}

bool RemoteDesktopSession::screenSharingEnabled() const
{
    return m_screenSharingEnabled;
}

void RemoteDesktopSession::setScreenSharingEnabled(bool enabled)
{
    m_screenSharingEnabled = enabled;
}

bool RemoteDesktopSession::clipboardEnabled() const
{
    return m_clipboardEnabled;
}

void RemoteDesktopSession::setClipboardEnabled(bool enabled)
{
    m_clipboardEnabled = enabled;
}

void RemoteDesktopSession::setEisCookie(int cookie)
{
    m_cookie = cookie;
}

int RemoteDesktopSession::eisCookie() const
{
    return m_cookie;
}

void RemoteDesktopSession::acquireStreamingInput()
{
    m_acquired = true;
}

void RemoteDesktopSession::refreshDescription()
{
    QObject* item = m_item;
    Q_UNUSED(item);
    setDescription(RemoteDesktopDialog::buildNotificationDescription(m_appId, deviceTypes(), screenSharingEnabled()));
}

#include "moc_remotedesktop.cpp"
