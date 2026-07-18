#include "x11worker.h"
#include "x11_debug.h"
#include "x11capture.h"

#include <QSocketNotifier>
#include <QTimer>

using namespace Qt::StringLiterals;

#include <atomic>
#include <map>
#include <memory>

extern "C" {
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#include <xcb/xtest.h>
}

#include <xcb/xcb.h>
#include <xcb/xcb_event.h>

class X11WorkerPrivate {
public:
    xcb_connection_t* connection = nullptr;
    int defaultScreen = 0;
    xcb_screen_t* screen = nullptr;
    QSocketNotifier* notifier = nullptr;
    QTimer* refreshTimer = nullptr;
    std::unique_ptr<X11Capture> capture;
    std::map<QByteArray, xcb_atom_t> atoms;

    QRect randrRootRect;
    QList<X11Types::OutputDescriptor> outputs;
    QList<X11Types::WindowDescriptor> windows;
    X11Types::CapabilitySnapshot capabilities;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shuttingDown{false};

    bool xfixesAvailable = false;
    bool xtestAvailable = false;
    bool shmAvailable = false;
    bool compositeAvailable = false;
    bool damageAvailable = false;
    bool randrAvailable = false;
    bool xi2Available = false;
};

static void internAtoms(xcb_connection_t* c, std::map<QByteArray, xcb_atom_t>& atoms)
{
    static const QList<QByteArray> names = {
        "_NET_CLIENT_LIST_STACKING",
        "_NET_CLIENT_LIST",
        "_NET_ACTIVE_WINDOW",
        "_NET_WM_NAME",
        "WM_NAME",
        "WM_CLASS",
        "_NET_WM_PID",
        "_NET_WM_STATE",
        "_NET_WM_STATE_SKIP_TASKBAR",
        "_NET_WM_STATE_SKIP_PAGER",
        "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK",
        "WM_STATE",
        "_XROOTPMAP_ID",
        "ESETROOT_PMAP_ID",
        "CLIPBOARD",
        "TARGETS",
        "TIMESTAMP",
        "MULTIPLE",
        "INCR",
        "UTF8_STRING",
    };
    std::vector<xcb_intern_atom_cookie_t> cookies;
    cookies.reserve(names.size());
    for (const auto& name : names) {
        cookies.push_back(xcb_intern_atom(c, 1, name.size(), name.constData()));
    }
    for (int i = 0; i < names.size(); ++i) {
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookies[i], nullptr);
        if (reply) {
            atoms[names[i]] = reply->atom;
            free(reply);
        }
    }
}

static QByteArray readPropertyString(xcb_connection_t* c, xcb_window_t window, xcb_atom_t atom)
{
    xcb_get_property_cookie_t ck = xcb_get_property(c, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(c, ck, nullptr);
    if (!reply) {
        return {};
    }
    QByteArray out;
    if (reply->format == 8) {
        out = QByteArray(reinterpret_cast<const char*>(xcb_get_property_value(reply)),
            xcb_get_property_value_length(reply));
    }
    free(reply);
    return out;
}

static quint32 readPropertyCardinal(xcb_connection_t* c, xcb_window_t window, xcb_atom_t atom)
{
    xcb_get_property_cookie_t ck = xcb_get_property(c, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(c, ck, nullptr);
    if (!reply || xcb_get_property_value_length(reply) < 4) {
        free(reply);
        return 0;
    }
    quint32 v = *reinterpret_cast<quint32*>(xcb_get_property_value(reply));
    free(reply);
    return v;
}

static bool hasState(xcb_connection_t* c, xcb_window_t window, xcb_atom_t stateProperty, xcb_atom_t stateAtom)
{
    xcb_get_property_cookie_t ck = xcb_get_property(c, 0, window, stateProperty, XCB_ATOM_ATOM, 0, UINT_MAX);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(c, ck, nullptr);
    if (!reply) {
        return false;
    }
    bool found = false;
    int length = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
    xcb_atom_t* atoms = reinterpret_cast<xcb_atom_t*>(xcb_get_property_value(reply));
    for (int i = 0; i < length; ++i) {
        if (atoms[i] == stateAtom) {
            found = true;
            break;
        }
    }
    free(reply);
    return found;
}

static X11Types::WindowDescriptor buildWindowDescriptor(xcb_connection_t* c, xcb_window_t xid, const std::map<QByteArray, xcb_atom_t>& atoms)
{
    X11Types::WindowDescriptor w;
    w.xid = xid;
    w.sourceKey = QString::number(xid);
    w.title = QString::fromUtf8(readPropertyString(c, xid, atoms.at("_NET_WM_NAME")));
    if (w.title.isEmpty()) {
        w.title = QString::fromUtf8(readPropertyString(c, xid, atoms.at("WM_NAME")));
    }
    QByteArray wmClass = readPropertyString(c, xid, atoms.at("WM_CLASS"));
    if (!wmClass.isEmpty()) {
        int nul = wmClass.indexOf('\0');
        if (nul > 0) {
            w.wmClass = QString::fromLatin1(wmClass.mid(nul + 1));
        } else {
            w.wmClass = QString::fromLatin1(wmClass);
        }
    }
    w.appId = w.wmClass.trimmed().toLower();
    w.pid = static_cast<qint64>(readPropertyCardinal(c, xid, atoms.at("_NET_WM_PID")));

    auto it = atoms.find("_NET_WM_STATE_SKIP_TASKBAR");
    w.skipTaskbar = (it != atoms.end()) && hasState(c, xid, atoms.at("_NET_WM_STATE"), it->second);
    auto it2 = atoms.find("_NET_WM_STATE_SKIP_PAGER");
    w.skipSwitcher = (it2 != atoms.end()) && hasState(c, xid, atoms.at("_NET_WM_STATE"), it2->second);

    xcb_get_window_attributes_reply_t* attributes = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, xid), nullptr);
    if (!attributes || attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY || attributes->override_redirect) {
        free(attributes);
        return w;
    }
    w.mapped = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attributes);

    xcb_get_geometry_cookie_t ck = xcb_get_geometry(c, xid);
    xcb_get_geometry_reply_t* geo = xcb_get_geometry_reply(c, ck, nullptr);
    if (geo) {
        QPoint rootPosition(geo->x, geo->y);
        if (auto* translated = xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, xid, geo->root, 0, 0), nullptr)) {
            rootPosition = QPoint(translated->dst_x, translated->dst_y);
            free(translated);
        }
        w.nativeGeometry = QRect(rootPosition, QSize(geo->width, geo->height));
        free(geo);
    }
    return w;
}

static QList<X11Types::WindowDescriptor> buildWindowList(xcb_connection_t* c, xcb_window_t root, const std::map<QByteArray, xcb_atom_t>& atoms)
{
    QList<X11Types::WindowDescriptor> result;
    QByteArray listData;
    xcb_window_t rootWin = root;
    xcb_get_property_cookie_t ck = xcb_get_property(c, 0, rootWin, atoms.at("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW, 0, UINT_MAX);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(c, ck, nullptr);
    if (!reply || xcb_get_property_value_length(reply) == 0) {
        if (reply) {
            free(reply);
        }
        ck = xcb_get_property(c, 0, rootWin, atoms.at("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW, 0, UINT_MAX);
        reply = xcb_get_property_reply(c, ck, nullptr);
    }
    xcb_window_t activeWindow = XCB_WINDOW_NONE;
    if (auto* active = xcb_get_property_reply(c, xcb_get_property(c, 0, root, atoms.at("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW, 0, 1), nullptr)) {
        if (xcb_get_property_value_length(active) >= int(sizeof(xcb_window_t))) {
            activeWindow = *static_cast<xcb_window_t*>(xcb_get_property_value(active));
        }
        free(active);
    }
    if (reply) {
        int length = xcb_get_property_value_length(reply) / sizeof(xcb_window_t);
        auto* wids = reinterpret_cast<xcb_window_t*>(xcb_get_property_value(reply));
        for (int i = 0; i < length; ++i) {
            auto descriptor = buildWindowDescriptor(c, wids[i], atoms);
            descriptor.active = descriptor.xid == activeWindow;
            if (descriptor.mapped && !descriptor.skipTaskbar && !descriptor.skipSwitcher && !descriptor.title.isEmpty()) {
                result.push_back(std::move(descriptor));
            }
        }
        free(reply);
    }
    return result;
}

static QList<X11Types::OutputDescriptor> queryRandROutputs(xcb_connection_t* c, xcb_window_t root)
{
    QList<X11Types::OutputDescriptor> out;
    xcb_randr_get_screen_resources_cookie_t srck = xcb_randr_get_screen_resources(c, root);
    xcb_randr_get_screen_resources_reply_t* src = xcb_randr_get_screen_resources_reply(c, srck, nullptr);
    if (!src) {
        return out;
    }
    int len = xcb_randr_get_screen_resources_outputs_length(src);
    xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_outputs(src);
    for (int i = 0; i < len; ++i) {
        X11Types::OutputDescriptor d;
        xcb_randr_get_output_info_cookie_t ck = xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME);
        xcb_randr_get_output_info_reply_t* info = xcb_randr_get_output_info_reply(c, ck, nullptr);
        if (!info) {
            continue;
        }
        d.name = QString::fromUtf8(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(info)), xcb_randr_get_output_info_name_length(info));
        d.uniqueId = d.name;
        d.connectorType = QString::fromUtf8(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(info)));
        // Determine CRTC geometry
        if (info->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_cookie_t cck = xcb_randr_get_crtc_info(c, info->crtc, XCB_CURRENT_TIME);
            xcb_randr_get_crtc_info_reply_t* cinfo = xcb_randr_get_crtc_info_reply(c, cck, nullptr);
            if (cinfo) {
                d.nativeGeometry = QRect(cinfo->x, cinfo->y, cinfo->width, cinfo->height);
                d.display = d.name;
                free(cinfo);
            }
        }
        free(info);
        if (!d.nativeGeometry.isEmpty()) {
            d.isLaptop = d.name.startsWith(u"eDP"_s, Qt::CaseInsensitive) || d.name.startsWith(u"LVDS"_s, Qt::CaseInsensitive) || d.name.startsWith(u"DSI"_s, Qt::CaseInsensitive);
            d.isTelevision = d.name.contains(u"TV"_s, Qt::CaseInsensitive);
            out.push_back(d);
        }
    }
    free(src);
    return out;
}

X11Worker::X11Worker(QObject* parent)
    : QObject(parent)
    , d(new X11WorkerPrivate)
{
}

X11Worker::~X11Worker()
{
    shutdown();
    delete d;
}

bool X11Worker::isInitialized() const
{
    return d->initialized.load();
}

bool X11Worker::isShuttingDown() const
{
    return d->shuttingDown.load();
}

void X11Worker::initialize()
{
    if (d->initialized.exchange(true)) {
        return;
    }
    d->connection = xcb_connect(nullptr, &d->defaultScreen);
    if (!d->connection || xcb_connection_has_error(d->connection)) {
        Q_EMIT error(QStringLiteral("Failed to open XCB connection"));
        d->initialized = false;
        return;
    }
    int pref = 5;
    Q_UNUSED(pref)
    xcb_prefetch_maximum_request_length(d->connection);
    auto setup = xcb_get_setup(d->connection);
    auto it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < d->defaultScreen; ++i) {
        xcb_screen_next(&it);
    }
    d->screen = it.data;

    // Probe extensions
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_xfixes_id);
        d->xfixesAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_test_id);
        d->xtestAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_shm_id);
        d->shmAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_composite_id);
        d->compositeAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_damage_id);
        d->damageAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_randr_id);
        d->randrAvailable = xc && xc->present;
    }
    {
        const auto* xc = xcb_get_extension_data(d->connection, &xcb_input_id);
        d->xi2Available = xc && xc->present;
    }

    internAtoms(d->connection, d->atoms);

    if (d->screen) {
        d->randrRootRect = QRect(0, 0, d->screen->width_in_pixels, d->screen->height_in_pixels);
    }

    if (d->randrAvailable && d->screen) {
        d->outputs = queryRandROutputs(d->connection, d->screen->root);
    }
    d->windows = buildWindowList(d->connection, d->screen ? d->screen->root : 0, d->atoms);
    d->capture = std::make_unique<X11Capture>(d->connection, d->screen, false);

    // Capability snapshot
    auto& cap = d->capabilities;
    cap.screencastMonitor = d->screen != nullptr;
    cap.screencastWindow = d->compositeAvailable && d->damageAvailable;
    cap.screencastCursorEmbedded = d->xfixesAvailable;
    cap.screencastCursorMetadata = false;
    cap.pointerInput = d->xtestAvailable;
    cap.keyboardInput = d->xtestAvailable;
    cap.inputCaptureKeyboard = d->xfixesAvailable && d->xi2Available;
    cap.inputCapturePointer = d->xfixesAvailable && d->xi2Available;
    cap.inputCaptureTouch = false;
    if (!cap.screencastMonitor) {
        cap.disabledReasons << QStringLiteral("no root screen");
    }
    if (!cap.screencastWindow) {
        cap.disabledReasons << QStringLiteral("Composite/Damage unavailable");
    }
    if (!cap.pointerInput) {
        cap.disabledReasons << QStringLiteral("XTEST unavailable");
    }
    if (!cap.inputCapturePointer) {
        cap.disabledReasons << QStringLiteral("XFixes/XI2 unavailable");
    }

    Q_EMIT outputsReady(d->outputs);
    Q_EMIT windowsReady(d->windows);
    Q_EMIT capabilityReady(cap);

    int fd = xcb_get_file_descriptor(d->connection);
    if (fd >= 0) {
        d->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(d->notifier, &QSocketNotifier::activated, this, [this](QSocketDescriptor) {
            // Drain events non-blockingly
            if (d->shuttingDown) {
                return;
            }
            xcb_generic_event_t* e = nullptr;
            while ((e = xcb_poll_for_event(d->connection)) != nullptr) {
                const uint8_t type = e->response_type & ~0x80;
                if (type == XCB_PROPERTY_NOTIFY || type == XCB_CREATE_NOTIFY || type == XCB_DESTROY_NOTIFY || type == XCB_MAP_NOTIFY || type == XCB_UNMAP_NOTIFY || type == XCB_CONFIGURE_NOTIFY) {
                    refreshWindows();
                }
                free(e);
            }
        });
    }
    d->refreshTimer = new QTimer(this);
    d->refreshTimer->setInterval(1000);
    connect(d->refreshTimer, &QTimer::timeout, this, [this] {
        refreshOutputs();
        refreshWindows();
    });
    d->refreshTimer->start();
}

void X11Worker::refreshOutputs()
{
    if (!d->connection) {
        return;
    }
    if (d->randrAvailable && d->screen) {
        d->outputs = queryRandROutputs(d->connection, d->screen->root);
    }
    Q_EMIT outputsReady(d->outputs);
}

void X11Worker::refreshWindows()
{
    if (!d->connection || !d->screen) {
        return;
    }
    d->windows = buildWindowList(d->connection, d->screen->root, d->atoms);
    Q_EMIT windowsReady(d->windows);
}

void X11Worker::queryCapabilities()
{
    Q_EMIT capabilityReady(d->capabilities);
}

void X11Worker::shutdown()
{
    if (d->shuttingDown.exchange(true)) {
        return;
    }
    if (d->notifier) {
        d->notifier->setEnabled(false);
        delete d->notifier;
        d->notifier = nullptr;
    }
    if (d->refreshTimer) {
        d->refreshTimer->stop();
        delete d->refreshTimer;
        d->refreshTimer = nullptr;
    }
    d->capture.reset();
    if (d->connection) {
        xcb_disconnect(d->connection);
        d->connection = nullptr;
    }
}

QImage X11Worker::grabWindow(quint64 xid, bool includeCursor)
{
    if (d->shuttingDown || !d->initialized) {
        return {};
    }
    return d->capture ? d->capture->captureWindow(xid, includeCursor) : QImage{};
}

QImage X11Worker::grabWorkspace(bool includeCursor)
{
    if (d->shuttingDown || !d->initialized) {
        return {};
    }
    return d->capture ? d->capture->captureWorkspace(includeCursor) : QImage{};
}

QImage X11Worker::grabOutput(const QString& outputUniqueId, bool includeCursor)
{
    if (d->shuttingDown || !d->initialized) {
        return {};
    }
    return d->capture ? d->capture->captureOutput(outputUniqueId, includeCursor) : QImage{};
}

QImage X11Worker::grabActiveWindow(bool includeCursor)
{
    if (d->shuttingDown || !d->initialized) {
        return {};
    }
    return d->capture ? d->capture->captureActiveWindow(includeCursor) : QImage{};
}
