#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

extern "C" {
#include <xcb/xcb.h>
}

class X11CapturePrivate;

class X11Capture : public QObject {
    Q_OBJECT
public:
    explicit X11Capture(QObject* parent = nullptr);
    X11Capture(xcb_connection_t* connection, xcb_screen_t* screen, bool takeOwnership, QObject* parent = nullptr);
    ~X11Capture() override;

    bool isValid() const;
    void setConnection(xcb_connection_t* connection, xcb_screen_t* screen, bool takeOwnership);

    QImage captureWorkspace(bool includeCursor = true);
    QImage captureWindow(quint64 xid, bool includeCursor = true);
    QImage captureOutput(const QString& outputUniqueId, bool includeCursor = true);
    QImage captureActiveWindow(bool includeCursor = true);

    static QRect boundedCaptureRect(const QRect& requested, const QRect& bounds);
    static QImage convertToRgb32(const QByteArray& data,
        const QSize& size,
        int bytesPerLine,
        int bitsPerPixel,
        int byteOrder,
        quint32 redMask,
        quint32 greenMask,
        quint32 blueMask);
    static void compositeCursor(QImage* target,
        const QImage& cursor,
        const QPoint& hotSpot,
        const QPoint& cursorRootPosition,
        const QRect& captureRootRect);

private:
    X11CapturePrivate* d = nullptr;
};
