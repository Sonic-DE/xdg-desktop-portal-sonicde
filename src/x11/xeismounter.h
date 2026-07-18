#pragma once

#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSizeF>

#include <cstdint>

class XEisMounter : public QObject {
    Q_OBJECT
public:
    enum class Mode {
        RemoteDesktopReceiver,
        InputCaptureSender,
    };

    explicit XEisMounter(QObject* parent = nullptr);
    ~XEisMounter() override;

    QDBusUnixFileDescriptor attachSender();
    QDBusUnixFileDescriptor attachReceiver();

    void setRegions(const QList<QRect>& regions);
    void setCapabilities(bool pointer, bool keyboard);

    bool isValid() const;
    QString lastError() const;
    int clientCount() const;

    static bool clientRoleMatchesMode(Mode mode, bool clientIsSender);
    static QList<QRect> normalizedRegions(const QList<QRect>& regions);

    void sendPointerMotion(const QSizeF& delta);
    void sendPointerMotionAbsolute(const QPointF& position);
    void sendPointerButton(uint linuxButton, bool pressed);
    void sendPointerAxis(qreal dx, qreal dy);
    void sendPointerAxisDiscrete(Qt::Orientation axis, int steps);
    void sendKey(uint evdevKeycode, bool pressed);
    void sendKeySym(uint keysym, bool pressed);
    void stopSending();

public Q_SLOTS:
    void teardown();

Q_SIGNALS:
    void clientConnected(int clientId);
    void clientDisconnected(int clientId);
    void errorOccurred(QString error);
    void pointerMotionReceived(int clientId, QSizeF delta);
    void pointerMotionAbsoluteReceived(int clientId, QPointF position);
    void pointerButtonReceived(int clientId, uint linuxButton, bool pressed);
    void pointerAxisReceived(int clientId, QPointF delta);
    void pointerAxisDiscreteReceived(int clientId, QPoint discrete120);
    void keyReceived(int clientId, uint evdevKeycode, bool pressed);
    void keySymReceived(int clientId, uint keysym, bool pressed);
    void frameReceived(int clientId, quint64 timeUsec);
    void receiverStarted(int clientId, uint sequence);
    void receiverStopped(int clientId);

private:
    class Private;
    Private* d = nullptr;
};
