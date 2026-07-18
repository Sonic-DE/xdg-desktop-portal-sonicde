#pragma once

#include "x11types.h"

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>

class X11Worker;

class X11Controller : public QObject {
    Q_OBJECT
public:
    explicit X11Controller(QObject* parent = nullptr);
    ~X11Controller() override;

    void setWorker(X11Worker* worker);
    const X11Types::CapabilitySnapshot& capabilities() const
    {
        return m_capabilities;
    }
    bool isReady() const
    {
        return m_ready;
    }
    QList<X11Types::OutputDescriptor> outputs() const
    {
        return m_outputs;
    }
    QList<X11Types::WindowDescriptor> windows() const
    {
        return m_windows;
    }
    QString activeApplicationId() const;
    QRect rootGeometry() const;

    void requestOutputs();
    void requestWindows();
    void requestCapability();
    void requestShutdown();
    void beginShutdown();
    bool isStopping() const
    {
        return m_stopping;
    }
#ifdef XDP_BUILD_TESTING
    void setTestFailNextCaptureInvoke(bool fail)
    {
        m_testFailNextCaptureInvoke = fail;
    }
#endif

    QImage grabWindow(quint64 xid, bool includeCursor = true);
    QImage grabWorkspace(bool includeCursor = true);
    QImage grabOutput(const QString& outputUniqueId, bool includeCursor = true);
    QImage grabActiveWindow(bool includeCursor = true);

    bool isInputCaptureActive() const;
    void setInputCaptureActive(bool active);
    bool isRemoteDesktopActive() const
    {
        return m_remoteDesktopSessions > 0;
    }
    void beginRemoteDesktopSession()
    {
        ++m_remoteDesktopSessions;
    }
    void endRemoteDesktopSession()
    {
        if (m_remoteDesktopSessions > 0)
            --m_remoteDesktopSessions;
    }

Q_SIGNALS:
    void outputsUpdated(QList<X11Types::OutputDescriptor> outputs);
    void windowsUpdated(QList<X11Types::WindowDescriptor> windows);
    void capabilityReady(X11Types::CapabilitySnapshot snapshot);
    void captureFrameAvailable(quint32 nodeId, X11Types::CaptureFrame frame);
    void captureFailed(quint32 nodeId, QString error);
    void streamReady(uint nodeId, QVariantMap metadata);
    void streamFailed(uint nodeId, QString error);
    void streamClosed(uint nodeId);
    void errorOccurred(QString error);

private Q_SLOTS:
    void onWorkerOutputsReady(QList<X11Types::OutputDescriptor> outputs);
    void onWorkerWindowsReady(QList<X11Types::WindowDescriptor> windows);
    void onWorkerCapabilityReady(X11Types::CapabilitySnapshot snapshot);

private:
    bool canCapture(QString* error = nullptr) const;
    void reportTerminalCaptureError(const QString& error);

    QPointer<X11Worker> m_worker;
    X11Types::CapabilitySnapshot m_capabilities;
    QList<X11Types::OutputDescriptor> m_outputs;
    QList<X11Types::WindowDescriptor> m_windows;
    bool m_ready = false;
    bool m_stopping = false;
    bool m_inputCaptureActive = false;
    uint m_remoteDesktopSessions = 0;
#ifdef XDP_BUILD_TESTING
    bool m_testFailNextCaptureInvoke = false;
#endif
};
