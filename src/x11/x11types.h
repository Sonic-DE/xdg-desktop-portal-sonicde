#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <optional>

#include <xcb/xcb.h>

namespace X11Types {
struct OutputDescriptor {
    QString uniqueId;
    QString name;
    QString display;
    QRect nativeGeometry;
    bool isLaptop = false;
    bool isTelevision = false;
    QString connectorType;

    bool operator==(const OutputDescriptor& other) const = default;
};

struct WindowDescriptor {
    xcb_window_t xid = 0;
    QString appId;
    QString title;
    QString wmClass;
    QRect nativeGeometry;
    bool mapped = false;
    bool active = false;
    bool skipTaskbar = false;
    bool skipSwitcher = false;
    qint64 pid = -1;
    QString sourceKey;

    bool operator==(const WindowDescriptor& other) const = default;
};

struct CaptureFrame {
    QImage image;
    QRect cursor;
    QImage cursorImage;
    bool hasCursor = false;
    qint64 sequence = 0;
};

struct CaptureError {
    QString message;
};

struct StreamReady {
    uint nodeId = 0;
    QVariantMap metadata;
};

struct InputEventError {
    QString message;
};

struct CapabilitySnapshot {
    bool screencastMonitor = false;
    bool screencastWindow = false;
    bool screencastCursorEmbedded = false;
    bool screencastCursorMetadata = false;
    bool pointerInput = false;
    bool keyboardInput = false;
    bool inputCaptureKeyboard = false;
    bool inputCapturePointer = false;
    bool inputCaptureTouch = false;
    QStringList disabledReasons;
};

}

Q_DECLARE_METATYPE(X11Types::OutputDescriptor)
Q_DECLARE_METATYPE(X11Types::WindowDescriptor)
Q_DECLARE_METATYPE(X11Types::CaptureFrame)
Q_DECLARE_METATYPE(X11Types::CaptureError)
Q_DECLARE_METATYPE(X11Types::StreamReady)
Q_DECLARE_METATYPE(X11Types::InputEventError)
Q_DECLARE_METATYPE(X11Types::CapabilitySnapshot)
Q_DECLARE_METATYPE(QList<X11Types::OutputDescriptor>)
Q_DECLARE_METATYPE(QList<X11Types::WindowDescriptor>)
