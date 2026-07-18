#include "x11clipboard.h"

#include <QElapsedTimer>
#include <QHash>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <xcb/xcb_event.h>
#include <xcb/xfixes.h>

using namespace Qt::StringLiterals;

namespace {
constexpr qsizetype s_incrThreshold = 256 * 1024;
constexpr int s_transferTimeoutMs = 15000;

struct UniqueFd {
    int fd = -1;

    UniqueFd() = default;
    explicit UniqueFd(int descriptor)
        : fd(descriptor)
    {
    }
    Q_DISABLE_COPY_MOVE(UniqueFd)
    ~UniqueFd()
    {
        reset();
    }

    int take()
    {
        const int old = fd;
        fd = -1;
        return old;
    }

    void reset(int descriptor = -1)
    {
        if (fd >= 0) {
            close(fd);
        }
        fd = descriptor;
    }
};

bool makePipe(UniqueFd& readEnd, UniqueFd& writeEnd)
{
    int fds[2] = {-1, -1};
#ifdef O_CLOEXEC
#ifdef O_NONBLOCK
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        readEnd.reset(fds[0]);
        writeEnd.reset(fds[1]);
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return false;
    }
#endif
#endif
    if (pipe(fds) != 0) {
        return false;
    }
    for (int fd : fds) {
        int flags = fcntl(fd, F_GETFD, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    readEnd.reset(fds[0]);
    writeEnd.reset(fds[1]);
    return true;
}

QByteArray atomNameForMime(const QString& mimeType)
{
    return X11ClipboardProtocol::canonicalMimeType(mimeType).toUtf8();
}

void removeDuplicateAtoms(QVector<xcb_atom_t>& atoms)
{
    QSet<xcb_atom_t> seen;
    for (auto it = atoms.begin(); it != atoms.end();) {
        if (seen.contains(*it)) {
            it = atoms.erase(it);
        } else {
            seen.insert(*it);
            ++it;
        }
    }
}

uint16_t eventType(const xcb_generic_event_t* event)
{
    return event->response_type & ~0x80;
}

bool writeAllNonBlocking(int fd, const QByteArray& bytes)
{
    const char* data = bytes.constData();
    qsizetype remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t written = write(fd, data, static_cast<size_t>(remaining));
        if (written > 0) {
            data += written;
            remaining -= written;
            continue;
        }
        if (written < 0 && (errno == EINTR)) {
            continue;
        }
        if (written < 0 && errno == EAGAIN) {
#if EWOULDBLOCK != EAGAIN
            return true;
        }
        if (written < 0 && errno == EWOULDBLOCK) {
#endif
            return true;
        }
        return false;
    }
    return true;
}

QByteArray readAvailable(int fd)
{
    QByteArray out;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<qsizetype>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
#if EWOULDBLOCK != EAGAIN
            break;
        }
        if (errno == EWOULDBLOCK) {
#endif
            break;
        }
        break;
    }
    return out;
}
} // namespace

namespace X11ClipboardProtocol {
QString canonicalMimeType(QString mimeType)
{
    mimeType = mimeType.trimmed().toLower();
    if (mimeType == u"utf8_string"_s || mimeType == u"text"_s || mimeType == u"string"_s) {
        return u"text/plain;charset=utf-8"_s;
    }
    if (mimeType == u"text/plain; charset=utf-8"_s || mimeType == u"text/plain;charset=utf8"_s || mimeType == u"text/plain"_s) {
        return u"text/plain;charset=utf-8"_s;
    }
    if (mimeType.isEmpty()) {
        return u"application/octet-stream"_s;
    }
    return mimeType;
}

QStringList mimeAliases(const QString& mimeType)
{
    const QString canonical = canonicalMimeType(mimeType);
    QStringList aliases{canonical};
    if (canonical == u"text/plain;charset=utf-8"_s) {
        aliases << u"text/plain"_s << u"UTF8_STRING"_s << u"TEXT"_s << u"STRING"_s;
    }
    aliases.removeDuplicates();
    return aliases;
}

QVector<xcb_atom_t> targetsForMime(const Atoms& atoms, const QString& mimeType)
{
    const QString canonical = canonicalMimeType(mimeType);
    QVector<xcb_atom_t> targets;
    if (canonical == u"text/plain;charset=utf-8"_s) {
        targets << atoms.textPlainUtf8 << atoms.utf8String << atoms.textPlain << atoms.text << atoms.string;
    } else if (canonical == u"application/octet-stream"_s) {
        targets << atoms.applicationOctetStream;
    }
    targets.erase(std::remove(targets.begin(), targets.end(), XCB_ATOM_NONE), targets.end());
    removeDuplicateAtoms(targets);
    return targets;
}

QString mimeForTarget(const Atoms& atoms, xcb_atom_t target)
{
    if (target == atoms.textPlainUtf8 || target == atoms.utf8String || target == atoms.textPlain || target == atoms.text || target == atoms.string) {
        return u"text/plain;charset=utf-8"_s;
    }
    if (target == atoms.applicationOctetStream) {
        return u"application/octet-stream"_s;
    }
    return {};
}

QVector<xcb_atom_t> advertisedTargets(const Atoms& atoms, const QString& mimeType)
{
    QVector<xcb_atom_t> targets{atoms.targets, atoms.timestamp, atoms.multiple};
    targets += targetsForMime(atoms, mimeType);
    targets.erase(std::remove(targets.begin(), targets.end(), XCB_ATOM_NONE), targets.end());
    removeDuplicateAtoms(targets);
    return targets;
}

std::optional<QVector<xcb_atom_t>> rewriteMultiplePairs(const QVector<xcb_atom_t>& pairs, const QSet<xcb_atom_t>& supportedTargets)
{
    if (pairs.size() % 2 != 0) {
        return std::nullopt;
    }
    QVector<xcb_atom_t> rewritten = pairs;
    for (qsizetype i = 0; i < rewritten.size(); i += 2) {
        if (!supportedTargets.contains(rewritten[i])) {
            rewritten[i + 1] = XCB_ATOM_NONE;
        }
    }
    return rewritten;
}

TransferMode chooseTransferMode(qsizetype byteCount, qsizetype maxRequestBytes)
{
    const qsizetype limit = std::max<qsizetype>(1, std::min(maxRequestBytes, s_incrThreshold));
    return byteCount > limit ? TransferMode::Incr : TransferMode::Normal;
}

QByteArray nextIncrChunk(SendTransfer& transfer)
{
    if (transfer.offset >= transfer.data.size()) {
        return {};
    }
    const qsizetype count = std::min<qsizetype>(transfer.chunkSize, transfer.data.size() - transfer.offset);
    const QByteArray chunk = transfer.data.mid(transfer.offset, count);
    transfer.offset += count;
    return chunk;
}

bool appendReceivedChunk(ReceiveTransfer& transfer, const QByteArray& chunk)
{
    if (chunk.isEmpty()) {
        transfer.done = true;
        if (transfer.writeFd >= 0) {
            close(transfer.writeFd);
            transfer.writeFd = -1;
        }
        return true;
    }
    transfer.bytesReceived += chunk.size();
    return transfer.writeFd < 0 || writeAllNonBlocking(transfer.writeFd, chunk);
}
} // namespace X11ClipboardProtocol

struct X11Clipboard::Private {
    explicit Private(X11Clipboard* qq)
        : q(qq)
    {
    }

    struct PendingWrite {
        QString sessionPath;
        QString mimeType;
        uint serial = 0;
        UniqueFd readFd;
        QSocketNotifier* notifier = nullptr;
        QByteArray data;
        qint64 lastActivityMs = 0;
    };

    X11Clipboard* q = nullptr;
    xcb_connection_t* connection = nullptr;
    bool ownsConnection = false;
    int defaultScreen = 0;
    xcb_screen_t* screen = nullptr;
    xcb_window_t window = XCB_WINDOW_NONE;
    X11ClipboardProtocol::Atoms atoms;
    QHash<QByteArray, xcb_atom_t> atomCache;
    QHash<uint, PendingWrite*> pendingWrites;
    QHash<xcb_atom_t, X11ClipboardProtocol::ReceiveTransfer> receives;
    QHash<xcb_atom_t, X11ClipboardProtocol::SendTransfer> sends;
    QSocketNotifier* xcbNotifier = nullptr;
    QTimer* timeoutTimer = nullptr;
    QElapsedTimer monotonic;
    xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
    QString ownerSession;
    QString ownerMimeType;
    QByteArray ownerData;
    int xfixesEventBase = -1;
    bool initialized = false;
    uint nextSerial = 1;

    xcb_atom_t intern(const QByteArray& name)
    {
        if (name.isEmpty()) {
            return XCB_ATOM_NONE;
        }
        if (const auto it = atomCache.constFind(name); it != atomCache.constEnd()) {
            return it.value();
        }
        xcb_intern_atom_cookie_t cookie = xcb_intern_atom(connection, 0, static_cast<uint16_t>(name.size()), name.constData());
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
        const auto cleanup = qScopeGuard([reply] {
            free(reply);
        });
        if (!reply) {
            return XCB_ATOM_NONE;
        }
        atomCache.insert(name, reply->atom);
        return reply->atom;
    }

    bool setupConnection(QString* errorOut)
    {
        if (!connection) {
            connection = xcb_connect(nullptr, &defaultScreen);
            ownsConnection = true;
        }
        if (!connection || xcb_connection_has_error(connection)) {
            if (errorOut) {
                *errorOut = u"Failed to open XCB connection"_s;
            }
            return false;
        }

        const xcb_setup_t* setup = xcb_get_setup(connection);
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < defaultScreen && it.rem; ++i) {
            xcb_screen_next(&it);
        }
        screen = it.data;
        if (!screen) {
            if (errorOut) {
                *errorOut = u"No X11 screen available"_s;
            }
            return false;
        }
        return true;
    }

    void internAtoms()
    {
        atoms.clipboard = intern("CLIPBOARD");
        atoms.targets = intern("TARGETS");
        atoms.timestamp = intern("TIMESTAMP");
        atoms.multiple = intern("MULTIPLE");
        atoms.incr = intern("INCR");
        atoms.utf8String = intern("UTF8_STRING");
        atoms.text = intern("TEXT");
        atoms.textPlain = intern("text/plain");
        atoms.textPlainUtf8 = intern("text/plain;charset=utf-8");
        atoms.applicationOctetStream = intern("application/octet-stream");
        atoms.xdpClipboardData = intern("_XDP_SONICDE_CLIPBOARD_DATA");
        atoms.xdpClipboardTargets = intern("_XDP_SONICDE_CLIPBOARD_TARGETS");
        atoms.xdpClipboardTransfer = intern("_XDP_SONICDE_CLIPBOARD_TRANSFER");
        atoms.property = intern("PROPERTY");
    }

    bool createWindow(QString* errorOut)
    {
        window = xcb_generate_id(connection);
        const uint32_t mask = XCB_CW_EVENT_MASK;
        const uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
        xcb_void_cookie_t cookie = xcb_create_window_checked(connection,
            XCB_COPY_FROM_PARENT,
            window,
            screen->root,
            0,
            0,
            1,
            1,
            0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            screen->root_visual,
            mask,
            values);
        xcb_generic_error_t* error = xcb_request_check(connection, cookie);
        const auto cleanup = qScopeGuard([error] {
            free(error);
        });
        if (error) {
            if (errorOut) {
                *errorOut = u"Failed to create hidden X11 clipboard window"_s;
            }
            window = XCB_WINDOW_NONE;
            return false;
        }
        return true;
    }

    void setupXFixes()
    {
        const xcb_query_extension_reply_t* ext = xcb_get_extension_data(connection, &xcb_xfixes_id);
        if (!ext || !ext->present) {
            return;
        }
        xfixesEventBase = ext->first_event;
        xcb_xfixes_query_version_cookie_t cookie = xcb_xfixes_query_version(connection, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
        free(xcb_xfixes_query_version_reply(connection, cookie, nullptr));
        xcb_xfixes_select_selection_input(connection,
            window,
            atoms.clipboard,
            XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
    }

    void startNotifiers()
    {
        if (!monotonic.isValid()) {
            monotonic.start();
        }
        const int fd = xcb_get_file_descriptor(connection);
        if (fd >= 0) {
            xcbNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, q);
            QObject::connect(xcbNotifier, &QSocketNotifier::activated, q, [this](QSocketDescriptor) {
                drainEvents();
            });
        }
        timeoutTimer = new QTimer(q);
        timeoutTimer->setInterval(1000);
        QObject::connect(timeoutTimer, &QTimer::timeout, q, [this] {
            reapTimeouts();
        });
        timeoutTimer->start();
    }

    void drainEvents()
    {
        while (connection) {
            xcb_generic_event_t* event = xcb_poll_for_event(connection);
            if (!event) {
                break;
            }
            q->handleXcbEvent(event);
            free(event);
        }
    }

    void reapTimeouts()
    {
        if (xcb_connection_has_error(connection)) {
            q->shutdown();
            return;
        }

        const qint64 now = monotonic.elapsed();
        for (uint serial : pendingWrites.keys()) {
            const PendingWrite* pending = pendingWrites.value(serial, nullptr);
            if (pending && now - pending->lastActivityMs > s_transferTimeoutMs) {
                removePending(serial);
            }
        }
        for (auto it = receives.begin(); it != receives.end();) {
            if (now - it->lastActivityMs > s_transferTimeoutMs) {
                if (it->writeFd >= 0) {
                    close(it->writeFd);
                }
                it = receives.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = sends.begin(); it != sends.end();) {
            if (now - it->lastActivityMs > s_transferTimeoutMs) {
                it = sends.erase(it);
            } else {
                ++it;
            }
        }
    }

    xcb_atom_t nextTransferProperty()
    {
        const QByteArray name = QByteArrayLiteral("_XDP_SONICDE_CLIPBOARD_TRANSFER_") + QByteArray::number(nextSerial++);
        return intern(name);
    }

    void becomeOwner()
    {
        timestamp = XCB_CURRENT_TIME;
        xcb_set_selection_owner(connection, window, atoms.clipboard, timestamp);
        xcb_flush(connection);
        Q_EMIT q->selectionOwnerChanged(QDBusObjectPath(ownerSession), QStringList{X11ClipboardProtocol::canonicalMimeType(ownerMimeType)}, true);
    }

    void loseOwner()
    {
        const QString oldSession = std::exchange(ownerSession, {});
        ownerMimeType.clear();
        ownerData.clear();
        sends.clear();
        if (!oldSession.isEmpty()) {
            Q_EMIT q->selectionOwnerChanged(QDBusObjectPath(oldSession), {}, false);
        }
    }

    PendingWrite* pendingFor(uint serial, const QString& sessionPath)
    {
        PendingWrite* pending = pendingWrites.value(serial, nullptr);
        if (!pending || pending->sessionPath != sessionPath) {
            return nullptr;
        }
        return pending;
    }

    void removePending(uint serial)
    {
        PendingWrite* pending = pendingWrites.take(serial);
        if (!pending) {
            return;
        }
        if (pending->notifier) {
            pending->notifier->setEnabled(false);
            pending->notifier->deleteLater();
        }
        delete pending;
    }

    xcb_atom_t preferredTarget(const QString& mimeType)
    {
        const QVector<xcb_atom_t> targets = X11ClipboardProtocol::targetsForMime(atoms, mimeType);
        return targets.isEmpty() ? intern(atomNameForMime(mimeType)) : targets.first();
    }

    void requestTargets()
    {
        xcb_convert_selection(connection, window, atoms.clipboard, atoms.targets, atoms.xdpClipboardTargets, XCB_CURRENT_TIME);
        xcb_flush(connection);
    }

    void requestData(X11ClipboardProtocol::ReceiveTransfer transfer)
    {
        transfer.lastActivityMs = monotonic.elapsed();
        receives.insert(transfer.property, transfer);
        xcb_convert_selection(connection, window, atoms.clipboard, transfer.target, transfer.property, XCB_CURRENT_TIME);
        xcb_flush(connection);
    }

    QByteArray dataForTarget(xcb_atom_t target) const
    {
        Q_UNUSED(target)
        return ownerData;
    }

    void notifySelection(xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target, xcb_atom_t property, xcb_timestamp_t time)
    {
        xcb_selection_notify_event_t notify = {};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = time;
        notify.requestor = requestor;
        notify.selection = selection;
        notify.target = target;
        notify.property = property;
        xcb_send_event(connection, false, requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify));
        xcb_flush(connection);
    }

    void changeProperty(xcb_window_t requestor, xcb_atom_t property, xcb_atom_t type, uint8_t format, const void* data, qsizetype bytes)
    {
        const qsizetype units = format == 32 ? bytes / 4 : bytes;
        xcb_change_property(connection,
            XCB_PROP_MODE_REPLACE,
            requestor,
            property,
            type,
            format,
            static_cast<uint32_t>(units),
            data);
    }

    bool serveSpecialTarget(const xcb_selection_request_event_t* request, bool notify = true)
    {
        const xcb_atom_t property = request->property == XCB_ATOM_NONE ? request->target : request->property;
        if (request->target == atoms.targets) {
            const QVector<xcb_atom_t> targets = X11ClipboardProtocol::advertisedTargets(atoms, ownerMimeType);
            changeProperty(request->requestor, property, atoms.atom, 32, targets.constData(), targets.size() * qsizetype(sizeof(xcb_atom_t)));
            if (notify) {
                notifySelection(request->requestor, request->selection, request->target, property, request->time);
            }
            return true;
        }
        if (request->target == atoms.timestamp) {
            const xcb_timestamp_t ts = timestamp;
            changeProperty(request->requestor, property, XCB_ATOM_INTEGER, 32, &ts, sizeof(ts));
            if (notify) {
                notifySelection(request->requestor, request->selection, request->target, property, request->time);
            }
            return true;
        }
        return false;
    }

    void serveDataTarget(const xcb_selection_request_event_t* request, bool notify = true)
    {
        const xcb_atom_t property = request->property == XCB_ATOM_NONE ? request->target : request->property;
        const QSet<xcb_atom_t> supported(X11ClipboardProtocol::advertisedTargets(atoms, ownerMimeType).cbegin(),
            X11ClipboardProtocol::advertisedTargets(atoms, ownerMimeType).cend());
        if (!supported.contains(request->target)) {
            if (notify) {
                notifySelection(request->requestor, request->selection, request->target, XCB_ATOM_NONE, request->time);
            }
            return;
        }

        const QByteArray data = dataForTarget(request->target);
        const auto mode = X11ClipboardProtocol::chooseTransferMode(data.size(), xcb_get_maximum_request_length(connection) * 4 - 1024);
        if (mode == X11ClipboardProtocol::TransferMode::Normal) {
            changeProperty(request->requestor, property, request->target, 8, data.constData(), data.size());
            if (notify) {
                notifySelection(request->requestor, request->selection, request->target, property, request->time);
            }
            return;
        }

        const uint32_t size = static_cast<uint32_t>(std::min<qsizetype>(data.size(), std::numeric_limits<uint32_t>::max()));
        changeProperty(request->requestor, property, atoms.incr, 32, &size, sizeof(size));
        X11ClipboardProtocol::SendTransfer transfer;
        transfer.requestor = request->requestor;
        transfer.target = request->target;
        transfer.property = property;
        transfer.data = data;
        transfer.lastActivityMs = monotonic.elapsed();
        transfer.incr = true;
        transfer.chunkSize = static_cast<int>(std::min<qsizetype>(64 * 1024, xcb_get_maximum_request_length(connection) * 4 - 1024));
        sends.insert(property, transfer);
        if (notify) {
            notifySelection(request->requestor, request->selection, request->target, property, request->time);
        }
    }

    void handleMultiple(const xcb_selection_request_event_t* request)
    {
        const xcb_atom_t property = request->property == XCB_ATOM_NONE ? request->target : request->property;
        xcb_get_property_cookie_t cookie = xcb_get_property(connection, false, request->requestor, property, atoms.atom, 0, std::numeric_limits<uint32_t>::max());
        xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
        const auto cleanup = qScopeGuard([reply] {
            free(reply);
        });
        if (!reply || reply->format != 32 || reply->type != atoms.atom) {
            notifySelection(request->requestor, request->selection, request->target, XCB_ATOM_NONE, request->time);
            return;
        }
        const auto* values = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
        QVector<xcb_atom_t> pairs(values, values + xcb_get_property_value_length(reply) / int(sizeof(xcb_atom_t)));
        const QVector<xcb_atom_t> advertised = X11ClipboardProtocol::advertisedTargets(atoms, ownerMimeType);
        const QSet<xcb_atom_t> supported(advertised.cbegin(), advertised.cend());
        const std::optional<QVector<xcb_atom_t>> rewritten = X11ClipboardProtocol::rewriteMultiplePairs(pairs, supported);
        if (!rewritten) {
            notifySelection(request->requestor, request->selection, request->target, XCB_ATOM_NONE, request->time);
            return;
        }
        changeProperty(request->requestor, property, atoms.atom, 32, rewritten->constData(), rewritten->size() * qsizetype(sizeof(xcb_atom_t)));
        for (qsizetype i = 0; i < rewritten->size(); i += 2) {
            if ((*rewritten)[i + 1] == XCB_ATOM_NONE || (*rewritten)[i] == atoms.multiple) {
                continue;
            }
            xcb_selection_request_event_t sub = *request;
            sub.target = (*rewritten)[i];
            sub.property = (*rewritten)[i + 1];
            if (!serveSpecialTarget(&sub, false)) {
                serveDataTarget(&sub, false);
            }
        }
        notifySelection(request->requestor, request->selection, request->target, property, request->time);
    }

    void handleSelectionRequest(const xcb_selection_request_event_t* request)
    {
        if (request->selection != atoms.clipboard || ownerSession.isEmpty()) {
            notifySelection(request->requestor, request->selection, request->target, XCB_ATOM_NONE, request->time);
            return;
        }
        if (request->target == atoms.multiple) {
            handleMultiple(request);
            return;
        }
        if (!serveSpecialTarget(request)) {
            serveDataTarget(request);
        }
    }

    void handleSelectionNotify(const xcb_selection_notify_event_t* notify)
    {
        if (notify->selection != atoms.clipboard) {
            return;
        }
        if (notify->property == XCB_ATOM_NONE) {
            for (auto it = receives.begin(); it != receives.end(); ++it) {
                if (it->target == notify->target) {
                    if (it->writeFd >= 0)
                        close(it->writeFd);
                    receives.erase(it);
                    break;
                }
            }
            return;
        }
        if (notify->target == atoms.targets && notify->property == atoms.xdpClipboardTargets) {
            xcb_get_property_cookie_t cookie = xcb_get_property(connection, true, window, notify->property, atoms.atom, 0, std::numeric_limits<uint32_t>::max());
            xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
            const auto cleanup = qScopeGuard([reply] {
                free(reply);
            });
            if (!reply || reply->format != 32) {
                return;
            }
            const auto* values = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
            QStringList mimes;
            for (int i = 0; i < xcb_get_property_value_length(reply) / int(sizeof(xcb_atom_t)); ++i) {
                const QString mime = X11ClipboardProtocol::mimeForTarget(atoms, values[i]);
                if (!mime.isEmpty()) {
                    mimes << mime;
                }
            }
            mimes.removeDuplicates();
            Q_EMIT q->selectionOwnerChanged(QDBusObjectPath(), mimes, false);
            return;
        }

        X11ClipboardProtocol::ReceiveTransfer transfer = receives.value(notify->property);
        if (transfer.property == XCB_ATOM_NONE) {
            return;
        }
        xcb_get_property_cookie_t cookie = xcb_get_property(connection, false, window, notify->property, XCB_GET_PROPERTY_TYPE_ANY, 0, std::numeric_limits<uint32_t>::max());
        xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
        const auto cleanup = qScopeGuard([reply] {
            free(reply);
        });
        if (!reply) {
            if (transfer.writeFd >= 0)
                close(transfer.writeFd);
            receives.remove(notify->property);
            return;
        }
        if (reply->type == atoms.incr) {
            transfer.incr = true;
            transfer.lastActivityMs = monotonic.elapsed();
            receives.insert(notify->property, transfer);
            xcb_delete_property(connection, window, notify->property);
            xcb_flush(connection);
            return;
        }
        const QByteArray chunk(static_cast<const char*>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
        X11ClipboardProtocol::appendReceivedChunk(transfer, chunk);
        transfer.lastActivityMs = monotonic.elapsed();
        receives.insert(notify->property, transfer);
        if (!transfer.incr) {
            X11ClipboardProtocol::appendReceivedChunk(transfer, {});
            receives.remove(notify->property);
        } else if (chunk.isEmpty()) {
            receives.remove(notify->property);
        }
        xcb_delete_property(connection, window, notify->property);
        xcb_flush(connection);
    }

    void handlePropertyNotify(const xcb_property_notify_event_t* property)
    {
        if (property->window == window && receives.contains(property->atom) && property->state == XCB_PROPERTY_NEW_VALUE) {
            X11ClipboardProtocol::ReceiveTransfer transfer = receives.value(property->atom);
            xcb_get_property_cookie_t cookie = xcb_get_property(connection, false, window, property->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, std::numeric_limits<uint32_t>::max());
            xcb_get_property_reply_t* reply = xcb_get_property_reply(connection, cookie, nullptr);
            const auto cleanup = qScopeGuard([reply] {
                free(reply);
            });
            if (!reply) {
                if (transfer.writeFd >= 0)
                    close(transfer.writeFd);
                receives.remove(property->atom);
                return;
            }
            const QByteArray chunk(static_cast<const char*>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
            X11ClipboardProtocol::appendReceivedChunk(transfer, chunk);
            transfer.lastActivityMs = monotonic.elapsed();
            if (chunk.isEmpty()) {
                receives.remove(property->atom);
            } else {
                receives.insert(property->atom, transfer);
            }
            xcb_delete_property(connection, window, property->atom);
            xcb_flush(connection);
            return;
        }

        if (sends.contains(property->atom) && property->state == XCB_PROPERTY_DELETE) {
            X11ClipboardProtocol::SendTransfer transfer = sends.value(property->atom);
            const QByteArray chunk = X11ClipboardProtocol::nextIncrChunk(transfer);
            transfer.lastActivityMs = monotonic.elapsed();
            xcb_change_property(connection,
                XCB_PROP_MODE_REPLACE,
                transfer.requestor,
                transfer.property,
                transfer.target,
                8,
                static_cast<uint32_t>(chunk.size()),
                chunk.constData());
            if (chunk.isEmpty()) {
                sends.remove(property->atom);
            } else {
                sends.insert(property->atom, transfer);
            }
            xcb_flush(connection);
        }
    }

    void handleXFixesSelection(const xcb_xfixes_selection_notify_event_t* event)
    {
        if (event->selection != atoms.clipboard) {
            return;
        }
        if (event->owner == window) {
            return;
        }
        loseOwner();
        requestTargets();
    }

    void cleanup()
    {
        for (uint serial : pendingWrites.keys()) {
            removePending(serial);
        }
        for (auto it = receives.begin(); it != receives.end(); ++it) {
            if (it->writeFd >= 0) {
                close(it->writeFd);
                it->writeFd = -1;
            }
        }
        receives.clear();
        sends.clear();
        if (timeoutTimer) {
            timeoutTimer->stop();
            delete timeoutTimer;
            timeoutTimer = nullptr;
        }
        if (xcbNotifier) {
            xcbNotifier->setEnabled(false);
            delete xcbNotifier;
            xcbNotifier = nullptr;
        }
        if (connection && window != XCB_WINDOW_NONE) {
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            window = XCB_WINDOW_NONE;
        }
        if (connection && ownsConnection) {
            xcb_disconnect(connection);
        }
        connection = nullptr;
        initialized = false;
    }
};

X11Clipboard::X11Clipboard(QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
}

X11Clipboard::X11Clipboard(xcb_connection_t* connection, int defaultScreen, bool ownsConnection, QObject* parent)
    : QObject(parent)
    , d(new Private(this))
{
    d->connection = connection;
    d->defaultScreen = defaultScreen;
    d->ownsConnection = ownsConnection;
}

X11Clipboard::~X11Clipboard()
{
    shutdown();
    delete d;
}

bool X11Clipboard::isSessionAuthorized(const QDBusObjectPath& session)
{
    return m_sessionState.value(session.path()).authorized;
}

void X11Clipboard::authorizeSession(const QDBusObjectPath& session, bool authorized)
{
    m_sessionState[session.path()].authorized = authorized;
    if (!authorized) {
        closeSessionTransfers(session.path());
    }
}

bool X11Clipboard::isSessionActive(const QDBusObjectPath& session)
{
    return m_sessionState.value(session.path()).active;
}

void X11Clipboard::setSessionActive(const QDBusObjectPath& session, bool active)
{
    m_sessionState[session.path()].active = active;
    if (!active) {
        closeSessionTransfers(session.path());
    }
}

bool X11Clipboard::initialize(QString* errorOut)
{
    if (d->initialized) {
        return true;
    }
    if (!d->setupConnection(errorOut)) {
        return false;
    }
    d->internAtoms();
    if (!d->createWindow(errorOut)) {
        return false;
    }
    d->setupXFixes();
    d->startNotifiers();
    d->initialized = true;
    xcb_flush(d->connection);
    return true;
}

void X11Clipboard::shutdown()
{
    if (d) {
        d->cleanup();
    }
}

bool X11Clipboard::sessionAllowed(const QDBusObjectPath& session, QString* errorOut) const
{
    const SessionState state = m_sessionState.value(session.path());
    if (!state.authorized) {
        if (errorOut) {
            *errorOut = u"Clipboard session is not authorized"_s;
        }
        return false;
    }
    if (!state.active) {
        if (errorOut) {
            *errorOut = u"Clipboard session is not active"_s;
        }
        return false;
    }
    return true;
}

void X11Clipboard::closeSessionTransfers(const QString& sessionPath)
{
    for (uint serial : d->pendingWrites.keys()) {
        if (d->pendingWrites.value(serial)->sessionPath == sessionPath) {
            d->removePending(serial);
        }
    }
    for (auto it = d->receives.begin(); it != d->receives.end();) {
        if (it->sessionPath == sessionPath) {
            if (it->writeFd >= 0) {
                close(it->writeFd);
            }
            it = d->receives.erase(it);
        } else {
            ++it;
        }
    }
    if (d->ownerSession == sessionPath) {
        d->loseOwner();
    }
}

QDBusUnixFileDescriptor X11Clipboard::requestRead(const QDBusObjectPath& session, const QString& mimeType, QString* errorOut)
{
    if (!sessionAllowed(session, errorOut)) {
        return {};
    }
    if (!initialize(errorOut)) {
        return {};
    }
    UniqueFd readEnd;
    UniqueFd writeEnd;
    if (!makePipe(readEnd, writeEnd)) {
        if (errorOut) {
            *errorOut = u"Failed to create clipboard read pipe"_s;
        }
        return {};
    }

    X11ClipboardProtocol::ReceiveTransfer transfer;
    transfer.sessionPath = session.path();
    transfer.mimeType = X11ClipboardProtocol::canonicalMimeType(mimeType);
    transfer.target = d->preferredTarget(transfer.mimeType);
    transfer.property = d->nextTransferProperty();
    transfer.writeFd = writeEnd.take();
    d->requestData(transfer);
    return QDBusUnixFileDescriptor(readEnd.take());
}

bool X11Clipboard::requestWrite(const QDBusObjectPath& session, const QString& mimeType, QString* errorOut)
{
    if (!sessionAllowed(session, errorOut)) {
        return false;
    }
    if (!initialize(errorOut)) {
        return false;
    }
    Q_UNUSED(mimeType)
    return true;
}

QDBusUnixFileDescriptor X11Clipboard::beginWrite(const QDBusObjectPath& session, const QString& mimeType, uint serial, QString* errorOut)
{
    if (!requestWrite(session, mimeType, errorOut)) {
        return {};
    }
    if (serial == 0) {
        serial = d->nextSerial++;
    }
    if (d->pendingWrites.contains(serial)) {
        if (errorOut) {
            *errorOut = u"Clipboard write serial is already in use"_s;
        }
        return {};
    }

    UniqueFd readEnd;
    UniqueFd writeEnd;
    if (!makePipe(readEnd, writeEnd)) {
        if (errorOut) {
            *errorOut = u"Failed to create clipboard write pipe"_s;
        }
        return {};
    }
    auto* pending = new Private::PendingWrite;
    pending->sessionPath = session.path();
    pending->mimeType = X11ClipboardProtocol::canonicalMimeType(mimeType);
    pending->serial = serial;
    pending->readFd.reset(readEnd.take());
    pending->lastActivityMs = d->monotonic.elapsed();
    pending->notifier = new QSocketNotifier(pending->readFd.fd, QSocketNotifier::Read, this);
    connect(pending->notifier, &QSocketNotifier::activated, this, [this, serial](QSocketDescriptor) {
        Private::PendingWrite* pending = d->pendingWrites.value(serial, nullptr);
        if (!pending) {
            return;
        }
        const QByteArray bytes = readAvailable(pending->readFd.fd);
        if (!bytes.isEmpty()) {
            pending->data.append(bytes);
            pending->lastActivityMs = d->monotonic.elapsed();
        }
    });
    d->pendingWrites.insert(serial, pending);
    return QDBusUnixFileDescriptor(writeEnd.take());
}

bool X11Clipboard::completeWrite(const QDBusObjectPath& session, uint serial, bool success, QString* errorOut)
{
    if (!sessionAllowed(session, errorOut)) {
        return false;
    }
    Private::PendingWrite* pending = d->pendingFor(serial, session.path());
    if (!pending) {
        if (errorOut) {
            *errorOut = u"Unknown clipboard write serial"_s;
        }
        return false;
    }
    pending->data.append(readAvailable(pending->readFd.fd));
    if (!success) {
        d->removePending(serial);
        return true;
    }
    d->ownerSession = pending->sessionPath;
    d->ownerMimeType = pending->mimeType;
    d->ownerData = pending->data;
    d->removePending(serial);
    d->becomeOwner();
    return true;
}

void X11Clipboard::handleXcbEvent(const xcb_generic_event_t* event)
{
    if (!d || !event) {
        return;
    }
    const uint16_t type = eventType(event);
    if (type == XCB_SELECTION_REQUEST) {
        d->handleSelectionRequest(reinterpret_cast<const xcb_selection_request_event_t*>(event));
        return;
    }
    if (type == XCB_SELECTION_NOTIFY) {
        d->handleSelectionNotify(reinterpret_cast<const xcb_selection_notify_event_t*>(event));
        return;
    }
    if (type == XCB_SELECTION_CLEAR) {
        d->loseOwner();
        return;
    }
    if (type == XCB_PROPERTY_NOTIFY) {
        d->handlePropertyNotify(reinterpret_cast<const xcb_property_notify_event_t*>(event));
        return;
    }
    if (d->xfixesEventBase >= 0 && type == d->xfixesEventBase + XCB_XFIXES_SELECTION_NOTIFY) {
        d->handleXFixesSelection(reinterpret_cast<const xcb_xfixes_selection_notify_event_t*>(event));
    }
}

#include "moc_x11clipboard.cpp"
