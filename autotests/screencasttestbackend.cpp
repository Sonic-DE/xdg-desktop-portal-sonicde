/*
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QTimer>

#include "desktopportal.h"
#include "screenselectionprovider.h"
#include "utils.h"
#include "x11/x11controller.h"
#include "x11portalbootstrap.h"

#include <memory>

using namespace Qt::StringLiterals;

class FixedScreenSelectionRequest final : public ScreenSelectionRequest {
    Q_OBJECT
public:
    FixedScreenSelectionRequest(const QString& outputId, quint64 windowXid, X11Controller* controller, QObject* parent)
        : ScreenSelectionRequest(parent)
        , m_outputId(outputId)
        , m_windowXid(windowXid)
        , m_controller(controller)
    {
        QTimer::singleShot(0, this, [this] {
            Q_EMIT finished(DialogResult::Accepted);
        });
    }

    QList<Output> selectedOutputs() const override
    {
        if (m_outputId.isEmpty()) {
            return {};
        }
        if (m_outputId == u"Workspace"_s) {
            Output out;
            out.setIsSynthetic(true);
            out.setOutputType(Output::Workspace);
            out.setName(u"Workspace"_s);
            out.setUniqueId(u"Workspace"_s);
            return {out};
        }
        if (!m_controller) {
            return {};
        }
        for (const auto& candidate : m_controller->outputs()) {
            if (candidate.uniqueId != m_outputId && candidate.name != m_outputId) {
                continue;
            }
            Output out;
            out.setOutputType(Output::Monitor);
            out.setName(candidate.name);
            out.setUniqueId(candidate.uniqueId);
            out.setGeometry(candidate.nativeGeometry);
            return {out};
        }
        return {};
    }

    QList<X11Types::WindowDescriptor> selectedWindows() const override
    {
        if (m_windowXid == 0 || !m_controller) {
            return {};
        }
        for (const auto& candidate : m_controller->windows()) {
            if (candidate.xid == m_windowXid) {
                return {candidate};
            }
        }
        X11Types::WindowDescriptor descriptor;
        descriptor.xid = m_windowXid;
        descriptor.title = u"Fixed test window"_s;
        descriptor.nativeGeometry = m_controller->rootGeometry();
        return {descriptor};
    }

    bool allowRestore() const override
    {
        return false;
    }

    QRect selectedRegion() const override
    {
        return {};
    }

    QWindow* windowHandle() const override
    {
        return nullptr;
    }

public Q_SLOTS:
    void reject() override
    {
        Q_EMIT finished(DialogResult::Rejected);
    }

private:
    const QString m_outputId;
    const quint64 m_windowXid = 0;
    X11Controller* const m_controller = nullptr;
};

class FixedScreenSelectionProvider final : public ScreenSelectionProvider {
    Q_OBJECT
public:
    FixedScreenSelectionProvider(QString outputId, quint64 windowXid, QObject* parent)
        : ScreenSelectionProvider(parent)
        , m_outputId(std::move(outputId))
        , m_windowXid(windowXid)
    {
    }

    ScreenSelectionRequest* createRequest(const QString& appName,
        bool multiple,
        ScreenCastPortal::SourceTypes types,
        X11Controller* controller,
        QObject* parent) override
    {
        Q_UNUSED(appName)
        Q_UNUSED(multiple)
        Q_UNUSED(types)
        return new FixedScreenSelectionRequest(m_outputId, m_windowXid, controller, parent);
    }

private:
    const QString m_outputId;
    const quint64 m_windowXid = 0;
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setQuitLockEnabled(false);

    if (QGuiApplication::platformName() != u"xcb"_s) {
        qCritical("screencast test backend requires QT_QPA_PLATFORM=xcb");
        return 1;
    }

    QCommandLineParser parser;
    QCommandLineOption outputOption(u"fixed-output"_s, u"Fixed output unique id or Workspace."_s, u"output"_s);
    QCommandLineOption windowOption(u"fixed-window-xid"_s, u"Fixed X11 window id."_s, u"xid"_s);
    QCommandLineOption replaceOption(u"replace"_s, u"Replace running test backend."_s);
    parser.addOptions({outputOption, windowOption, replaceOption});
    parser.process(app);

    bool ok = false;
    const quint64 windowXid = parser.value(windowOption).isEmpty() ? 0 : parser.value(windowOption).toULongLong(&ok, 0);
    if (!parser.value(windowOption).isEmpty() && !ok) {
        qCritical("invalid --fixed-window-xid value");
        return 2;
    }

    PortalBootstrap bootstrap(&app);
    QString readyError;
    if (!bootstrap.waitForReady(5000, &readyError)) {
        qCritical().noquote() << u"X11 portal backend failed to become ready: "_s + readyError;
        return 1;
    }

    FixedScreenSelectionProvider selectionProvider(parser.value(outputOption), windowXid, &app);
    std::unique_ptr<DesktopPortal> desktopPortal;

    const QLatin1String serviceName("org.freedesktop.impl.portal.desktop.sonicde.test");
    const QLatin1String objectPath("/org/freedesktop/portal/desktop");
    const auto dbusQueueOption = parser.isSet(replaceOption) ? QDBusConnectionInterface::ReplaceExistingService : QDBusConnectionInterface::DontQueueService;

    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (!sessionBus.interface()->registerService(serviceName, dbusQueueOption, QDBusConnectionInterface::AllowReplacement)) {
        qCritical() << "failed to register test backend service" << serviceName;
        return 1;
    }
    desktopPortal = std::make_unique<DesktopPortal>(&bootstrap, serviceName, nullptr, &selectionProvider);
    if (!sessionBus.registerObject(QString(objectPath), desktopPortal.get(), QDBusConnection::ExportAdaptors)) {
        qCritical() << "failed to register test backend object" << objectPath;
        return 1;
    }

    const int result = app.exec();
    sessionBus.unregisterObject(QString(objectPath));
    desktopPortal.reset();
    return result;
}

#include "screencasttestbackend.moc"
