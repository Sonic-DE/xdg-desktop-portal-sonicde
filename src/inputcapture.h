/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef XDG_DESKTOP_PORTAL_KDE_INPUTCAPTURE_H
#define XDG_DESKTOP_PORTAL_KDE_INPUTCAPTURE_H

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>

#include "session.h"

class QDBusMessage;
class InputCaptureSession;
class X11Controller;
class InputCaptureBackend;

class InputCapturePortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.InputCapture")
    Q_PROPERTY(uint version READ version CONSTANT)
    Q_PROPERTY(uint SupportedCapabilities READ SupportedCapabilities CONSTANT)
public:
    explicit InputCapturePortal(QObject* parent, X11Controller* controller = nullptr);
    ~InputCapturePortal() override;

    enum Capability : uint {
        None = 0x0,
        Keyboard = 0x1,
        Pointer = 0x2,
        TouchScreen = 0x4,
        All = (Keyboard | Pointer | TouchScreen),
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    enum class State {
        Disabled,
        Deactivated,
        Activated
    };

    enum class PersistMode : uint {
        None = 0,
        WhileRunning = 1,
        UntilRevoked = 2,
    };

    uint version() const
    {
        return 2;
    }
    uint SupportedCapabilities() const;

    struct zone {
        uint width;
        uint height;
        int x_offset;
        int y_offset;

        bool operator==(const zone &) const = default;
    };

public Q_SLOTS:
    uint CreateSession(const QDBusObjectPath& handle,
        const QDBusObjectPath& session_handle,
        const QString& app_id,
        const QString& parent_window,
        const QVariantMap& options,
        const QDBusMessage& message,
        uint& replyResponse,
        QVariantMap& replyResults);

    [[nodiscard]] QVariantMap CreateSession2(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options);

    void Start(const QDBusObjectPath &handle,
               const QDBusObjectPath &session_handle,
               const QString &app_id,
               const QString &parent_window,
               const QVariantMap &options,
               const QDBusMessage &message,
               uint &replyResponse,
               QVariantMap &replyResults);

    uint
    GetZones(const QDBusObjectPath &handle, const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results);
    uint SetPointerBarriers(const QDBusObjectPath &handle,
                            const QDBusObjectPath &session_handle,
                            const QString &app_id,
                            const QVariantMap &options,
                            const QList<QVariantMap> &barriers,
                            uint zone_set,
                            QVariantMap &results);

    uint Enable(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results);
    uint Disable(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results);

    uint Release(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, QVariantMap &results);
    QDBusUnixFileDescriptor ConnectToEIS(const QDBusObjectPath &session_handle, const QString &app_id, const QVariantMap &options, const QDBusMessage &message);

Q_SIGNALS:
    void Disabled(const QDBusObjectPath &session_handle, const QVariantMap &options);
    void Activated(const QDBusObjectPath &session_handle, const QVariantMap &options);
    void Deactivated(const QDBusObjectPath &session_handle, const QVariantMap &options);
    void ZonesChanged(const QDBusObjectPath &session_handle, const QVariantMap &options);

private:
    uint m_zoneId = 0;
    X11Controller* m_controller = nullptr;
    InputCaptureBackend* m_backend = nullptr;
};
Q_DECLARE_OPERATORS_FOR_FLAGS(InputCapturePortal::Capabilities)


class InputCaptureSession : public Session
{
    Q_OBJECT
public:
    explicit InputCaptureSession(QObject *parent, const QString &appId, const QString &path);
    ~InputCaptureSession() override;

    SessionType type() const override
    {
        return SessionType::InputCapture;
    }

    InputCapturePortal::State state;
    InputCapturePortal::Capabilities capabilities() const
    {
        return m_capabilities;
    }
    void setCapabilities(InputCapturePortal::Capabilities capabilities)
    {
        m_capabilities = capabilities;
    }
    InputCapturePortal::PersistMode persistMode() const
    {
        return m_persistMode;
    }
    void setPersistMode(InputCapturePortal::PersistMode mode)
    {
        m_persistMode = mode;
    }
    bool started() const
    {
        return m_started;
    }
    void setStarted(bool started)
    {
        m_started = started;
    }

    void addBarrier(uint id, const QPair<QPoint, QPoint> &barriers);
    void clearBarriers();

    [[nodiscard]] bool clipboardEnabled() const;
    void setClipboardEnabled(bool enabled);

Q_SIGNALS:
    void disabled();
    void activated(uint activationId, uint barrier, const QPointF &cursorPosition);
    void deactivated(uint activationId);

private:
    bool m_clipboardEnabled = false;
    InputCapturePortal::Capabilities m_capabilities;
    InputCapturePortal::PersistMode m_persistMode = InputCapturePortal::PersistMode::None;
    bool m_started = false;
    QList<std::tuple<uint, QPoint, QPoint>> m_barriers;
};

#endif
