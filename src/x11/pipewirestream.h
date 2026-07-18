/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QWaitCondition>

#include <array>
#include <functional>
#include <optional>

#include <pipewire/stream.h>

struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_thread_loop;
struct spa_pod;
struct spa_chunk;
struct spa_hook;

class PipeWireStream : public QObject {
    Q_OBJECT
public:
    enum class PixelFormat {
        BGRx,
        RGBx,
    };
    Q_ENUM(PixelFormat)

    enum class State {
        Idle,
        Connecting,
        Ready,
        Streaming,
        Failed,
        Closed,
    };
    Q_ENUM(State)

    struct CursorInfo {
        QRect rect;
        QPoint hotspot;
        QImage image;
        bool visible = false;
    };

    struct Frame {
        QByteArray data;
        QSize size;
        qsizetype stride = 0;
        PixelFormat format = PixelFormat::BGRx;
        CursorInfo cursor;
        qint64 sequence = 0;
    };

    enum class FrameProviderStatus {
        FrameReady,
        Retry,
        FatalError,
    };
    Q_ENUM(FrameProviderStatus)

    struct FrameProviderResult {
        FrameProviderStatus status = FrameProviderStatus::Retry;
        Frame frame;
        QString error;

        static FrameProviderResult frameReady(Frame frame);
        static FrameProviderResult retry();
        static FrameProviderResult fatalError(QString error);
    };

    class FrameExchange {
    public:
        bool push(Frame frame);
        std::optional<Frame> takeLatest();
        void close();
        qsizetype queuedCount() const;
        bool isClosed() const;

    private:
        mutable QMutex m_mutex;
        std::array<std::optional<Frame>, 3> m_slots;
        qsizetype m_start = 0;
        qsizetype m_count = 0;
        bool m_closed = false;
    };

    class LifecycleState {
    public:
        State state() const;
        quint32 nodeId() const;
        QString error() const;
        bool isReady() const;
        bool isTerminal() const;

        bool markConnecting();
        bool markCreated(quint32 nodeId);
        bool markStreaming();
        bool markFailed(const QString& error);
        bool markClosed();

    private:
        State m_state = State::Idle;
        quint32 m_nodeId = 0;
        QString m_error;
    };

    using FrameProvider = std::function<FrameProviderResult()>;

    explicit PipeWireStream(QObject* parent = nullptr);
    ~PipeWireStream() override;

    QVariantMap metadata() const;
    void setMetadata(const QVariantMap& metadata);

    void configure(const QSize& size, PixelFormat format = PixelFormat::BGRx, int maxFramerate = 30, bool cursorMetadata = true);
    bool start(FrameProvider provider = {});
    bool waitForReady(int timeoutMs = 3000);
    void pushFrame(Frame frame);
    void close();

    quint32 nodeId() const;
    State state() const;
    QString error() const;
    bool isReady() const;

    void setFrameProviderForTest(FrameProvider provider);
    bool renderNextFrameForTest(void* data, uint32_t maxsize, spa_chunk* chunk = nullptr);

    static void onStreamStateChanged(void* data, pw_stream_state oldState, pw_stream_state state, const char* error);
    static void onStreamParamChanged(void* data, uint32_t id, const spa_pod* param);
    static void onStreamProcess(void* data);

Q_SIGNALS:
    void created(quint32 nodeId);
    void failed(const QString& error);
    void closed();

private:
    void setStateFromPipeWire(pw_stream_state state, const QString& error);
    void fail(const QString& error);
    void updateNegotiatedFormat(const spa_pod* param);
    void processFrame();
    void notifyWaiters();
    std::optional<Frame> nextFrame();
    bool fillFrameData(void* data, uint32_t maxsize, spa_chunk* chunk);
    void closeFromPipeWireThread();

    QVariantMap m_metadata;
    QSize m_size;
    PixelFormat m_format = PixelFormat::BGRx;
    int m_maxFramerate = 30;
    bool m_cursorMetadata = true;
    FrameProvider m_frameProvider;
    FrameExchange m_exchange;
    Frame m_lastFrame;
    bool m_haveLastFrame = false;
    qint64 m_sequence = 0;
    uint32_t m_stride = 0;
    uint32_t m_bufferSize = 0;

    mutable QMutex m_frameMutex;
    mutable QMutex m_stateMutex;
    QWaitCondition m_stateWait;
    LifecycleState m_lifecycle;
    bool m_closedEmitted = false;

    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_stream* m_stream = nullptr;
    spa_hook* m_streamListener = nullptr;
};

Q_DECLARE_METATYPE(PipeWireStream::Frame)
Q_DECLARE_METATYPE(PipeWireStream::FrameProviderResult)
