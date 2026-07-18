#include "x11portalbootstrap.h"
#include "x11_debug.h"

#include "x11/x11controller.h"
#include "x11/x11worker.h"
#include "x11/xeismounter.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

PortalBootstrap::PortalBootstrap(QObject* parent)
    : QObject(parent)
    , m_thread(new QThread(this))
    , m_worker(new X11Worker)
    , m_controller(new X11Controller(this))
    , m_eisMounter(new XEisMounter(this))
{
    qRegisterMetaType<X11Types::OutputDescriptor>("X11Types::OutputDescriptor");
    qRegisterMetaType<X11Types::WindowDescriptor>("X11Types::WindowDescriptor");
    qRegisterMetaType<X11Types::CaptureFrame>("X11Types::CaptureFrame");
    qRegisterMetaType<X11Types::CaptureError>("X11Types::CaptureError");
    qRegisterMetaType<X11Types::StreamReady>("X11Types::StreamReady");
    qRegisterMetaType<X11Types::InputEventError>("X11Types::InputEventError");
    qRegisterMetaType<X11Types::CapabilitySnapshot>("X11Types::CapabilitySnapshot");
    qRegisterMetaType<QList<X11Types::OutputDescriptor>>("QList<X11Types::OutputDescriptor>");
    qRegisterMetaType<QList<X11Types::WindowDescriptor>>("QList<X11Types::WindowDescriptor>");

    m_worker->moveToThread(m_thread);
    m_controller->setWorker(m_worker);

    connect(m_thread, &QThread::started, m_worker, &X11Worker::initialize);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

PortalBootstrap::~PortalBootstrap()
{
    if (m_controller) {
        m_controller->beginShutdown();
    }
    if (m_worker && m_thread && m_thread->isRunning() && QThread::currentThread() != m_thread) {
        const bool invoked = QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        if (!invoked) {
            qCWarning(XdgDesktopPortalKdeX11) << "Failed to invoke X11 worker shutdown";
        }
    } else if (m_worker && (!m_thread || QThread::currentThread() == m_thread)) {
        qCWarning(XdgDesktopPortalKdeX11) << "Skipping blocking X11 worker shutdown from worker thread";
    }
    if (m_thread && m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool PortalBootstrap::waitForReady(int timeoutMs, QString* diagnostic)
{
    if (m_controller->isReady()) {
        return true;
    }
    if (!m_thread || !m_thread->isRunning()) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("X11 worker thread is not running");
        }
        return false;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    bool timedOut = false;
    connect(m_controller, &X11Controller::capabilityReady, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, [&loop, &timedOut] {
        timedOut = true;
        loop.quit();
    });
    timer.start(timeoutMs);
    loop.exec();

    if (m_controller->isReady()) {
        return true;
    }
    if (diagnostic) {
        *diagnostic = timedOut ? QStringLiteral("Timed out waiting for X11 capability readiness") : QStringLiteral("X11 capability readiness failed");
    }
    return false;
}
