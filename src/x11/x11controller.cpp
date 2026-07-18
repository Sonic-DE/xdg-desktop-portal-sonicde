#include "x11controller.h"
#include "x11_debug.h"
#include "x11worker.h"

#include <QGuiApplication>
#include <QScreen>

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

#include <utility>

X11Controller::X11Controller(QObject* parent)
    : QObject(parent)
{
    connect(this, &X11Controller::outputsUpdated, this, &X11Controller::onWorkerOutputsReady);
    connect(this, &X11Controller::windowsUpdated, this, &X11Controller::onWorkerWindowsReady);
    connect(this, &X11Controller::capabilityReady, this, &X11Controller::onWorkerCapabilityReady);
}

X11Controller::~X11Controller()
{
    beginShutdown();
}

void X11Controller::setWorker(X11Worker* worker)
{
    m_worker = worker;
    if (!worker) {
        return;
    }
    connect(worker, &X11Worker::outputsReady, this, &X11Controller::outputsUpdated, Qt::QueuedConnection);
    connect(worker, &X11Worker::windowsReady, this, &X11Controller::windowsUpdated, Qt::QueuedConnection);
    connect(worker, &X11Worker::capabilityReady, this, &X11Controller::capabilityReady, Qt::QueuedConnection);
    connect(worker, &X11Worker::streamReady, this, &X11Controller::streamReady, Qt::QueuedConnection);
    connect(worker, &X11Worker::streamFailed, this, &X11Controller::streamFailed, Qt::QueuedConnection);
    connect(worker, &X11Worker::streamClosed, this, &X11Controller::streamClosed, Qt::QueuedConnection);
    connect(worker, &X11Worker::captureFrameReady, this, &X11Controller::captureFrameAvailable, Qt::QueuedConnection);
    connect(worker, &X11Worker::captureFailed, this, &X11Controller::captureFailed, Qt::QueuedConnection);
    connect(worker, &X11Worker::error, this, &X11Controller::errorOccurred, Qt::QueuedConnection);
}

void X11Controller::requestOutputs()
{
    if (m_worker && !m_stopping) {
        const bool queued = QMetaObject::invokeMethod(m_worker.data(), "refreshOutputs", Qt::QueuedConnection);
        if (!queued) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to queue X11 output refresh"));
        }
    }
}

void X11Controller::requestWindows()
{
    if (m_worker && !m_stopping) {
        const bool queued = QMetaObject::invokeMethod(m_worker.data(), "refreshWindows", Qt::QueuedConnection);
        if (!queued) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to queue X11 window refresh"));
        }
    }
}

void X11Controller::requestCapability()
{
    if (m_worker && !m_stopping) {
        const bool queued = QMetaObject::invokeMethod(m_worker.data(), "queryCapabilities", Qt::QueuedConnection);
        if (!queued) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to queue X11 capability query"));
        }
    }
}

void X11Controller::requestShutdown()
{
    beginShutdown();
    if (m_worker) {
        const bool queued = QMetaObject::invokeMethod(m_worker.data(), "shutdown", Qt::QueuedConnection);
        if (!queued) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to queue X11 shutdown"));
        }
    }
}

void X11Controller::beginShutdown()
{
    m_stopping = true;
}

QImage X11Controller::grabWindow(quint64 xid, bool includeCursor)
{
    QString error;
    if (!canCapture(&error)) {
        reportTerminalCaptureError(error);
        return {};
    }
    QImage img;
    QPointer<X11Worker> worker = m_worker;
#ifdef XDP_BUILD_TESTING
    if (std::exchange(m_testFailNextCaptureInvoke, false)) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 window capture"));
        return {};
    }
#endif
    const bool invoked = QMetaObject::invokeMethod(worker.data(), [worker, &img, xid, includeCursor]() {
        if (worker) {
            img = worker->grabWindow(xid, includeCursor);
        } }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 window capture"));
        return {};
    }
    return img;
}

QImage X11Controller::grabWorkspace(bool includeCursor)
{
    QString error;
    if (!canCapture(&error)) {
        reportTerminalCaptureError(error);
        return {};
    }
    QImage img;
    QPointer<X11Worker> worker = m_worker;
#ifdef XDP_BUILD_TESTING
    if (std::exchange(m_testFailNextCaptureInvoke, false)) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 workspace capture"));
        return {};
    }
#endif
    const bool invoked = QMetaObject::invokeMethod(worker.data(), [worker, &img, includeCursor]() {
        if (worker) {
            img = worker->grabWorkspace(includeCursor);
        } }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 workspace capture"));
        return {};
    }
    return img;
}

QImage X11Controller::grabOutput(const QString& outputUniqueId, bool includeCursor)
{
    QString error;
    if (!canCapture(&error)) {
        reportTerminalCaptureError(error);
        return {};
    }
    QImage img;
    QPointer<X11Worker> worker = m_worker;
#ifdef XDP_BUILD_TESTING
    if (std::exchange(m_testFailNextCaptureInvoke, false)) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 output capture"));
        return {};
    }
#endif
    const bool invoked = QMetaObject::invokeMethod(worker.data(), [worker, &img, outputUniqueId, includeCursor]() {
        if (worker) {
            img = worker->grabOutput(outputUniqueId, includeCursor);
        } }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 output capture"));
        return {};
    }
    return img;
}

QImage X11Controller::grabActiveWindow(bool includeCursor)
{
    QString error;
    if (!canCapture(&error)) {
        reportTerminalCaptureError(error);
        return {};
    }
    QImage img;
    QPointer<X11Worker> worker = m_worker;
#ifdef XDP_BUILD_TESTING
    if (std::exchange(m_testFailNextCaptureInvoke, false)) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 active window capture"));
        return {};
    }
#endif
    const bool invoked = QMetaObject::invokeMethod(worker.data(), [worker, &img, includeCursor]() {
        if (worker) {
            img = worker->grabActiveWindow(includeCursor);
        } }, Qt::BlockingQueuedConnection);
    if (!invoked) {
        reportTerminalCaptureError(QStringLiteral("Failed to invoke X11 active window capture"));
        return {};
    }
    return img;
}

bool X11Controller::isInputCaptureActive() const
{
    return m_inputCaptureActive;
}

void X11Controller::setInputCaptureActive(bool active)
{
    m_inputCaptureActive = active;
}

void X11Controller::onWorkerOutputsReady(QList<X11Types::OutputDescriptor> outputs)
{
    m_outputs = std::move(outputs);
}

void X11Controller::onWorkerWindowsReady(QList<X11Types::WindowDescriptor> windows)
{
    m_windows = std::move(windows);
}

void X11Controller::onWorkerCapabilityReady(X11Types::CapabilitySnapshot snapshot)
{
    if (m_stopping) {
        return;
    }
    m_capabilities = snapshot;
    m_ready = true;
}

bool X11Controller::canCapture(QString* error) const
{
    auto setError = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (!m_worker) {
        return setError(QStringLiteral("X11 capture worker is not available"));
    }
    if (!m_ready) {
        return setError(QStringLiteral("X11 capture worker is not ready"));
    }
    QThread* workerThread = m_worker->thread();
    if (!workerThread || !workerThread->isRunning()) {
        return setError(QStringLiteral("X11 capture worker thread is not running"));
    }
    if (m_stopping) {
        return setError(QStringLiteral("X11 capture is shutting down"));
    }
    if (QThread::currentThread() == workerThread) {
        return setError(QStringLiteral("X11 capture cannot be invoked from the worker thread"));
    }
    return true;
}

void X11Controller::reportTerminalCaptureError(const QString& error)
{
    qCWarning(XdgDesktopPortalKdeX11) << error;
    Q_EMIT errorOccurred(error);
}

QString X11Controller::activeApplicationId() const
{
    for (const auto& window : m_windows) {
        if (window.active) {
            return window.appId;
        }
    }
    return {};
}

QRect X11Controller::rootGeometry() const
{
    QRect geometry;
    for (const auto& output : m_outputs) {
        geometry = geometry.united(output.nativeGeometry);
    }
    if (geometry.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            geometry = geometry.united(screen->geometry());
        }
    }
    return geometry;
}
