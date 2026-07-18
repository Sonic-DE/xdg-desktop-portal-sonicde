#pragma once

#include <QByteArray>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include <xcb/xcb.h>

class QSocketNotifier;
class QTimer;

namespace X11ClipboardProtocol {
struct Atoms {
    xcb_atom_t clipboard = XCB_ATOM_NONE;
    xcb_atom_t targets = XCB_ATOM_NONE;
    xcb_atom_t timestamp = XCB_ATOM_NONE;
    xcb_atom_t multiple = XCB_ATOM_NONE;
    xcb_atom_t incr = XCB_ATOM_NONE;
    xcb_atom_t utf8String = XCB_ATOM_NONE;
    xcb_atom_t string = XCB_ATOM_STRING;
    xcb_atom_t text = XCB_ATOM_NONE;
    xcb_atom_t textPlain = XCB_ATOM_NONE;
    xcb_atom_t textPlainUtf8 = XCB_ATOM_NONE;
    xcb_atom_t applicationOctetStream = XCB_ATOM_NONE;
    xcb_atom_t xdpClipboardData = XCB_ATOM_NONE;
    xcb_atom_t xdpClipboardTargets = XCB_ATOM_NONE;
    xcb_atom_t xdpClipboardTransfer = XCB_ATOM_NONE;
    xcb_atom_t property = XCB_ATOM_NONE;
    xcb_atom_t atom = XCB_ATOM_ATOM;
};

enum class TransferMode {
    Normal,
    Incr,
};

struct SendTransfer {
    xcb_window_t requestor = XCB_WINDOW_NONE;
    xcb_atom_t target = XCB_ATOM_NONE;
    xcb_atom_t property = XCB_ATOM_NONE;
    QByteArray data;
    qsizetype offset = 0;
    qint64 lastActivityMs = 0;
    int chunkSize = 64 * 1024;
    bool incr = false;
};

struct ReceiveTransfer {
    QString sessionPath;
    QString mimeType;
    xcb_atom_t target = XCB_ATOM_NONE;
    xcb_atom_t property = XCB_ATOM_NONE;
    int writeFd = -1;
    qint64 lastActivityMs = 0;
    bool incr = false;
    bool done = false;
    qsizetype bytesReceived = 0;
};

QString canonicalMimeType(QString mimeType);
QStringList mimeAliases(const QString& mimeType);
QVector<xcb_atom_t> targetsForMime(const Atoms& atoms, const QString& mimeType);
QString mimeForTarget(const Atoms& atoms, xcb_atom_t target);
QVector<xcb_atom_t> advertisedTargets(const Atoms& atoms, const QString& mimeType);
std::optional<QVector<xcb_atom_t>> rewriteMultiplePairs(const QVector<xcb_atom_t>& pairs, const QSet<xcb_atom_t>& supportedTargets);
TransferMode chooseTransferMode(qsizetype byteCount, qsizetype maxRequestBytes);
QByteArray nextIncrChunk(SendTransfer& transfer);
bool appendReceivedChunk(ReceiveTransfer& transfer, const QByteArray& chunk);
} // namespace X11ClipboardProtocol

class X11Clipboard : public QObject {
    Q_OBJECT
public:
    explicit X11Clipboard(QObject* parent = nullptr);
    X11Clipboard(xcb_connection_t* connection, int defaultScreen, bool ownsConnection, QObject* parent = nullptr);
    ~X11Clipboard() override;

    bool isSessionAuthorized(const QDBusObjectPath& session);
    void authorizeSession(const QDBusObjectPath& session, bool authorized);
    bool isSessionActive(const QDBusObjectPath& session);
    void setSessionActive(const QDBusObjectPath& session, bool active);

    bool initialize(QString* errorOut = nullptr);
    void shutdown();

    QDBusUnixFileDescriptor requestRead(const QDBusObjectPath& session, const QString& mimeType, QString* errorOut);
    bool requestWrite(const QDBusObjectPath& session, const QString& mimeType, QString* errorOut);
    bool completeWrite(const QDBusObjectPath& session, uint serial, bool success, QString* errorOut);

    QDBusUnixFileDescriptor beginWrite(const QDBusObjectPath& session, const QString& mimeType, uint serial, QString* errorOut);
    void handleXcbEvent(const xcb_generic_event_t* event);

Q_SIGNALS:
    void selectionOwnerChanged(const QDBusObjectPath& session, const QStringList& mimeTypes, bool isOwner);

private:
    struct Private;
    struct SessionState {
        bool authorized = false;
        bool active = false;
    };

    bool sessionAllowed(const QDBusObjectPath& session, QString* errorOut) const;
    void closeSessionTransfers(const QString& sessionPath);

    QHash<QString, SessionState> m_sessionState;
    Private* d = nullptr;
};
