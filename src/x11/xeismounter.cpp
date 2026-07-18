#include "xeismounter.h"

#include "x11_debug.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <unordered_map>

#include <libeis.h>
#include <xkbcommon/xkbcommon.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

namespace {
constexpr uint ScrollStep = 120;

struct EisDeleter {
    void operator()(eis* context) const
    {
        if (context) {
            eis_unref(context);
        }
    }
};

struct EisEventDeleter {
    void operator()(eis_event* event) const
    {
        if (event) {
            eis_event_unref(event);
        }
    }
};

struct EisKeymapDeleter {
    void operator()(eis_keymap* keymap) const
    {
        if (keymap) {
            eis_keymap_unref(keymap);
        }
    }
};

struct EisRegionDeleter {
    void operator()(eis_region* region) const
    {
        if (region) {
            eis_region_unref(region);
        }
    }
};

struct XkbContextDeleter {
    void operator()(xkb_context* context) const
    {
        if (context) {
            xkb_context_unref(context);
        }
    }
};

struct XkbKeymapDeleter {
    void operator()(xkb_keymap* keymap) const
    {
        if (keymap) {
            xkb_keymap_unref(keymap);
        }
    }
};

using EisPtr = std::unique_ptr<eis, EisDeleter>;
using EisEventPtr = std::unique_ptr<eis_event, EisEventDeleter>;
using EisKeymapPtr = std::unique_ptr<eis_keymap, EisKeymapDeleter>;
using EisRegionPtr = std::unique_ptr<eis_region, EisRegionDeleter>;
using XkbContextPtr = std::unique_ptr<xkb_context, XkbContextDeleter>;
using XkbKeymapPtr = std::unique_ptr<xkb_keymap, XkbKeymapDeleter>;

QString errnoString(const char* operation, int err)
{
    const int positiveErrno = err < 0 ? -err : err;
    return QString::fromLatin1("%1 failed: %2").arg(QString::fromLatin1(operation), QString::fromLocal8Bit(std::strerror(positiveErrno)));
}

quint64 clientId(eis_client* client)
{
    return reinterpret_cast<quintptr>(client) & 0x7fffffff;
}

int createAnonymousFile(const QByteArray& data)
{
#ifdef __linux__
    int memfd = memfd_create("xdp-sonicde-eis-keymap", MFD_CLOEXEC);
    if (memfd >= 0) {
        qsizetype written = 0;
        while (written < data.size()) {
            const ssize_t rc = write(memfd, data.constData() + written, static_cast<size_t>(data.size() - written));
            if (rc < 0) {
                close(memfd);
                return -errno;
            }
            written += rc;
        }
        lseek(memfd, 0, SEEK_SET);
        return memfd;
    }
#endif
    QTemporaryFile file;
    if (!file.open()) {
        return -errno;
    }
    if (file.write(data) != data.size()) {
        return -EIO;
    }
    file.flush();
    const int fd = dup(file.handle());
    if (fd < 0) {
        return -errno;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

QByteArray defaultXkbKeymap()
{
    XkbContextPtr context(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
    if (!context) {
        return {};
    }
    xkb_rule_names names = {};
    XkbKeymapPtr keymap(xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!keymap) {
        return {};
    }
    char* raw = xkb_keymap_get_as_string(keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!raw) {
        return {};
    }
    QByteArray result(raw);
    std::free(raw);
    return result;
}

bool hasAnyCapability(eis_event* event)
{
    return eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER)
        || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER_ABSOLUTE)
        || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON)
        || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL)
        || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)
        || eis_event_seat_has_capability(event, EIS_DEVICE_CAP_TEXT);
}
}

class XEisMounter::Private {
public:
    struct DeviceState {
        eis_device* device = nullptr;
        bool sending = false;
        uint sequence = 1;
    };

    struct ClientState {
        int id = 0;
        Mode mode = Mode::RemoteDesktopReceiver;
        eis_client* client = nullptr;
        eis_seat* seat = nullptr;
        QList<DeviceState> devices;
    };

    explicit Private(XEisMounter* q)
        : q(q)
    {
    }

    ~Private()
    {
        teardown();
    }

    void initialize()
    {
        context.reset(eis_new(q));
        if (!context) {
            fail(QStringLiteral("Failed to create libeis context"));
            return;
        }
        eis_log_set_priority(context.get(), EIS_LOG_PRIORITY_WARNING);
        const int rc = eis_setup_backend_fd(context.get());
        if (rc < 0) {
            fail(errnoString("eis_setup_backend_fd", rc));
            context.reset();
            return;
        }
        const int fd = eis_get_fd(context.get());
        if (fd < 0) {
            fail(errnoString("eis_get_fd", fd));
            context.reset();
            return;
        }
        notifier = new QSocketNotifier(fd, QSocketNotifier::Read, q);
        QObject::connect(notifier, &QSocketNotifier::activated, q, [this]() {
            dispatch();
        });
        valid = true;
    }

    QDBusUnixFileDescriptor attach(Mode mode)
    {
        if (!valid || !context) {
            fail(QStringLiteral("Cannot attach EIS client because the libeis backend is not available"));
            return {};
        }
        if (hasFixedMode && fixedMode != mode) {
            fail(QStringLiteral("An EIS backend cannot mix sender and receiver clients"));
            return {};
        }
        const int fd = eis_backend_fd_add_client(context.get());
        if (fd < 0) {
            fail(errnoString("eis_backend_fd_add_client", fd));
            return {};
        }
        pendingModes.append(mode);
        fixedMode = mode;
        hasFixedMode = true;
        return QDBusUnixFileDescriptor(fd);
    }

    void dispatch()
    {
        if (!context) {
            return;
        }
        eis_dispatch(context.get());
        while (true) {
            EisEventPtr event(eis_get_event(context.get()));
            if (!event) {
                break;
            }
            handleEvent(event.get());
        }
    }

    void handleEvent(eis_event* event)
    {
        switch (eis_event_get_type(event)) {
        case EIS_EVENT_CLIENT_CONNECT:
            handleClientConnect(event);
            break;
        case EIS_EVENT_CLIENT_DISCONNECT:
            handleClientDisconnect(event);
            break;
        case EIS_EVENT_SEAT_BIND:
        case EIS_EVENT_SEAT_DEVICE_REQUESTED:
            handleSeatBind(event);
            break;
        case EIS_EVENT_DEVICE_CLOSED:
            handleDeviceClosed(event);
            break;
        case EIS_EVENT_DEVICE_START_EMULATING:
            if (ClientState* client = clientForEvent(event)) {
                Q_EMIT q->receiverStarted(client->id, eis_event_emulating_get_sequence(event));
            }
            break;
        case EIS_EVENT_DEVICE_STOP_EMULATING:
            if (ClientState* client = clientForEvent(event)) {
                Q_EMIT q->receiverStopped(client->id);
            }
            break;
        case EIS_EVENT_POINTER_MOTION:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->pointerMotionReceived(client->id, QSizeF(eis_event_pointer_get_dx(event), eis_event_pointer_get_dy(event)));
            }
            break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->pointerMotionAbsoluteReceived(client->id, QPointF(eis_event_pointer_get_absolute_x(event), eis_event_pointer_get_absolute_y(event)));
            }
            break;
        case EIS_EVENT_BUTTON_BUTTON:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->pointerButtonReceived(client->id, eis_event_button_get_button(event), eis_event_button_get_is_press(event));
            }
            break;
        case EIS_EVENT_SCROLL_DELTA:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->pointerAxisReceived(client->id, QPointF(eis_event_scroll_get_dx(event), eis_event_scroll_get_dy(event)));
            }
            break;
        case EIS_EVENT_SCROLL_DISCRETE:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->pointerAxisDiscreteReceived(client->id, QPoint(eis_event_scroll_get_discrete_dx(event), eis_event_scroll_get_discrete_dy(event)));
            }
            break;
        case EIS_EVENT_KEYBOARD_KEY:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->keyReceived(client->id, eis_event_keyboard_get_key(event), eis_event_keyboard_get_key_is_press(event));
            }
            break;
        case EIS_EVENT_TEXT_KEYSYM:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->keySymReceived(client->id, eis_event_text_get_keysym(event), eis_event_text_get_keysym_is_press(event));
            }
            break;
        case EIS_EVENT_FRAME:
            if (ClientState* client = inboundClientForEvent(event)) {
                Q_EMIT q->frameReceived(client->id, eis_event_get_time(event));
            }
            break;
        default:
            break;
        }
    }

    void handleClientConnect(eis_event* event)
    {
        eis_client* client = eis_event_get_client(event);
        if (!client) {
            return;
        }
        if (pendingModes.isEmpty()) {
            eis_client_disconnect(client);
            fail(QStringLiteral("Rejected unexpected EIS client without an attach request"));
            return;
        }

        const Mode mode = pendingModes.takeFirst();
        const bool clientIsSender = eis_client_is_sender(client);
        if (!XEisMounter::clientRoleMatchesMode(mode, clientIsSender)) {
            eis_client_disconnect(client);
            fail(QString::fromLatin1("Rejected EIS client with invalid role for %1 mode").arg(mode == Mode::RemoteDesktopReceiver ? QStringLiteral("RemoteDesktop") : QStringLiteral("InputCapture")));
            return;
        }

        auto state = std::make_unique<ClientState>();
        state->id = static_cast<int>(clientId(client));
        state->mode = mode;
        state->client = client;

        eis_client_connect(client);
        state->seat = eis_client_new_seat(client, "default");
        if (!state->seat) {
            eis_client_disconnect(client);
            fail(QStringLiteral("Failed to create EIS seat for client"));
            return;
        }
        configureSeat(state->seat);
        eis_seat_add(state->seat);

        const int id = state->id;
        clientsByClient.insert(client, state.get());
        clientsById.insert(id, state.get());
        clients.push_back(std::move(state));
        Q_EMIT q->clientConnected(id);
    }

    void handleClientDisconnect(eis_event* event)
    {
        eis_client* client = eis_event_get_client(event);
        ClientState* state = clientsByClient.value(client, nullptr);
        if (!state) {
            return;
        }
        const int id = state->id;
        clientsByClient.remove(client);
        clientsById.remove(id);
        std::erase_if(clients, [state](const std::unique_ptr<ClientState>& item) {
            return item.get() == state;
        });
        Q_EMIT q->clientDisconnected(id);
    }

    void configureSeat(eis_seat* seat)
    {
        if (pointerEnabled) {
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
        }
        if (keyboardEnabled) {
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_TEXT);
        }
    }

    void handleSeatBind(eis_event* event)
    {
        eis_seat* seat = eis_event_get_seat(event);
        if (!seat || !hasAnyCapability(event)) {
            return;
        }
        ClientState* client = clientsByClient.value(eis_seat_get_client(seat), nullptr);
        if (!client) {
            return;
        }
        createDevice(client, event);
    }

    void createDevice(ClientState* client, eis_event* event)
    {
        eis_device* device = eis_seat_new_device(client->seat);
        if (!device) {
            fail(QStringLiteral("Failed to create EIS device"));
            return;
        }

        eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
        eis_device_configure_name(device, client->mode == Mode::RemoteDesktopReceiver ? "Remote desktop input" : "Captured input");

        if (pointerEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
        }
        if (pointerEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER_ABSOLUTE)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            addRegions(device);
        }
        if (pointerEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
        }
        if (pointerEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
        }
        if (keyboardEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
            addKeymap(device);
        }
        if (keyboardEnabled && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_TEXT)) {
            eis_device_configure_capability(device, EIS_DEVICE_CAP_TEXT);
        }

        eis_device_add(device);
        eis_device_resume(device);
        client->devices.append(DeviceState{device});
    }

    void addRegions(eis_device* device)
    {
        const QList<QRect> effectiveRegions = regions.isEmpty() ? currentScreenRegions() : regions;
        for (const QRect& rect : XEisMounter::normalizedRegions(effectiveRegions)) {
            EisRegionPtr region(eis_device_new_region(device));
            if (!region) {
                continue;
            }
            eis_region_set_offset(region.get(), static_cast<uint32_t>(std::max(0, rect.x())), static_cast<uint32_t>(std::max(0, rect.y())));
            eis_region_set_size(region.get(), static_cast<uint32_t>(rect.width()), static_cast<uint32_t>(rect.height()));
            eis_region_add(region.get());
        }
    }

    void addKeymap(eis_device* device)
    {
        if (serializedKeymap.isEmpty()) {
            serializedKeymap = defaultXkbKeymap();
        }
        if (serializedKeymap.isEmpty()) {
            qCWarning(XdgDesktopPortalKdeX11) << "EIS keyboard device has no keymap";
            return;
        }
        const int fd = createAnonymousFile(serializedKeymap);
        if (fd < 0) {
            qCWarning(XdgDesktopPortalKdeX11) << errnoString("keymap memfd", fd);
            return;
        }
        EisKeymapPtr keymap(eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd, static_cast<size_t>(serializedKeymap.size())));
        close(fd);
        if (!keymap) {
            qCWarning(XdgDesktopPortalKdeX11) << "Failed to create EIS keymap";
            return;
        }
        eis_keymap_add(keymap.get());
    }

    void handleDeviceClosed(eis_event* event)
    {
        eis_device* device = eis_event_get_device(event);
        if (!device) {
            return;
        }
        ClientState* client = clientsByClient.value(eis_device_get_client(device), nullptr);
        if (!client) {
            return;
        }
        for (int i = 0; i < client->devices.size(); ++i) {
            if (client->devices[i].device == device) {
                client->devices.removeAt(i);
                return;
            }
        }
    }

    ClientState* clientForEvent(eis_event* event) const
    {
        if (eis_device* device = eis_event_get_device(event)) {
            return clientsByClient.value(eis_device_get_client(device), nullptr);
        }
        if (eis_seat* seat = eis_event_get_seat(event)) {
            return clientsByClient.value(eis_seat_get_client(seat), nullptr);
        }
        if (eis_client* client = eis_event_get_client(event)) {
            return clientsByClient.value(client, nullptr);
        }
        return nullptr;
    }

    ClientState* inboundClientForEvent(eis_event* event) const
    {
        ClientState* client = clientForEvent(event);
        if (!client || client->mode != Mode::RemoteDesktopReceiver) {
            return nullptr;
        }
        return client;
    }

    QList<QRect> currentScreenRegions() const
    {
        QList<QRect> result;
        if (qGuiApp) {
            const auto screens = qGuiApp->screens();
            for (QScreen* screen : screens) {
                result.append(screen->geometry());
            }
        }
        if (result.isEmpty()) {
            result.append(QRect(0, 0, 1, 1));
        }
        return result;
    }

    DeviceState* outputDevice(enum eis_device_capability cap)
    {
        for (const auto& client : clients) {
            if (client->mode != Mode::InputCaptureSender) {
                continue;
            }
            for (DeviceState& device : client->devices) {
                if (device.device && eis_device_has_capability(device.device, cap)) {
                    if (!device.sending) {
                        eis_device_start_emulating(device.device, device.sequence++);
                        device.sending = true;
                    }
                    return &device;
                }
            }
        }
        return nullptr;
    }

    void frame(DeviceState* device)
    {
        if (device && context && device->device) {
            eis_device_frame(device->device, eis_now(context.get()));
        }
    }

    void teardown()
    {
        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
            notifier = nullptr;
        }
        for (const auto& client : clients) {
            if (client->client) {
                eis_client_disconnect(client->client);
            }
        }
        clients.clear();
        clientsByClient.clear();
        clientsById.clear();
        pendingModes.clear();
        context.reset();
        valid = false;
    }

    void fail(const QString& error)
    {
        lastError = error;
        qCWarning(XdgDesktopPortalKdeX11) << error;
        Q_EMIT q->errorOccurred(error);
    }

    XEisMounter* q = nullptr;
    EisPtr context;
    QSocketNotifier* notifier = nullptr;
    bool valid = false;
    QString lastError;
    QList<Mode> pendingModes;
    bool pointerEnabled = true;
    bool keyboardEnabled = true;
    Mode fixedMode = Mode::RemoteDesktopReceiver;
    bool hasFixedMode = false;
    QList<QRect> regions;
    QByteArray serializedKeymap;
    std::vector<std::unique_ptr<ClientState>> clients;
    QHash<eis_client*, ClientState*> clientsByClient;
    QHash<int, ClientState*> clientsById;
};

XEisMounter::XEisMounter(QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
    d->initialize();
}

XEisMounter::~XEisMounter()
{
    delete d;
}

QDBusUnixFileDescriptor XEisMounter::attachSender()
{
    return d->attach(Mode::InputCaptureSender);
}

QDBusUnixFileDescriptor XEisMounter::attachReceiver()
{
    return d->attach(Mode::RemoteDesktopReceiver);
}

void XEisMounter::setRegions(const QList<QRect>& regions)
{
    d->regions = normalizedRegions(regions);
}

void XEisMounter::setCapabilities(bool pointer, bool keyboard)
{
    d->pointerEnabled = pointer;
    d->keyboardEnabled = keyboard;
}

bool XEisMounter::isValid() const
{
    return d->valid;
}

QString XEisMounter::lastError() const
{
    return d->lastError;
}

int XEisMounter::clientCount() const
{
    return static_cast<int>(d->clients.size());
}

bool XEisMounter::clientRoleMatchesMode(Mode mode, bool clientIsSender)
{
    switch (mode) {
    case Mode::RemoteDesktopReceiver:
        return clientIsSender;
    case Mode::InputCaptureSender:
        return !clientIsSender;
    }
    return false;
}

QList<QRect> XEisMounter::normalizedRegions(const QList<QRect>& regions)
{
    QList<QRect> result;
    for (const QRect& region : regions) {
        if (!region.isValid() || region.isEmpty()) {
            continue;
        }
        if (!result.contains(region)) {
            result.append(region);
        }
    }
    if (result.isEmpty()) {
        result.append(QRect(0, 0, 1, 1));
    }
    return result;
}

void XEisMounter::sendPointerMotion(const QSizeF& delta)
{
    if (delta.isNull()) {
        return;
    }
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_POINTER)) {
        eis_device_pointer_motion(device->device, delta.width(), delta.height());
        d->frame(device);
    }
}

void XEisMounter::sendPointerMotionAbsolute(const QPointF& position)
{
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_POINTER_ABSOLUTE)) {
        eis_device_pointer_motion_absolute(device->device, position.x(), position.y());
        d->frame(device);
    }
}

void XEisMounter::sendPointerButton(uint linuxButton, bool pressed)
{
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_BUTTON)) {
        eis_device_button_button(device->device, linuxButton, pressed);
        d->frame(device);
    }
}

void XEisMounter::sendPointerAxis(qreal dx, qreal dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_SCROLL)) {
        eis_device_scroll_delta(device->device, dx, dy);
        d->frame(device);
    }
}

void XEisMounter::sendPointerAxisDiscrete(Qt::Orientation axis, int steps)
{
    if (steps == 0) {
        return;
    }
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_SCROLL)) {
        eis_device_scroll_discrete(device->device, axis == Qt::Horizontal ? steps * static_cast<int>(ScrollStep) : 0, axis == Qt::Vertical ? steps * static_cast<int>(ScrollStep) : 0);
        d->frame(device);
    }
}

void XEisMounter::sendKey(uint evdevKeycode, bool pressed)
{
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_KEYBOARD)) {
        eis_device_keyboard_key(device->device, evdevKeycode, pressed);
        d->frame(device);
    }
}

void XEisMounter::sendKeySym(uint keysym, bool pressed)
{
    if (auto* device = d->outputDevice(EIS_DEVICE_CAP_TEXT)) {
        eis_device_text_keysym(device->device, keysym, pressed);
        d->frame(device);
    }
}

void XEisMounter::stopSending()
{
    for (const auto& client : d->clients) {
        for (auto& device : client->devices) {
            if (device.sending && device.device) {
                eis_device_stop_emulating(device.device);
                device.sending = false;
            }
        }
    }
}

void XEisMounter::teardown()
{
    d->teardown();
}
