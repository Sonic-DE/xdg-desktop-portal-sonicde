/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 * SPDX-FileCopyrightText: 2022 Harald Sitter <sitter@kde.org>
 */

#include "screencast.h"
#include "dbushelpers.h"
#include "notificationinhibition.h"
#include "remotedesktop.h"
#include "request.h"
#include "restoredata.h"
#include "screencast_debug.h"
#include "screenselectionprovider.h"
#include "session.h"
#include "utils.h"
#include "x11/x11controller.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KStatusNotifierItem>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDataStream>
#include <QGuiApplication>
#include <QMenu>

#include <ranges>

using namespace Qt::StringLiterals;

struct WindowRestoreInfo {
    QString appId;
    QString title;
};

QDataStream& operator<<(QDataStream& out, const WindowRestoreInfo& info)
{
    return out << info.appId << info.title;
}
QDataStream& operator>>(QDataStream& in, WindowRestoreInfo& info)
{
    return in >> info.appId >> info.title;
}

Q_DECLARE_METATYPE(WindowRestoreInfo)

int levenshteinDistance(const QString& a, const QString& b)
{
    if (a == b) {
        return 0;
    }

    auto v0 = std::make_unique_for_overwrite<int[]>(b.size() + 1);
    auto v1 = std::make_unique_for_overwrite<int[]>(b.size() + 1);
    std::iota(v0.get(), v0.get() + b.size(), 0);

    for (int i = 0; i < a.size(); ++i) {
        v1[0] = i + 1;
        for (int j = 0; j < b.size(); ++j) {
            const int delCost = v0[j + 1] + 1;
            const int insertCost = v1[j] + 1;
            const int subsCost = a.at(i) == b.at(j) ? v0[j] : v0[j] + 1;
            v1[j + 1] = std::min({delCost, insertCost, subsCost});
        }
        std::swap(v0, v1);
    }
    return v0[b.size()];
}

QList<X11Types::WindowDescriptor> tryMatchWindows(const QList<WindowRestoreInfo>& toRestore, const QList<X11Types::WindowDescriptor>& candidates)
{
    QList<X11Types::WindowDescriptor> matches;
    for (const auto& restoreInfo : toRestore) {
        int bestDistance = INT_MAX;
        X11Types::WindowDescriptor bestMatch;
        bool found = false;
        for (const auto& window : candidates) {
            if (window.appId != restoreInfo.appId) {
                continue;
            }
            if (const int distance = levenshteinDistance(window.title, restoreInfo.title); distance == 0) {
                matches.push_back(window);
                found = true;
                break;
            } else if (distance < bestDistance) {
                bestDistance = distance;
                bestMatch = window;
            }
        }
        if (!found && bestDistance < restoreInfo.title.size() / 2) {
            matches.push_back(bestMatch);
        }
    }
    return matches;
}

ScreenCastPortal::ScreenCastPortal(QObject* parent, X11Controller* controller, ScreenSelectionProvider* selectionProvider)
    : QDBusAbstractAdaptor(parent)
    , m_controller(controller)
    , m_selectionProvider(selectionProvider)
{
    qDBusRegisterMetaType<RestoreData>();
    qRegisterMetaType<QList<WindowRestoreInfo>>();
    qDBusRegisterMetaType<std::pair<uint, QVariantMap>>();
    qDBusRegisterMetaType<QList<std::pair<uint, QVariantMap>>>();
}

ScreenCastPortal::~ScreenCastPortal() = default;

uint ScreenCastPortal::AvailableSourceTypes() const
{
    if (!m_controller) {
        return 0;
    }
    uint types = 0;
    if (m_controller->capabilities().screencastMonitor) {
        types |= Monitor;
    }
    if (m_controller->capabilities().screencastWindow) {
        types |= Window;
    }
    return types;
}

uint ScreenCastPortal::AvailableCursorModes() const
{
    uint modes = Hidden;
    if (m_controller && m_controller->capabilities().screencastCursorEmbedded) {
        modes |= Embedded;
    }
    return modes;
}

bool inhibitionsEnabled()
{
    auto cfg = KSharedConfig::openConfig(QStringLiteral("plasmanotifyrc"));
    KConfigGroup grp(cfg, u"DoNotDisturb"_s);
    return grp.readEntry("WhenScreenSharing", true);
}

uint ScreenCastPortal::CreateSession(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QVariantMap& options,
    QVariantMap& results)
{
    Q_UNUSED(results)

    qCDebug(XdgDesktopPortalKdeScreenCast) << "CreateSession called with parameters:";
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    handle: " << handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    app_id: " << app_id;
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    options: " << options;

    Session* session = new ScreenCastSession(this, app_id, session_handle.path(), QStringLiteral("media-record"), m_controller);

    if (!session->isValid()) {
        delete session;
        return PortalResponse::OtherError;
    }

    return PortalResponse::Success;
}

uint ScreenCastPortal::SelectSources(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QVariantMap& options,
    QVariantMap& results)
{
    Q_UNUSED(results)

    qCDebug(XdgDesktopPortalKdeScreenCast) << "SelectSource called with parameters:";
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    handle: " << handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    app_id: " << app_id;
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    options: " << options;

    ScreenCastSession* session = Session::getSession<ScreenCastSession>(session_handle.path());

    if (!session) {
        qCWarning(XdgDesktopPortalKdeScreenCast) << "Tried to select sources on non-existing session " << session_handle.path();
        return PortalResponse::OtherError;
    }

    session->setOptions(options);
    if (session->type() == Session::RemoteDesktop) {
        auto remoteDesktopSession = qobject_cast<RemoteDesktopSession*>(session);
        if (remoteDesktopSession) {
            remoteDesktopSession->setScreenSharingEnabled(true);
        }
    } else {
        session->setPersistMode(ScreenCastPortal::PersistMode(options.value(QStringLiteral("persist_mode")).toUInt()));
        session->setRestoreData(options.value(QStringLiteral("restore_data")));
    }

    return PortalResponse::Success;
}

std::pair<PortalResponse::Response, QVariantMap> continueStartAfterDialog(ScreenCastSession* session,
    const QList<Output>& selectedOutputs,
    const QRect& selectedRegion,
    const QList<X11Types::WindowDescriptor>& selectedWindows,
    bool allowRestore)
{
    X11Controller* controller = session->controller();
    if (!controller) {
        return {PortalResponse::OtherError, {}};
    }
    QVariantList outputs;
    QList<WindowRestoreInfo> windows;
    std::vector<std::unique_ptr<ScreencastingStream>> streams;
    QPointer<ScreenCastSession> guardedSession(session);
    const bool includeCursor = session->cursorMode() == ScreenCastPortal::Embedded;
    for (const auto& output : std::as_const(selectedOutputs)) {
        auto stream = std::make_unique<ScreencastingStream>(nullptr);
        QRect streamGeometry(QPoint(output.x(), output.y()), QSize(output.w(), output.h()));
        if (output.outputType() == Output::Workspace) {
            streamGeometry = controller->rootGeometry();
        } else if (output.outputType() == Output::Region) {
            streamGeometry = selectedRegion;
        }
        stream->setGeometry(streamGeometry);
        QVariantMap md;
        md[u"position"_s] = streamGeometry.topLeft();
        md[u"size"_s] = streamGeometry.size();
        md[u"source_type"_s] = 1;
        stream->setMetaData(md);
        if (allowRestore) {
            outputs += output.uniqueId();
        }
        const auto outputType = output.outputType();
        const QString outputId = output.uniqueId();
        auto provider = [controller, outputType, outputId, selectedRegion, includeCursor]() -> PipeWireStream::FrameProviderResult {
            if (!controller || !controller->isReady()) {
                return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 capture controller is not ready"));
            }
            QImage image;
            if (outputType == Output::Workspace) {
                image = controller->grabWorkspace(includeCursor);
            } else if (outputType == Output::Region) {
                image = controller->grabWorkspace(includeCursor);
                if (!image.isNull()) {
                    image = image.copy(selectedRegion.translated(-controller->rootGeometry().topLeft()));
                }
            } else {
                image = controller->grabOutput(outputId, includeCursor);
            }
            if (image.isNull()) {
                return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 output capture failed"));
            }
            image = image.convertToFormat(QImage::Format_RGBX8888);
            PipeWireStream::Frame frame;
            frame.size = image.size();
            frame.stride = image.bytesPerLine();
            frame.format = PipeWireStream::PixelFormat::RGBx;
            frame.data = QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
            return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
        };
        if (!stream->start(provider) || stream->nodeid() == 0) {
            qCWarning(XdgDesktopPortalKdeScreenCast) << "Failed to create PipeWire output stream" << outputId;
            return {PortalResponse::OtherError, {}};
        }
        streams.push_back(std::move(stream));
    }
    for (const auto& win : std::as_const(selectedWindows)) {
        auto stream = std::make_unique<ScreencastingStream>(nullptr);
        stream->setGeometry(win.nativeGeometry);
        QVariantMap md;
        md[u"source_type"_s] = 2;
        stream->setMetaData(md);
        if (allowRestore) {
            windows += WindowRestoreInfo{win.appId, win.title};
        }
        const quint64 xid = win.xid;
        auto provider = [controller, xid, includeCursor]() -> PipeWireStream::FrameProviderResult {
            if (!controller || !controller->isReady()) {
                return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 capture controller is not ready"));
            }
            QImage image = controller->grabWindow(xid, includeCursor);
            if (image.isNull()) {
                return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("X11 window capture failed"));
            }
            image = image.convertToFormat(QImage::Format_RGBX8888);
            PipeWireStream::Frame frame;
            frame.size = image.size();
            frame.stride = image.bytesPerLine();
            frame.format = PipeWireStream::PixelFormat::RGBx;
            frame.data = QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
            return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
        };
        if (!stream->start(provider) || stream->nodeid() == 0) {
            qCWarning(XdgDesktopPortalKdeScreenCast) << "Failed to create PipeWire window stream" << xid;
            return {PortalResponse::OtherError, {}};
        }
        streams.push_back(std::move(stream));
    }

    if (streams.empty()) {
        qCWarning(XdgDesktopPortalKdeScreenCast) << "No PipeWire streams were created";
        return {PortalResponse::OtherError, {}};
    }

    if (!guardedSession) {
        return {PortalResponse::OtherError, {}};
    }

    session->setStreams(std::move(streams));

    QVariantMap results;
    QList<std::pair<uint, QVariantMap>> dbusResultForStreams;
    std::ranges::transform(session->streams(), std::back_inserter(dbusResultForStreams), [](const std::unique_ptr<ScreencastingStream>& stream) {
        return std::pair{stream->nodeid(), stream->metaData()};
    });
    results.insert(QStringLiteral("streams"), QVariant::fromValue(dbusResultForStreams));
    if (allowRestore) {
        results.insert(u"persist_mode"_s, quint32(session->persistMode()));
        if (session->persistMode() != ScreenCastPortal::NoPersist) {
            const RestoreData restoreData = {u"KDE"_s,
                RestoreData::currentRestoreDataVersion(),
                QVariantMap{
                    {u"outputs"_s, outputs},
                    {u"windows"_s, QVariant::fromValue(windows)},
                    {u"region"_s, selectedRegion},
                }};
            results.insert(u"restore_data"_s, QVariant::fromValue<RestoreData>(restoreData));
        }
    }

    if (inhibitionsEnabled()) {
        new NotificationInhibition(session->appId(), i18nc("Do not disturb mode is enabled because...", "Screen sharing in progress"), session);
    }
    qCDebug(XdgDesktopPortalKdeScreenCast) << "Screencast started successfully";
    return {PortalResponse::Success, results};
}

void ScreenCastPortal::Start(const QDBusObjectPath& handle,
    const QDBusObjectPath& session_handle,
    const QString& app_id,
    const QString& parent_window,
    const QVariantMap& options,
    const QDBusMessage& message,
    uint& replyResponse,
    QVariantMap& replyResults)
{
    qCDebug(XdgDesktopPortalKdeScreenCast) << "Start called with parameters:";
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    handle: " << handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    session_handle: " << session_handle.path();
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    app_id: " << app_id;
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    parent_window: " << parent_window;
    qCDebug(XdgDesktopPortalKdeScreenCast) << "    options: " << options;

    QPointer<ScreenCastSession> session = Session::getSession<ScreenCastSession>(session_handle.path());

    if (!session) {
        qCWarning(XdgDesktopPortalKdeScreenCast) << "Tried to call start on non-existing session " << session_handle.path();
        replyResponse = PortalResponse::OtherError;
        return;
    }

    if (QGuiApplication::screens().isEmpty()) {
        qCWarning(XdgDesktopPortalKdeScreenCast) << "Failed to show dialog as there is no screen to select";
        replyResponse = PortalResponse::OtherError;
        return;
    }

    const PersistMode persist = session->persistMode();
    bool valid = false;
    QList<Output> selectedOutputs;
    QList<X11Types::WindowDescriptor> selectedWindows;
    QRect selectedRegion;
    if (persist != NoPersist && session->restoreData().isValid()) {
        const RestoreData restoreData = qdbus_cast<RestoreData>(session->restoreData().value<QDBusArgument>());
        if (restoreData.session == QLatin1String("KDE") && restoreData.version == RestoreData::currentRestoreDataVersion()) {
            const QVariantMap restoreDataPayload = restoreData.payload;
            const QVariantList restoreOutputs = restoreDataPayload[QStringLiteral("outputs")].toList();
            selectedRegion = restoreDataPayload.value(QStringLiteral("region")).toRect();
            if (!restoreOutputs.isEmpty()) {
                for (const auto& outputUniqueId : restoreOutputs) {
                    if (outputUniqueId == "Workspace"_L1) {
                        Output out;
                        out.setIsSynthetic(true);
                        out.setOutputType(Output::Workspace);
                        out.setName(QStringLiteral("Workspace"));
                        out.setUniqueId(QStringLiteral("Workspace"));
                        out.setWidth(0);
                        out.setHeight(0);
                        out.setX(0);
                        out.setY(0);
                        selectedOutputs << out;
                    } else if (outputUniqueId == "Region"_L1 && selectedRegion.isValid()) {
                        Output out;
                        out.setIsSynthetic(true);
                        out.setOutputType(Output::Region);
                        out.setName(QStringLiteral("Region"));
                        out.setUniqueId(QStringLiteral("Region"));
                        out.setGeometry(selectedRegion);
                        selectedOutputs << out;
                    } else {
                        const QString id = outputUniqueId.toString();
                        const auto descriptors = m_controller ? m_controller->outputs() : QList<X11Types::OutputDescriptor>{};
                        const auto match = std::ranges::find_if(descriptors, [&id](const auto& candidate) { return candidate.uniqueId == id; });
                        if (match != descriptors.end()) {
                            Output out;
                            out.setOutputType(Output::Monitor);
                            out.setName(match->name);
                            out.setUniqueId(match->uniqueId);
                            out.setGeometry(match->nativeGeometry);
                            selectedOutputs << out;
                        }
                    }
                }
                valid = selectedOutputs.count() == restoreOutputs.count();
            }
            const auto restoreWindows = restoreDataPayload[QStringLiteral("windows")].value<QList<WindowRestoreInfo>>();
            if (!restoreWindows.isEmpty()) {
                selectedWindows = tryMatchWindows(restoreWindows, m_controller ? m_controller->windows() : QList<X11Types::WindowDescriptor>{});
                valid = selectedWindows.count() == restoreWindows.count();
            }
        }
    }

    if (valid) {
        std::tie(replyResponse, replyResults) = continueStartAfterDialog(session, selectedOutputs, selectedRegion, selectedWindows, true);
        return;
    }

    auto* selectionProvider = m_selectionProvider;
    if (!selectionProvider) {
        selectionProvider = new DialogScreenSelectionProvider(this);
        m_selectionProvider = selectionProvider;
    }

    auto* selectionRequest = selectionProvider->createRequest(app_id, session->multipleSources(), SourceTypes(session->types()), m_controller, this);
    Utils::setParentWindow(selectionRequest->windowHandle(), parent_window);
    Request::makeClosableDialogRequestWithSession(handle, selectionRequest, session);
    delayReply(message, selectionRequest, this, [selectionRequest, session](DialogResult result) -> QVariantList {
        if (result == DialogResult::Rejected) {
            return QVariantList{PortalResponse::fromDialogResult(result), QVariantMap{}};
        }
        auto [response, results] = continueStartAfterDialog(session,
            selectionRequest->selectedOutputs(),
            selectionRequest->selectedRegion(),
            selectionRequest->selectedWindows(),
            selectionRequest->allowRestore());
        return QVariantList{response, results};
    });
}

ScreenCastSession::ScreenCastSession(QObject* parent, const QString& appId, const QString& path, const QString& iconName, X11Controller* controller)
    : Session(parent, appId, path)
    , m_item(new KStatusNotifierItem(this))
    , m_controller(controller)
{
    m_item->setStandardActionsEnabled(false);
    m_item->setIconByName(iconName);

    auto menu = new QMenu;
    auto stopAction = menu->addAction(QIcon::fromTheme(QStringLiteral("process-stop")), i18nc("@action:inmenu stops screen/window sharing", "End"));
    connect(stopAction, &QAction::triggered, this, &Session::close);
    m_item->setContextMenu(menu);
    m_item->setIsMenu(true);
}

ScreenCastSession::~ScreenCastSession() = default;

bool ScreenCastSession::multipleSources() const
{
    return m_multipleSources;
}

ScreenCastPortal::SourceType ScreenCastSession::types() const
{
    return m_types;
}

void ScreenCastSession::setPersistMode(ScreenCastPortal::PersistMode persistMode)
{
    m_persistMode = persistMode;
}

ScreenCastPortal::CursorModes ScreenCastSession::cursorMode() const
{
    return m_cursorMode;
}

void ScreenCastSession::setOptions(const QVariantMap& options)
{
    m_multipleSources = options.value(QStringLiteral("multiple")).toBool();
    m_cursorMode = ScreenCastPortal::CursorModes(options.value(QStringLiteral("cursor_mode")).toUInt());
    m_types = ScreenCastPortal::SourceType(options.value(QStringLiteral("types")).toUInt());

    if (m_types == 0) {
        m_types = ScreenCastPortal::Monitor;
    }
    // Drop virtual because the X11 backend does not advertise it.
    if (m_types == ScreenCastPortal::Virtual) {
        m_types = ScreenCastPortal::Monitor;
    }
    auto portal = qobject_cast<ScreenCastPortal*>(parent());
    const uint available = portal ? portal->AvailableSourceTypes() : ScreenCastPortal::Monitor | ScreenCastPortal::Window;
    m_types = ScreenCastPortal::SourceType(static_cast<uint>(m_types) & available);
}

void ScreenCastSession::setStreams(std::vector<std::unique_ptr<ScreencastingStream>>&& streams)
{
    Q_ASSERT(!streams.empty());
    m_streams = std::move(streams);

    m_item->setStandardActionsEnabled(false);
    if (qobject_cast<RemoteDesktopSession*>(this)) {
        refreshDescription();
    } else {
        const bool isWindow = m_streams[0]->metaData()[QLatin1String("source_type")] == ScreenCastPortal::Window;
        m_item->setToolTipSubTitle(i18ncp("%1 number of screens, %2 the app that receives them",
            "Sharing contents to %2",
            "%1 video streams to %2",
            m_streams.size(),
            Utils::applicationName(m_appId)));
        m_item->setTitle(i18nc("SNI title that indicates there's a process seeing our windows or screens", "Screen casting"));
        if (isWindow) {
            m_item->setOverlayIconByName(QStringLiteral("window"));
        } else {
            m_item->setOverlayIconByName(QStringLiteral("monitor"));
        }
    }
    m_item->setToolTipIconByName(m_item->overlayIconName());
    m_item->setToolTipTitle(m_item->title());

    for (const auto& s : m_streams) {
        connect(s.get(), &ScreencastingStream::closed, this, &ScreenCastSession::streamClosed);
    }
    m_item->setStatus(KStatusNotifierItem::Active);
}

void ScreenCastSession::setDescription(const QString& description)
{
    m_item->setToolTipSubTitle(description);
}

void ScreenCastSession::streamClosed()
{
    ScreencastingStream* stream = qobject_cast<ScreencastingStream*>(sender());
    std::erase_if(m_streams, [stream](const std::unique_ptr<ScreencastingStream>& s) {
        return s.get() == stream;
    });

    if (m_streams.empty()) {
        close();
    }
}

#include "moc_screencast.cpp"
