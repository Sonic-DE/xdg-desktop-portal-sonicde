#pragma once

#include <QObject>
#include <QPointF>
#include <QRect>
#include <QString>

#include <xcb/xcb.h>

class X11Input : public QObject {
    Q_OBJECT
public:
    enum class ConnectionOwnership {
        Borrowed,
        Owned,
    };

    explicit X11Input(QObject* parent = nullptr);
    explicit X11Input(xcb_connection_t* connection, ConnectionOwnership ownership = ConnectionOwnership::Borrowed, QObject* parent = nullptr);
    ~X11Input() override;

    bool isAvailable() const;
    QString lastError() const;

    static int linuxButtonToXButton(int linuxButton);
    static int evdevKeycodeToXKeycode(int evdevKeycode);
    static bool isValidEvdevKeycode(int evdevKeycode);

    void injectPointerMotion(const QSizeF& delta);
    void injectPointerMotionAbsolute(const QRect& streamGeometry, const QPointF& pos);
    void injectPointerButton(int linuxButton, bool pressed);
    void injectPointerAxis(qreal dx, qreal dy);
    void injectPointerAxisDiscrete(Qt::Orientation axis, int steps);
    void injectKeySym(int keysym, bool pressed);
    void injectKeyCode(int keycode, bool pressed);

Q_SIGNALS:
    void availabilityChanged(bool available);
    void errorOccurred(QString error);

private:
    class Private;
    Private* d = nullptr;
};
