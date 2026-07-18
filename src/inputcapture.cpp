/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 * SPDX-FileCopyrightText: 2022 Harald Sitter <sitter@kde.org>
 * SPDX-FileCopyrightText: 2024 David Redondo <kde@david-redondo.de>
 */

#include "inputcapture.h"

#include "dbushelpers.h"
#include "inputcapture_debug.h"
#include "inputcapturebarrier.h"
#include "inputcapturedialog.h"
#include "request.h"
#include "restoredata.h"
#include "session.h"
#include "utils.h"
#include "x11/x11controller.h"
#include "x11/xeismounter.h"

#include <KLocalizedString>
#include <KNotification>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QGuiApplication>
#include <QScreen>
#include <QSocketNotifier>

#include <limits>
#include <memory>
#include <utility>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

using namespace Qt::StringLiterals;

QDBusArgument &operator<<(QDBusArgument &argument, const InputCapturePortal::zone &zone)
{
    argument.beginStructure();
    argument << zone.width << zone.height << zone.x_offset << zone.y_offset;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, InputCapturePortal::zone &zone)
{
    argument.beginStructure();
    argument >> zone.width >> zone.height >> zone.x_offset >> zone.y_offset;
    argument.endStructure();
    return argument;
}

class InputCaptureBackend : public QObject {
public:
    explicit InputCaptureBackend(InputCapturePortal* portal, X11Controller* controller)
        : QObject(portal)
        , m_portal(portal)
        , m_controller(controller)
    {
        const auto eisProbe = std::make_unique<XEisMounter>();
        m_eisValid = eisProbe->isValid();
        int screenNumber = 0;
        m_connection = xcb_connect(nullptr, &screenNumber);
        if (!m_connection || xcb_connection_has_error(m_connection))
            return;
        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
        for (int i = 0; i < screenNumber; ++i)
            xcb_screen_next(&iterator);
        m_screen = iterator.data;
        const auto* xi = xcb_get_extension_data(m_connection, &xcb_input_id);
        const auto* fixes = xcb_get_extension_data(m_connection, &xcb_xfixes_id);
        if (!m_screen || !xi || !xi->present || !fixes || !fixes->present)
            return;
        auto* xiVersion = xcb_input_xi_query_version_reply(m_connection, xcb_input_xi_query_version(m_connection, 2, 3), nullptr);
        auto* fixesVersion = xcb_xfixes_query_version_reply(m_connection, xcb_xfixes_query_version(m_connection, 5, 0), nullptr);
        const bool versionsSupported = xiVersion && (xiVersion->major_version > 2 || (xiVersion->major_version == 2 && xiVersion->minor_version >= 3))
            && fixesVersion && fixesVersion->major_version >= 5;
        free(xiVersion);
        free(fixesVersion);
        if (!versionsSupported)
            return;
        m_xiOpcode = xi->major_opcode;
        struct {
            xcb_input_event_mask_t header;
            uint32_t mask;
        } eventMask{};
        eventMask.header.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
        eventMask.header.mask_len = 1;
        eventMask.mask = XCB_INPUT_XI_EVENT_MASK_BARRIER_HIT | XCB_INPUT_XI_EVENT_MASK_BARRIER_LEAVE;
        xcb_input_xi_select_events(m_connection, m_screen->root, 1, &eventMask.header);
        xcb_flush(m_connection);
        m_notifier = new QSocketNotifier(xcb_get_file_descriptor(m_connection), QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
        m_valid = true;
    }

    ~InputCaptureBackend() override
    {
        clearAll();
        if (m_connection)
            xcb_disconnect(m_connection);
    }

    bool valid() const
    {
        return m_valid && m_eisValid;
    }
    QDBusUnixFileDescriptor connectClient(const QString& sessionPath)
    {
        XEisMounter* eis = m_sessionEis.value(sessionPath);
        if (!eis) {
            eis = new XEisMounter(this);
            if (!eis->isValid()) {
                eis->deleteLater();
                return {};
            }
            QList<QRect> regions;
            if (m_controller) {
                for (const auto& output : m_controller->outputs())
                    regions.push_back(output.nativeGeometry);
            }
            eis->setRegions(regions);
            if (auto* session = Session::getSession<InputCaptureSession>(sessionPath)) {
                eis->setCapabilities(session->capabilities().testFlag(InputCapturePortal::Pointer),
                    session->capabilities().testFlag(InputCapturePortal::Keyboard));
            }
            m_sessionEis.insert(sessionPath, eis);
        }
        return eis->attachSender();
    }

    QList<uint> setBarriers(const QString& sessionPath, const QList<std::tuple<uint, QPoint, QPoint>>& barriers)
    {
        removeBarriers(sessionPath);
        QList<uint> failed;
        for (const auto& [portalId, start, end] : barriers) {
            constexpr int minimum = std::numeric_limits<int16_t>::min();
            constexpr int maximum = std::numeric_limits<int16_t>::max();
            if (start.x() < minimum || start.y() < minimum || end.x() < minimum || end.y() < minimum
                || start.x() > maximum || start.y() > maximum || end.x() > maximum || end.y() > maximum) {
                failed.push_back(portalId);
                continue;
            }
            const xcb_xfixes_barrier_t xid = xcb_generate_id(m_connection);
            auto cookie = xcb_xfixes_create_pointer_barrier_checked(m_connection, xid, m_screen->root,
                uint16_t(int16_t(start.x())), uint16_t(int16_t(start.y())), uint16_t(int16_t(end.x())), uint16_t(int16_t(end.y())), 0, 0, nullptr);
            if (auto* error = xcb_request_check(m_connection, cookie)) {
                free(error);
                failed.push_back(portalId);
                continue;
            }
            m_barriers.insert(xid, {sessionPath, portalId});
            m_sessionBarriers[sessionPath].push_back(xid);
        }
        xcb_flush(m_connection);
        return failed;
    }

    void enable(const QString& sessionPath)
    {
        m_enabled.insert(sessionPath);
    }
    void disable(const QString& sessionPath)
    {
        if (m_activeSession == sessionPath)
            deactivate();
        m_enabled.remove(sessionPath);
    }
    void closeSession(const QString& sessionPath)
    {
        disable(sessionPath);
        removeBarriers(sessionPath);
        if (auto* eis = m_sessionEis.take(sessionPath))
            eis->deleteLater();
        xcb_flush(m_connection);
    }
    bool release(uint activationId)
    {
        if (activationId != m_activationId || m_activeSession.isEmpty())
            return false;
        if (m_hitBarrier != XCB_NONE) {
            xcb_input_barrier_release_pointer_info_t info{};
            info.deviceid = m_hitDevice;
            info.barrier = m_hitBarrier;
            info.eventid = m_hitEvent;
            xcb_input_xi_barrier_release_pointer(m_connection, 1, &info);
        }
        deactivate();
        return true;
    }

private:
    struct Barrier {
        QString session;
        uint id = 0;
    };
    static double fp1616(xcb_input_fp1616_t value)
    {
        return double(value) / 65536.0;
    }
    void activate(const Barrier& barrier, const xcb_input_barrier_hit_event_t* hit)
    {
        if (!m_enabled.contains(barrier.session) || !m_activeSession.isEmpty())
            return;
        if (m_controller && m_controller->isRemoteDesktopActive())
            return;
        auto* session = Session::getSession<InputCaptureSession>(barrier.session);
        if (!session)
            return;
        const bool captureKeyboard = session->capabilities().testFlag(InputCapturePortal::Keyboard);
        const auto pointerReply = xcb_grab_pointer_reply(m_connection,
            xcb_grab_pointer(m_connection, false, m_screen->root,
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
            nullptr);
        xcb_grab_keyboard_reply_t* keyboardReply = captureKeyboard
            ? xcb_grab_keyboard_reply(m_connection,
                  xcb_grab_keyboard(m_connection, false, m_screen->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr)
            : nullptr;
        const bool grabbed = pointerReply && pointerReply->status == XCB_GRAB_STATUS_SUCCESS
            && (!captureKeyboard || (keyboardReply && keyboardReply->status == XCB_GRAB_STATUS_SUCCESS));
        free(pointerReply);
        free(keyboardReply);
        if (!grabbed) {
            xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
            xcb_ungrab_keyboard(m_connection, XCB_CURRENT_TIME);
            return;
        }
        m_activeSession = barrier.session;
        m_activationId = ++m_nextActivation;
        if (!m_activationId)
            m_activationId = ++m_nextActivation;
        m_hitBarrier = hit->barrier;
        m_hitDevice = hit->deviceid;
        m_hitEvent = hit->eventid;
        session->state = InputCapturePortal::State::Activated;
        if (m_controller)
            m_controller->setInputCaptureActive(true);
        QVariantMap options{{u"activation_id"_s, m_activationId}, {u"barrier_id"_s, barrier.id},
            {u"cursor_position"_s, QPointF(fp1616(hit->root_x), fp1616(hit->root_y))}};
        Q_EMIT m_portal->Activated(QDBusObjectPath(barrier.session), options);
    }

    void deactivate()
    {
        if (m_activeSession.isEmpty())
            return;
        const QString old = std::exchange(m_activeSession, {});
        if (auto* session = Session::getSession<InputCaptureSession>(old)) {
            session->state = InputCapturePortal::State::Deactivated;
        }
        xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(m_connection, XCB_CURRENT_TIME);
        xcb_flush(m_connection);
        if (auto* eis = m_sessionEis.value(old))
            eis->stopSending();
        if (m_controller)
            m_controller->setInputCaptureActive(false);
        Q_EMIT m_portal->Deactivated(QDBusObjectPath(old), {{u"activation_id"_s, m_activationId}});
        m_hitBarrier = XCB_NONE;
    }

    void drain()
    {
        while (auto* event = xcb_poll_for_event(m_connection)) {
            const uint8_t type = event->response_type & ~0x80;
            if (type == XCB_GE_GENERIC) {
                auto* ge = reinterpret_cast<xcb_ge_generic_event_t*>(event);
                if (ge->extension == m_xiOpcode && ge->event_type == XCB_INPUT_BARRIER_HIT) {
                    auto* hit = reinterpret_cast<xcb_input_barrier_hit_event_t*>(event);
                    if (m_barriers.contains(hit->barrier))
                        activate(m_barriers.value(hit->barrier), hit);
                }
            } else if (!m_activeSession.isEmpty()) {
                auto* session = Session::getSession<InputCaptureSession>(m_activeSession);
                const auto capabilities = session ? session->capabilities() : InputCapturePortal::Capabilities{};
                switch (type) {
                case XCB_MOTION_NOTIFY: {
                    if (!capabilities.testFlag(InputCapturePortal::Pointer))
                        break;
                    auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);
                    if (auto* eis = m_sessionEis.value(m_activeSession))
                        eis->sendPointerMotionAbsolute(QPointF(motion->root_x, motion->root_y));
                    break;
                }
                case XCB_BUTTON_PRESS:
                case XCB_BUTTON_RELEASE: {
                    if (!capabilities.testFlag(InputCapturePortal::Pointer))
                        break;
                    auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
                    if (auto* eis = m_sessionEis.value(m_activeSession)) {
                        if (type == XCB_BUTTON_PRESS && button->detail >= 4 && button->detail <= 7) {
                            const bool vertical = button->detail <= 5;
                            const int steps = (button->detail == 4 || button->detail == 6) ? -1 : 1;
                            eis->sendPointerAxisDiscrete(vertical ? Qt::Vertical : Qt::Horizontal, steps);
                        } else {
                            static const uint linuxButtons[] = {0, 272, 274, 273};
                            if (button->detail < std::size(linuxButtons))
                                eis->sendPointerButton(linuxButtons[button->detail], type == XCB_BUTTON_PRESS);
                        }
                    }
                    break;
                }
                case XCB_KEY_PRESS:
                case XCB_KEY_RELEASE: {
                    if (!capabilities.testFlag(InputCapturePortal::Keyboard))
                        break;
                    auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
                    if (key->detail >= 8) {
                        if (auto* eis = m_sessionEis.value(m_activeSession))
                            eis->sendKey(key->detail - 8, type == XCB_KEY_PRESS);
                    }
                    break;
                }
                }
            }
            free(event);
        }
    }

    void removeBarriers(const QString& session)
    {
        for (auto xid : m_sessionBarriers.take(session)) {
            xcb_xfixes_delete_pointer_barrier(m_connection, xid);
            m_barriers.remove(xid);
        }
    }
    void clearAll()
    {
        deactivate();
        for (auto xid : m_barriers.keys())
            xcb_xfixes_delete_pointer_barrier(m_connection, xid);
        m_barriers.clear();
        m_sessionBarriers.clear();
    }

    InputCapturePortal* m_portal;
    X11Controller* m_controller;
    bool m_eisValid = false;
    QHash<QString, XEisMounter*> m_sessionEis;
    xcb_connection_t* m_connection = nullptr;
    xcb_screen_t* m_screen = nullptr;
    QSocketNotifier* m_notifier = nullptr;
    uint8_t m_xiOpcode = 0;
    bool m_valid = false;
    QHash<xcb_xfixes_barrier_t, Barrier> m_barriers;
    QHash<QString, QList<xcb_xfixes_barrier_t>> m_sessionBarriers;
    QSet<QString> m_enabled;
    QString m_activeSession;
    uint m_nextActivation = 0;
    uint m_activationId = 0;
    xcb_xfixes_barrier_t m_hitBarrier = XCB_NONE;
    xcb_input_device_id_t m_hitDevice = 0;
    uint32_t m_hitEvent = 0;
};

InputCapturePortal::InputCapturePortal(QObject* parent, X11Controller* controller)
    : QDBusAbstractAdaptor(parent)
    , m_controller(controller)
    , m_backend(new InputCaptureBackend(this, controller))
{
    qDBusRegisterMetaType<zone>();
    qDBusRegisterMetaType<QList<zone>>();
    qDBusRegisterMetaType<QList<QMap<QString, QVariant>>>();
    qDBusRegisterMetaType<std::tuple<uint, QPoint, QPoint>>();
    qDBusRegisterMetaType<QList<std::tuple<uint, QPoint, QPoint>>>();
}

InputCapturePortal::~InputCapturePortal() = default;

uint InputCapturePortal::SupportedCapabilities() const
{
    return m_backend && m_backend->valid() ? Keyboard | Pointer : None;
}

uint InputCapturePortal::CreateSession(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QString& parent_window,
    const QVariantMap& options,
    const QDBusMessage& message,
    uint& replyResponse,
    QVariantMap& replyResults)
{
    Q_UNUSED(handle)
    Q_UNUSED(parent_window)
    Q_UNUSED(options)
    Q_UNUSED(message)
    Q_UNUSED(replyResults)

    auto* session = Session::getSession<InputCaptureSession>(CreateSession2(session_handle, app_id, options).value(QStringLiteral("session_id")).toString());
    if (!session || !session->isValid()) {
        replyResponse = PortalResponse::OtherError;
        return PortalResponse::OtherError;
    }
    return PortalResponse::Success;
}

QVariantMap InputCapturePortal::CreateSession2(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options)
{
    Q_UNUSED(options)

    auto* session = new InputCaptureSession(this, app_id, session_handle.path());
    const auto requested = Capabilities(options.value(u"capabilities"_s, SupportedCapabilities()).toUInt());
    const auto supported = Capabilities(SupportedCapabilities());
    session->setCapabilities(requested & supported);
    session->setPersistMode(PersistMode(options.value(u"persist_mode"_s, uint(PersistMode::None)).toUInt()));
    connect(session, &Session::closed, this, [this, path = session_handle.path()] { m_backend->closeSession(path); });
    if (!session->isValid() || session->capabilities() == None) {
        session->deleteLater();
        return {};
    }
    return {{u"session_id"_s, session_handle.path()}};
}

void InputCapturePortal::Start(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QString& parent_window,
    const QVariantMap& options,
    const QDBusMessage& message,
    uint& replyResponse,
    QVariantMap& replyResults)
{
    Q_UNUSED(options)

    auto* session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || session->started()) {
        replyResponse = PortalResponse::OtherError;
        return;
    }
    auto* dialog = new InputCaptureDialog(app_id, session->capabilities(), session->persistMode(), parent());
    Utils::setParentWindow(dialog->windowHandle(), parent_window);
    Request::makeClosableDialogRequestWithSession(handle, dialog, session);
    delayReply(message, dialog, this, [session, dialog](DialogResult result) {
        QVariantMap results;
        const auto response = PortalResponse::fromDialogResult(result);
        if (result == DialogResult::Accepted) {
            if (!dialog->allowRestore())
                session->setPersistMode(PersistMode::None);
            session->setStarted(true);
            results.insert(u"capabilities"_s, uint(session->capabilities()));
            results.insert(u"persist_mode"_s, uint(session->persistMode()));
            if (session->persistMode() != PersistMode::None) {
                const RestoreData restoreData{u"KDE"_s, RestoreData::currentRestoreDataVersion(),
                    QVariantMap{{u"capabilities"_s, uint(session->capabilities())}}};
                results.insert(u"restore_data"_s, QVariant::fromValue(restoreData));
            }
        }
        return QVariantList{response, results};
    });
    replyResponse = PortalResponse::Success;
    replyResults.clear();
}

uint InputCapturePortal::GetZones(const QDBusObjectPath &handle,
                                  const QDBusObjectPath &session_handle,
                                  const QString &app_id,
                                  const QVariantMap &options,
                                  QVariantMap &results)
{
    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(options)

    auto* session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started()) {
        return PortalResponse::OtherError;
    }
    Q_UNUSED(session)

    results.insert(u"zone_set"_s, m_zoneId);
    QList<zone> zones;
    for (const auto screen : qGuiApp->screens()) {
        zone z{
            static_cast<uint>(screen->geometry().width()),
            static_cast<uint>(screen->geometry().height()),
            screen->geometry().x(),
            screen->geometry().y(),
        };
        if (!zones.contains(z)) {
            zones.push_back(z);
        }
    }
    results.insert(u"zones"_s, QVariant::fromValue(zones));
    return PortalResponse::Success;
}

uint InputCapturePortal::SetPointerBarriers(const QDBusObjectPath &handle,
                                            const QDBusObjectPath &session_handle,
                                            const QString &app_id,
                                            const QVariantMap &options,
                                            const QList<QVariantMap> &barriers,
                                            uint zone_set,
                                            QVariantMap &results)
{
    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(options)

    auto* session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started()) {
        return PortalResponse::OtherError;
    }
    if (zone_set != m_zoneId) {
        return PortalResponse::OtherError;
    }
    QList<uint> failedBarriers;
    QList<std::tuple<uint, QPoint, QPoint>> validBarriers;

    QList<QRect> screenGeometries;
    for (const auto screen : qGuiApp->screens()) {
        const QRect geometry = screen->geometry();
        if (!screenGeometries.contains(geometry)) {
            screenGeometries.append(geometry);
        }
    }

    for (const auto &barrier : barriers) {
        const auto id = barrier.value(u"barrier_id"_s).toUInt();
        int x1;
        int y1;
        int x2;
        int y2;
        const auto position = barrier.value(u"position"_s).value<QDBusArgument>();
        position.beginStructure();
        position >> x1 >> y1 >> x2 >> y2;
        position.endStructure();

        if (id == 0) {
            failedBarriers.append(id);
            continue;
        }

        const auto barrierOrFailure = checkAndMakeBarrier(x1, y1, x2, y2, screenGeometries);
        if (auto reason = std::get_if<BarrierFailureReason>(&barrierOrFailure)) {
            Q_UNUSED(reason);
            failedBarriers.append(id);
        } else {
            const auto validated = std::get<1>(barrierOrFailure);
            session->addBarrier(id, validated);
            validBarriers.push_back({id, validated.first, validated.second});
        }
    }
    failedBarriers += m_backend->setBarriers(session_handle.path(), validBarriers);
    results.insert(u"failed_barriers"_s, QVariant::fromValue(failedBarriers));
    return PortalResponse::Success;
}

QDBusUnixFileDescriptor
InputCapturePortal::ConnectToEIS(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, const QDBusMessage &message)
{
    Q_UNUSED(app_id)
    Q_UNUSED(options)
    auto* session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started() || session->state != State::Disabled) {
        message.setDelayedReply(true);
        QDBusConnection::sessionBus().send(message.createErrorReply(QDBusError::InvalidArgs, u"Invalid InputCapture session state"_s));
        return {};
    }
    return m_backend->connectClient(session_handle.path());
}

uint InputCapturePortal::Enable(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(app_id)
    Q_UNUSED(options)
    Q_UNUSED(results)

    auto *session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started() || session->state != State::Disabled) {
        return PortalResponse::OtherError;
    }
    session->state = State::Deactivated;
    m_backend->enable(session_handle.path());
    return PortalResponse::Success;
}

uint InputCapturePortal::Disable(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(app_id)
    Q_UNUSED(options)
    Q_UNUSED(results)

    auto *session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started() || session->state == State::Disabled) {
        return PortalResponse::OtherError;
    }
    m_backend->disable(session_handle.path());
    session->state = State::Disabled;
    Q_EMIT Disabled(QDBusObjectPath(session->handle()), {});
    return PortalResponse::Success;
}

uint InputCapturePortal::Release(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(app_id)
    Q_UNUSED(results)

    auto *session = Session::getSession<InputCaptureSession>(session_handle.path());
    if (!session || !session->started() || session->state != State::Activated) {
        return PortalResponse::OtherError;
    }
    return m_backend->release(options.value(u"activation_id"_s).toUInt()) ? PortalResponse::Success : PortalResponse::OtherError;
}

InputCaptureSession::InputCaptureSession(QObject *parent, const QString &appId, const QString &path)
    : Session(parent, appId, path)
    , state(InputCapturePortal::State::Disabled)
{
}

InputCaptureSession::~InputCaptureSession() = default;

void InputCaptureSession::addBarrier(uint id, const QPair<QPoint, QPoint> &barrier)
{
    m_barriers.push_back({id, barrier.first, barrier.second});
}

void InputCaptureSession::clearBarriers()
{
    m_barriers.clear();
}

void InputCaptureSession::setClipboardEnabled(bool enabled)
{
    m_clipboardEnabled = enabled;
}

bool InputCaptureSession::clipboardEnabled() const
{
    return m_clipboardEnabled;
}

#include "moc_inputcapture.cpp"
