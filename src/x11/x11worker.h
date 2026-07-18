#pragma once

#include "x11types.h"

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>

class X11WorkerPrivate;

class X11Worker : public QObject {
    Q_OBJECT
public:
    explicit X11Worker(QObject* parent = nullptr);
    ~X11Worker() override;

    bool isInitialized() const;
    bool isShuttingDown() const;

Q_SIGNALS:
    void outputsReady(QList<X11Types::OutputDescriptor> outputs);
    void windowsReady(QList<X11Types::WindowDescriptor> windows);
    void capabilityReady(X11Types::CapabilitySnapshot snapshot);
    void streamReady(uint nodeId, QVariantMap metadata);
    void streamFailed(uint nodeId, QString error);
    void streamClosed(uint nodeId);
    void captureFrameReady(quint32 nodeId, X11Types::CaptureFrame frame);
    void captureFailed(quint32 nodeId, QString error);
    void error(QString message);

public Q_SLOTS:
    void initialize();
    void refreshOutputs();
    void refreshWindows();
    void queryCapabilities();
    void shutdown();

    QImage grabWindow(quint64 xid, bool includeCursor = true);
    QImage grabWorkspace(bool includeCursor = true);
    QImage grabOutput(const QString& outputUniqueId, bool includeCursor = true);
    QImage grabActiveWindow(bool includeCursor = true);

private:
    X11WorkerPrivate* d;
};
