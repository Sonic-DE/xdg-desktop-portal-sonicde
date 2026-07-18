#include "x11input.h"

#include "x11_debug.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSizeF>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <linux/input-event-codes.h>
#include <xcb/xtest.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

namespace {
constexpr int XKeycodeOffset = 8;
constexpr int XKeycodeMin = 8;
constexpr int XKeycodeMax = 255;
constexpr int WheelStep = 120;

struct XcbErrorDeleter {
    void operator()(xcb_generic_error_t* error) const
    {
        std::free(error);
    }
};

struct XcbVersionReplyDeleter {
    void operator()(xcb_test_get_version_reply_t* reply) const
    {
        std::free(reply);
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

struct XkbStateDeleter {
    void operator()(xkb_state* state) const
    {
        if (state) {
            xkb_state_unref(state);
        }
    }
};

using XcbErrorPtr = std::unique_ptr<xcb_generic_error_t, XcbErrorDeleter>;
using XcbVersionReplyPtr = std::unique_ptr<xcb_test_get_version_reply_t, XcbVersionReplyDeleter>;
using XkbContextPtr = std::unique_ptr<xkb_context, XkbContextDeleter>;
using XkbKeymapPtr = std::unique_ptr<xkb_keymap, XkbKeymapDeleter>;
using XkbStatePtr = std::unique_ptr<xkb_state, XkbStateDeleter>;

QString xcbErrorString(const char* operation, const xcb_generic_error_t* error)
{
    if (!error) {
        return QString::fromLatin1("%1 failed").arg(QString::fromLatin1(operation));
    }
    return QString::fromLatin1("%1 failed with X11 error %2 (major %3, minor %4)")
        .arg(QString::fromLatin1(operation))
        .arg(error->error_code)
        .arg(error->major_code)
        .arg(error->minor_code);
}

int buttonForAxis(Qt::Orientation axis, int direction)
{
    if (axis == Qt::Vertical) {
        return direction > 0 ? 4 : 5;
    }
    return direction > 0 ? 6 : 7;
}
}

class X11Input::Private {
public:
    explicit Private(X11Input* q)
        : q(q)
    {
    }

    ~Private()
    {
        state.reset();
        keymap.reset();
        context.reset();
        if (connection && ownership == ConnectionOwnership::Owned) {
            xcb_disconnect(connection);
        }
    }

    void initialize(xcb_connection_t* injectedConnection, ConnectionOwnership injectedOwnership)
    {
        connection = injectedConnection;
        ownership = injectedOwnership;
        if (!connection) {
            int screen = 0;
            connection = xcb_connect(nullptr, &screen);
            ownership = ConnectionOwnership::Owned;
        }

        if (!connection || xcb_connection_has_error(connection)) {
            fail(QStringLiteral("Failed to connect to the X11 server"));
            return;
        }

        const xcb_setup_t* setup = xcb_get_setup(connection);
        if (!setup) {
            fail(QStringLiteral("Failed to query X11 setup"));
            return;
        }
        minKeycode = setup->min_keycode;
        maxKeycode = setup->max_keycode;

        xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
        if (it.rem) {
            root = it.data->root;
            rootGeometry = QRect(0, 0, it.data->width_in_pixels, it.data->height_in_pixels);
        }

        xcb_generic_error_t* rawError = nullptr;
        XcbVersionReplyPtr versionReply(xcb_test_get_version_reply(connection,
            xcb_test_get_version(connection, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION),
            &rawError));
        XcbErrorPtr versionError(rawError);
        if (!versionReply || versionError) {
            fail(xcbErrorString("XTEST probe", versionError.get()));
            return;
        }
        xtestAvailable = true;

        uint16_t xkbMajor = 0;
        uint16_t xkbMinor = 0;
        if (!xkb_x11_setup_xkb_extension(connection,
                XKB_X11_MIN_MAJOR_XKB_VERSION,
                XKB_X11_MIN_MINOR_XKB_VERSION,
                XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                &xkbMajor,
                &xkbMinor,
                nullptr,
                nullptr)) {
            fail(QStringLiteral("Failed to initialize the XKB X11 extension"));
            return;
        }

        context.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if (!context) {
            fail(QStringLiteral("Failed to create XKB context"));
            return;
        }
        keyboardDevice = xkb_x11_get_core_keyboard_device_id(connection);
        if (keyboardDevice < 0) {
            fail(QStringLiteral("Failed to query the XKB core keyboard device"));
            return;
        }
        keymap.reset(xkb_x11_keymap_new_from_device(context.get(), connection, keyboardDevice, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!keymap) {
            fail(QStringLiteral("Failed to create XKB keymap from the X11 keyboard"));
            return;
        }
        state.reset(xkb_x11_state_new_from_device(keymap.get(), connection, keyboardDevice));
        if (!state) {
            fail(QStringLiteral("Failed to create XKB state from the X11 keyboard"));
            return;
        }

        available = true;
        Q_EMIT q->availabilityChanged(true);
    }

    bool checkAvailable(const char* operation)
    {
        if (!available || !connection || !xtestAvailable) {
            fail(QString::fromLatin1("%1 requested before XTEST input injection is available").arg(QString::fromLatin1(operation)));
            return false;
        }
        if (xcb_connection_has_error(connection)) {
            fail(QString::fromLatin1("%1 failed because the X11 connection is in an error state").arg(QString::fromLatin1(operation)));
            return false;
        }
        return true;
    }

    bool sendFakeInput(uint8_t type, uint8_t detail, int16_t x, int16_t y, xcb_window_t eventRoot, const char* operation)
    {
        if (!checkAvailable(operation)) {
            return false;
        }
        XcbErrorPtr error(xcb_request_check(connection, xcb_test_fake_input_checked(connection, type, detail, XCB_CURRENT_TIME, eventRoot, x, y, 0)));
        if (error) {
            fail(xcbErrorString(operation, error.get()));
            return false;
        }
        xcb_flush(connection);
        return true;
    }

    bool sendButton(uint8_t detail, bool pressed, const char* operation)
    {
        return sendFakeInput(pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, detail, 0, 0, XCB_NONE, operation);
    }

    void fail(const QString& error)
    {
        lastError = error;
        qCWarning(XdgDesktopPortalKdeX11) << error;
        if (available) {
            available = false;
            Q_EMIT q->availabilityChanged(false);
        }
        Q_EMIT q->errorOccurred(error);
    }

    xkb_keycode_t keycodeForKeysym(xkb_keysym_t keysym) const
    {
        if (!keymap || keysym == XKB_KEY_NoSymbol || keysym == XKB_KEY_VoidSymbol) {
            return XKB_KEYCODE_INVALID;
        }

        const xkb_keycode_t first = xkb_keymap_min_keycode(keymap.get());
        const xkb_keycode_t last = xkb_keymap_max_keycode(keymap.get());
        for (xkb_keycode_t keycode = first; keycode <= last; ++keycode) {
            const xkb_layout_index_t layoutCount = xkb_keymap_num_layouts_for_key(keymap.get(), keycode);
            for (xkb_layout_index_t layout = 0; layout < layoutCount; ++layout) {
                const xkb_level_index_t levelCount = xkb_keymap_num_levels_for_key(keymap.get(), keycode, layout);
                for (xkb_level_index_t level = 0; level < levelCount; ++level) {
                    const xkb_keysym_t* syms = nullptr;
                    const int symCount = xkb_keymap_key_get_syms_by_level(keymap.get(), keycode, layout, level, &syms);
                    for (int i = 0; i < symCount; ++i) {
                        if (syms[i] == keysym) {
                            return keycode;
                        }
                    }
                }
            }
        }
        return XKB_KEYCODE_INVALID;
    }

    bool validateXKeycode(int xKeycode, const char* operation)
    {
        if (xKeycode < XKeycodeMin || xKeycode > XKeycodeMax || xKeycode < minKeycode || xKeycode > maxKeycode) {
            fail(QString::fromLatin1("%1 rejected invalid X keycode %2 (server range %3-%4)")
                    .arg(QString::fromLatin1(operation))
                    .arg(xKeycode)
                    .arg(minKeycode)
                    .arg(maxKeycode));
            return false;
        }
        if (keymap && (xKeycode < static_cast<int>(xkb_keymap_min_keycode(keymap.get())) || xKeycode > static_cast<int>(xkb_keymap_max_keycode(keymap.get())))) {
            fail(QString::fromLatin1("%1 rejected keycode %2 because it is outside the active XKB keymap").arg(QString::fromLatin1(operation)).arg(xKeycode));
            return false;
        }
        return true;
    }

    X11Input* q = nullptr;
    xcb_connection_t* connection = nullptr;
    ConnectionOwnership ownership = ConnectionOwnership::Borrowed;
    bool available = false;
    bool xtestAvailable = false;
    QString lastError;
    xcb_window_t root = XCB_NONE;
    QRect rootGeometry;
    int minKeycode = XKeycodeMin;
    int maxKeycode = XKeycodeMax;
    int32_t keyboardDevice = -1;
    XkbContextPtr context;
    XkbKeymapPtr keymap;
    XkbStatePtr state;
};

X11Input::X11Input(QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
    d->initialize(nullptr, ConnectionOwnership::Owned);
}

X11Input::X11Input(xcb_connection_t* connection, ConnectionOwnership ownership, QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
    d->initialize(connection, ownership);
}

X11Input::~X11Input()
{
    delete d;
}

bool X11Input::isAvailable() const
{
    return d->available;
}

QString X11Input::lastError() const
{
    return d->lastError;
}

int X11Input::linuxButtonToXButton(int linuxButton)
{
    switch (linuxButton) {
    case BTN_LEFT:
        return 1;
    case BTN_MIDDLE:
        return 2;
    case BTN_RIGHT:
        return 3;
    case BTN_SIDE:
        return 8;
    case BTN_EXTRA:
        return 9;
    case BTN_FORWARD:
        return 10;
    case BTN_BACK:
        return 11;
    case BTN_TASK:
        return 12;
    default:
        return 0;
    }
}

int X11Input::evdevKeycodeToXKeycode(int evdevKeycode)
{
    return isValidEvdevKeycode(evdevKeycode) ? evdevKeycode + XKeycodeOffset : -1;
}

bool X11Input::isValidEvdevKeycode(int evdevKeycode)
{
    return evdevKeycode >= 0 && evdevKeycode + XKeycodeOffset >= XKeycodeMin && evdevKeycode + XKeycodeOffset <= XKeycodeMax;
}

void X11Input::injectPointerMotion(const QSizeF& delta)
{
    if (delta.isNull()) {
        return;
    }
    d->sendFakeInput(XCB_MOTION_NOTIFY,
        0,
        static_cast<int16_t>(std::clamp(std::lround(delta.width()), static_cast<long>(std::numeric_limits<int16_t>::min()), static_cast<long>(std::numeric_limits<int16_t>::max()))),
        static_cast<int16_t>(std::clamp(std::lround(delta.height()), static_cast<long>(std::numeric_limits<int16_t>::min()), static_cast<long>(std::numeric_limits<int16_t>::max()))),
        XCB_NONE,
        "relative pointer motion");
}

void X11Input::injectPointerMotionAbsolute(const QRect& streamGeometry, const QPointF& pos)
{
    const QRect target = streamGeometry.isValid() ? streamGeometry : d->rootGeometry;
    if (!target.isValid()) {
        d->fail(QStringLiteral("absolute pointer motion rejected because no target geometry is available"));
        return;
    }
    const int x = std::clamp(static_cast<int>(std::lround(pos.x())), target.left(), target.right());
    const int y = std::clamp(static_cast<int>(std::lround(pos.y())), target.top(), target.bottom());
    d->sendFakeInput(XCB_MOTION_NOTIFY,
        0,
        static_cast<int16_t>(std::clamp(x, static_cast<int>(std::numeric_limits<int16_t>::min()), static_cast<int>(std::numeric_limits<int16_t>::max()))),
        static_cast<int16_t>(std::clamp(y, static_cast<int>(std::numeric_limits<int16_t>::min()), static_cast<int>(std::numeric_limits<int16_t>::max()))),
        d->root,
        "absolute pointer motion");
}

void X11Input::injectPointerButton(int linuxButton, bool pressed)
{
    const int xButton = linuxButtonToXButton(linuxButton);
    if (xButton <= 0 || xButton > std::numeric_limits<uint8_t>::max()) {
        d->fail(QString::fromLatin1("pointer button rejected unsupported Linux button code %1").arg(linuxButton));
        return;
    }
    d->sendButton(static_cast<uint8_t>(xButton), pressed, "pointer button");
}

void X11Input::injectPointerAxis(qreal dx, qreal dy)
{
    const int horizontalSteps = static_cast<int>(std::lround(dx / WheelStep));
    const int verticalSteps = static_cast<int>(std::lround(dy / WheelStep));
    if (horizontalSteps != 0) {
        injectPointerAxisDiscrete(Qt::Horizontal, horizontalSteps);
    }
    if (verticalSteps != 0) {
        injectPointerAxisDiscrete(Qt::Vertical, verticalSteps);
    }
}

void X11Input::injectPointerAxisDiscrete(Qt::Orientation axis, int steps)
{
    if (steps == 0) {
        return;
    }
    const uint8_t button = static_cast<uint8_t>(buttonForAxis(axis, steps));
    for (int i = 0; i < std::abs(steps); ++i) {
        if (!d->sendButton(button, true, "pointer axis discrete press")) {
            return;
        }
        if (!d->sendButton(button, false, "pointer axis discrete release")) {
            return;
        }
    }
}

void X11Input::injectKeySym(int keysym, bool pressed)
{
    if (!d->checkAvailable("keysym")) {
        return;
    }
    const xkb_keycode_t keycode = d->keycodeForKeysym(static_cast<xkb_keysym_t>(keysym));
    if (keycode == XKB_KEYCODE_INVALID) {
        d->fail(QString::fromLatin1("keysym injection rejected keysym 0x%1 because it is absent from the active XKB keymap").arg(static_cast<uint>(keysym), 0, 16));
        return;
    }
    if (!d->validateXKeycode(static_cast<int>(keycode), "keysym")) {
        return;
    }
    d->sendFakeInput(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, static_cast<uint8_t>(keycode), 0, 0, XCB_NONE, "keysym");
}

void X11Input::injectKeyCode(int keycode, bool pressed)
{
    const int xKeycode = evdevKeycodeToXKeycode(keycode);
    if (xKeycode < 0) {
        d->fail(QString::fromLatin1("keycode injection rejected invalid evdev keycode %1").arg(keycode));
        return;
    }
    if (!d->validateXKeycode(xKeycode, "keycode")) {
        return;
    }
    d->sendFakeInput(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, static_cast<uint8_t>(xKeycode), 0, 0, XCB_NONE, "keycode");
}
