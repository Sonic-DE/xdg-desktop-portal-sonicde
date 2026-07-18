/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewirestream.h"

#include <QCoreApplication>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/format.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

namespace {
constexpr uint32_t s_bufferCount = 3;
constexpr uint32_t s_defaultWidth = 1;
constexpr uint32_t s_defaultHeight = 1;
constexpr uint32_t s_cursorBitmapBytes = 64 * 64 * 4;
constexpr uint32_t s_cursorMetaSize = SPA_ROUND_UP_N(sizeof(spa_meta_cursor), SPA_POD_ALIGN)
    + SPA_ROUND_UP_N(sizeof(spa_meta_bitmap), SPA_POD_ALIGN) + s_cursorBitmapBytes;

const pw_stream_events* pipeWireStreamEvents()
{
    static const pw_stream_events events = {
        PW_VERSION_STREAM_EVENTS,
        nullptr,
        PipeWireStream::onStreamStateChanged,
        nullptr,
        nullptr,
        PipeWireStream::onStreamParamChanged,
        nullptr,
        nullptr,
        PipeWireStream::onStreamProcess,
        nullptr,
        nullptr,
        nullptr,
    };
    return &events;
}

void ensurePipeWireInitialized()
{
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    Q_UNUSED(initialized)
}

uint32_t spaFormat(PipeWireStream::PixelFormat format)
{
    switch (format) {
    case PipeWireStream::PixelFormat::RGBx:
        return SPA_VIDEO_FORMAT_RGBx;
    case PipeWireStream::PixelFormat::BGRx:
        return SPA_VIDEO_FORMAT_BGRx;
    }
    return SPA_VIDEO_FORMAT_BGRx;
}

PipeWireStream::PixelFormat streamFormat(uint32_t format)
{
    return format == SPA_VIDEO_FORMAT_RGBx ? PipeWireStream::PixelFormat::RGBx : PipeWireStream::PixelFormat::BGRx;
}

uint32_t safeWidth(const QSize& size)
{
    return uint32_t(std::max(size.width(), int(s_defaultWidth)));
}

uint32_t safeHeight(const QSize& size)
{
    return uint32_t(std::max(size.height(), int(s_defaultHeight)));
}

bool checkedFrameGeometry(const QSize& size, uint32_t* width, uint32_t* height, uint32_t* stride, uint32_t* bufferSize)
{
    if (size.width() <= 0 || size.height() <= 0) {
        return false;
    }

    constexpr qsizetype bytesPerPixel = 4;
    const qsizetype checkedWidth = qsizetype(size.width());
    const qsizetype checkedHeight = qsizetype(size.height());
    if (checkedWidth > std::numeric_limits<qsizetype>::max() / bytesPerPixel) {
        return false;
    }

    const qsizetype checkedStride = checkedWidth * bytesPerPixel;
    if (checkedHeight > std::numeric_limits<qsizetype>::max() / checkedStride) {
        return false;
    }

    const qsizetype checkedBufferSize = checkedStride * checkedHeight;
    if (checkedWidth > std::numeric_limits<uint32_t>::max() || checkedHeight > std::numeric_limits<uint32_t>::max()
        || checkedStride > std::numeric_limits<uint32_t>::max() || checkedBufferSize > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    *width = uint32_t(checkedWidth);
    *height = uint32_t(checkedHeight);
    *stride = uint32_t(checkedStride);
    *bufferSize = uint32_t(checkedBufferSize);
    return true;
}

QByteArray convertedImageBytes(const QImage& image, PipeWireStream::PixelFormat format, qsizetype stride)
{
    if (image.isNull()) {
        return {};
    }

    const QImage::Format qtFormat = format == PipeWireStream::PixelFormat::RGBx ? QImage::Format_RGBX8888 : QImage::Format_RGB32;
    const QImage converted = image.convertToFormat(qtFormat);
    const qsizetype rowBytes = qsizetype(converted.width()) * 4;
    QByteArray data(stride * converted.height(), Qt::Uninitialized);
    for (int y = 0; y < converted.height(); ++y) {
        memcpy(data.data() + y * stride, converted.constScanLine(y), std::min(rowBytes, stride));
    }
    return data;
}

void fillCursorMetadata(spa_buffer* buffer, const PipeWireStream::Frame& frame, uint64_t sequence)
{
    auto* cursor = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(buffer, SPA_META_Cursor, sizeof(spa_meta_cursor)));
    if (!cursor) {
        return;
    }

    memset(cursor, 0, sizeof(spa_meta_cursor));
    if (!frame.cursor.visible) {
        return;
    }

    cursor->id = uint32_t(std::max<uint64_t>(sequence + 1, 1));
    cursor->position.x = frame.cursor.rect.x();
    cursor->position.y = frame.cursor.rect.y();
    cursor->hotspot.x = frame.cursor.hotspot.x();
    cursor->hotspot.y = frame.cursor.hotspot.y();

    const spa_meta* meta = spa_buffer_find_meta(buffer, SPA_META_Cursor);
    if (!meta || frame.cursor.image.isNull()) {
        return;
    }

    const uint32_t bitmapOffset = SPA_ROUND_UP_N(sizeof(spa_meta_cursor), SPA_POD_ALIGN);
    const uint32_t pixelsOffset = bitmapOffset + SPA_ROUND_UP_N(sizeof(spa_meta_bitmap), SPA_POD_ALIGN);
    const qsizetype stride = qsizetype(frame.cursor.image.width()) * 4;
    const QByteArray pixels = convertedImageBytes(frame.cursor.image, frame.format, stride);
    if (pixels.isEmpty() || pixelsOffset + uint32_t(pixels.size()) > meta->size) {
        return;
    }

    auto* bitmap = SPA_PTROFF(cursor, bitmapOffset, spa_meta_bitmap);
    memset(bitmap, 0, sizeof(spa_meta_bitmap));
    bitmap->format = spaFormat(frame.format);
    bitmap->size.width = uint32_t(frame.cursor.image.width());
    bitmap->size.height = uint32_t(frame.cursor.image.height());
    bitmap->stride = int32_t(stride);
    bitmap->offset = pixelsOffset - bitmapOffset;
    memcpy(SPA_PTROFF(bitmap, bitmap->offset, void), pixels.constData(), size_t(pixels.size()));
    cursor->bitmap_offset = bitmapOffset;
}
}

PipeWireStream::FrameProviderResult PipeWireStream::FrameProviderResult::frameReady(Frame frame)
{
    FrameProviderResult result;
    result.status = FrameProviderStatus::FrameReady;
    result.frame = std::move(frame);
    return result;
}

PipeWireStream::FrameProviderResult PipeWireStream::FrameProviderResult::retry()
{
    return {};
}

PipeWireStream::FrameProviderResult PipeWireStream::FrameProviderResult::fatalError(QString error)
{
    FrameProviderResult result;
    result.status = FrameProviderStatus::FatalError;
    result.error = std::move(error);
    return result;
}

bool PipeWireStream::FrameExchange::push(Frame frame)
{
    QMutexLocker locker(&m_mutex);
    if (m_closed) {
        return false;
    }

    if (m_count == qsizetype(m_slots.size())) {
        m_slots[m_start].reset();
        m_start = (m_start + 1) % qsizetype(m_slots.size());
        --m_count;
    }

    const qsizetype index = (m_start + m_count) % qsizetype(m_slots.size());
    m_slots[index] = std::move(frame);
    ++m_count;
    return true;
}

std::optional<PipeWireStream::Frame> PipeWireStream::FrameExchange::takeLatest()
{
    QMutexLocker locker(&m_mutex);
    if (m_count == 0) {
        return std::nullopt;
    }

    const qsizetype latest = (m_start + m_count - 1) % qsizetype(m_slots.size());
    std::optional<Frame> frame = std::move(m_slots[latest]);
    for (auto& slot : m_slots) {
        slot.reset();
    }
    m_start = 0;
    m_count = 0;
    return frame;
}

void PipeWireStream::FrameExchange::close()
{
    QMutexLocker locker(&m_mutex);
    m_closed = true;
    for (auto& slot : m_slots) {
        slot.reset();
    }
    m_start = 0;
    m_count = 0;
}

qsizetype PipeWireStream::FrameExchange::queuedCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_count;
}

bool PipeWireStream::FrameExchange::isClosed() const
{
    QMutexLocker locker(&m_mutex);
    return m_closed;
}

PipeWireStream::State PipeWireStream::LifecycleState::state() const
{
    return m_state;
}

quint32 PipeWireStream::LifecycleState::nodeId() const
{
    return m_nodeId;
}

QString PipeWireStream::LifecycleState::error() const
{
    return m_error;
}

bool PipeWireStream::LifecycleState::isReady() const
{
    return m_nodeId != 0 && (m_state == State::Ready || m_state == State::Streaming);
}

bool PipeWireStream::LifecycleState::isTerminal() const
{
    return m_state == State::Failed || m_state == State::Closed;
}

bool PipeWireStream::LifecycleState::markConnecting()
{
    if (isTerminal()) {
        return false;
    }
    m_state = State::Connecting;
    return true;
}

bool PipeWireStream::LifecycleState::markCreated(quint32 nodeId)
{
    if (nodeId == 0 || nodeId == SPA_ID_INVALID || isTerminal()) {
        return false;
    }
    const bool changed = m_nodeId != nodeId || m_state != State::Ready;
    m_nodeId = nodeId;
    m_state = State::Ready;
    return changed;
}

bool PipeWireStream::LifecycleState::markStreaming()
{
    if (isTerminal()) {
        return false;
    }
    const bool changed = m_state != State::Streaming;
    m_state = State::Streaming;
    return changed;
}

bool PipeWireStream::LifecycleState::markFailed(const QString& error)
{
    if (m_state == State::Failed && m_error == error) {
        return false;
    }
    m_state = State::Failed;
    m_error = error;
    return true;
}

bool PipeWireStream::LifecycleState::markClosed()
{
    if (m_state == State::Closed) {
        return false;
    }
    m_state = State::Closed;
    return true;
}

PipeWireStream::PipeWireStream(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<PipeWireStream::Frame>();
    qRegisterMetaType<PipeWireStream::FrameProviderResult>();
}

PipeWireStream::~PipeWireStream()
{
    close();
}

QVariantMap PipeWireStream::metadata() const
{
    return m_metadata;
}

void PipeWireStream::setMetadata(const QVariantMap& metadata)
{
    m_metadata = metadata;
}

void PipeWireStream::configure(const QSize& size, PixelFormat format, int maxFramerate, bool cursorMetadata)
{
    QMutexLocker locker(&m_frameMutex);
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t bufferSize = 0;
    if (!checkedFrameGeometry(size, &width, &height, &stride, &bufferSize)) {
        locker.unlock();
        fail(QStringLiteral("Invalid PipeWire stream dimensions %1x%2").arg(size.width()).arg(size.height()));
        return;
    }

    m_size = QSize(int(width), int(height));
    m_format = format;
    m_maxFramerate = std::max(maxFramerate, 1);
    m_cursorMetadata = cursorMetadata;
    m_stride = stride;
    m_bufferSize = bufferSize;
}

bool PipeWireStream::start(FrameProvider provider)
{
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_lifecycle.isReady() || m_lifecycle.state() == State::Connecting || m_lifecycle.state() == State::Streaming) {
            return true;
        }
        if (m_lifecycle.isTerminal()) {
            return false;
        }
        m_lifecycle.markConnecting();
    }

    m_frameProvider = std::move(provider);
    if (!m_size.isValid() && state() != State::Failed) {
        configure(QSize(int(s_defaultWidth), int(s_defaultHeight)), m_format, m_maxFramerate, m_cursorMetadata);
    }
    if (state() == State::Failed) {
        return false;
    }

    ensurePipeWireInitialized();

    m_loop = pw_thread_loop_new("xdp-sonicde-screencast", nullptr);
    if (!m_loop) {
        fail(QStringLiteral("Failed to create PipeWire thread loop: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (pw_thread_loop_start(m_loop) < 0) {
        fail(QStringLiteral("Failed to start PipeWire thread loop: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        close();
        return false;
    }

    pw_thread_loop_lock(m_loop);
    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context) {
        pw_thread_loop_unlock(m_loop);
        fail(QStringLiteral("Failed to create PipeWire context: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        close();
        return false;
    }

    m_core = pw_context_connect(m_context, nullptr, 0);
    if (!m_core) {
        pw_thread_loop_unlock(m_loop);
        fail(QStringLiteral("Failed to connect to PipeWire: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        close();
        return false;
    }

    auto* properties = pw_properties_new(PW_KEY_MEDIA_TYPE,
        "Video",
        PW_KEY_MEDIA_CATEGORY,
        "Capture",
        PW_KEY_MEDIA_ROLE,
        "Screen",
        PW_KEY_MEDIA_CLASS,
        "Video/Source",
        PW_KEY_NODE_NAME,
        "sonicde.portal.screencast",
        PW_KEY_NODE_DESCRIPTION,
        "SonicDE screen cast",
        PW_KEY_NODE_VIRTUAL,
        "true",
        PW_KEY_NODE_ALWAYS_PROCESS,
        "true",
        PW_KEY_NODE_DONT_RECONNECT,
        "true",
        nullptr);
    m_stream = pw_stream_new(m_core, "SonicDE Screen Cast", properties);
    if (!m_stream) {
        pw_thread_loop_unlock(m_loop);
        fail(QStringLiteral("Failed to create PipeWire stream: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        close();
        return false;
    }

    m_streamListener = new spa_hook;
    pw_stream_add_listener(m_stream, m_streamListener, pipeWireStreamEvents(), this);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_rectangle rectangle = SPA_RECTANGLE(uint32_t(m_size.width()), uint32_t(m_size.height()));
    const spa_fraction framerate = SPA_FRACTION(0, 1);
    const spa_fraction maxFramerate = SPA_FRACTION(uint32_t(m_maxFramerate), 1);
    const spa_pod* params[1];
    params[0] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(2, spaFormat(m_format), SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_Rectangle(&rectangle),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_Fraction(&framerate),
        SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_Fraction(&maxFramerate)));

    const int result = pw_stream_connect(m_stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        pw_stream_flags(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_TRIGGER),
        params,
        1);
    if (result < 0) {
        pw_thread_loop_unlock(m_loop);
        fail(QStringLiteral("Failed to connect PipeWire stream: %1").arg(QString::fromLocal8Bit(strerror(-result))));
        close();
        return false;
    }
    pw_thread_loop_unlock(m_loop);

    return true;
}

bool PipeWireStream::waitForReady(int timeoutMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_lifecycle.isReady()) {
        return true;
    }
    if (m_lifecycle.isTerminal()) {
        return false;
    }
    if (timeoutMs <= 0) {
        return false;
    }
    m_stateWait.wait(&m_stateMutex, timeoutMs);
    return m_lifecycle.isReady();
}

void PipeWireStream::pushFrame(Frame frame)
{
    QMutexLocker locker(&m_frameMutex);
    if (!frame.size.isValid()) {
        frame.size = m_size;
    }
    if (frame.stride <= 0) {
        frame.stride = qsizetype(safeWidth(frame.size)) * 4;
    }
    if (frame.sequence == 0) {
        frame.sequence = ++m_sequence;
    }
    m_exchange.push(std::move(frame));
    if (m_stream) {
        pw_stream_trigger_process(m_stream);
    }
}

void PipeWireStream::setFrameProviderForTest(FrameProvider provider)
{
    QMutexLocker locker(&m_frameMutex);
    m_frameProvider = std::move(provider);
}

bool PipeWireStream::renderNextFrameForTest(void* data, uint32_t maxsize, spa_chunk* chunk)
{
    QMutexLocker locker(&m_frameMutex);
    return fillFrameData(data, maxsize, chunk);
}

void PipeWireStream::close()
{
    m_exchange.close();

    if (m_loop) {
        pw_thread_loop_lock(m_loop);
    }
    if (m_stream) {
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    delete m_streamListener;
    m_streamListener = nullptr;
    if (m_core) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_loop) {
        pw_thread_loop_unlock(m_loop);
        pw_thread_loop_stop(m_loop);
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }

    bool emitClosed = false;
    {
        QMutexLocker locker(&m_stateMutex);
        m_lifecycle.markClosed();
        emitClosed = !m_closedEmitted;
        m_closedEmitted = true;
        notifyWaiters();
    }
    if (emitClosed) {
        Q_EMIT closed();
    }
}

quint32 PipeWireStream::nodeId() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_lifecycle.nodeId();
}

PipeWireStream::State PipeWireStream::state() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_lifecycle.state();
}

QString PipeWireStream::error() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_lifecycle.error();
}

bool PipeWireStream::isReady() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_lifecycle.isReady();
}

void PipeWireStream::onStreamStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error)
{
    auto* stream = static_cast<PipeWireStream*>(data);
    stream->setStateFromPipeWire(state, QString::fromUtf8(error ? error : ""));
}

void PipeWireStream::onStreamParamChanged(void* data, uint32_t id, const spa_pod* param)
{
    if (id != SPA_PARAM_Format || !param) {
        return;
    }
    auto* stream = static_cast<PipeWireStream*>(data);
    stream->updateNegotiatedFormat(param);
}

void PipeWireStream::onStreamProcess(void* data)
{
    auto* stream = static_cast<PipeWireStream*>(data);
    stream->processFrame();
}

void PipeWireStream::setStateFromPipeWire(pw_stream_state state, const QString& error)
{
    switch (state) {
    case PW_STREAM_STATE_ERROR:
        fail(error.isEmpty() ? QStringLiteral("PipeWire stream entered error state") : error);
        return;
    case PW_STREAM_STATE_CONNECTING: {
        QMutexLocker locker(&m_stateMutex);
        m_lifecycle.markConnecting();
        notifyWaiters();
    }
        return;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        return;
    }

    quint32 id = 0;
    if (m_stream) {
        id = pw_stream_get_node_id(m_stream);
    }

    bool emitCreated = false;
    {
        QMutexLocker locker(&m_stateMutex);
        emitCreated = m_lifecycle.markCreated(id);
        if (state == PW_STREAM_STATE_STREAMING) {
            m_lifecycle.markStreaming();
        }
        notifyWaiters();
    }
    if (emitCreated) {
        Q_EMIT created(id);
    }
}

void PipeWireStream::fail(const QString& error)
{
    bool emitFailed = false;
    {
        QMutexLocker locker(&m_stateMutex);
        emitFailed = m_lifecycle.markFailed(error);
        notifyWaiters();
    }
    if (emitFailed) {
        Q_EMIT failed(error);
    }
}

void PipeWireStream::updateNegotiatedFormat(const spa_pod* param)
{
    QMutexLocker locker(&m_frameMutex);
    spa_video_info_raw info = {};
    if (spa_format_video_raw_parse(param, &info) < 0) {
        fail(QStringLiteral("PipeWire negotiated an invalid video format"));
        return;
    }

    if (info.format != SPA_VIDEO_FORMAT_BGRx && info.format != SPA_VIDEO_FORMAT_RGBx) {
        fail(QStringLiteral("PipeWire negotiated unsupported video format %1").arg(info.format));
        return;
    }

    m_format = streamFormat(info.format);
    QSize negotiatedSize = m_size;
    if (info.size.width > 0 && info.size.height > 0 && info.size.width <= uint32_t(std::numeric_limits<int>::max())
        && info.size.height <= uint32_t(std::numeric_limits<int>::max())) {
        negotiatedSize = QSize(int(info.size.width), int(info.size.height));
    }
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t bufferSize = 0;
    if (!checkedFrameGeometry(negotiatedSize, &width, &height, &stride, &bufferSize)) {
        fail(QStringLiteral("PipeWire negotiated invalid video dimensions %1x%2").arg(info.size.width).arg(info.size.height));
        return;
    }
    m_size = QSize(int(width), int(height));
    m_stride = stride;
    m_bufferSize = bufferSize;

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[3];
    uint32_t count = 0;
    params[count++] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamBuffers,
        SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(s_bufferCount, 2, s_bufferCount),
        SPA_PARAM_BUFFERS_blocks,
        SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,
        SPA_POD_Int(int32_t(m_bufferSize)),
        SPA_PARAM_BUFFERS_stride,
        SPA_POD_Int(int32_t(m_stride)),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr)));
    params[count++] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamMeta,
        SPA_PARAM_Meta,
        SPA_PARAM_META_type,
        SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(spa_meta_header))));
    if (m_cursorMetadata) {
        params[count++] = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta,
            SPA_PARAM_Meta,
            SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Cursor),
            SPA_PARAM_META_size,
            SPA_POD_Int(s_cursorMetaSize)));
    }
    pw_stream_update_params(m_stream, params, count);
}

void PipeWireStream::processFrame()
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_stream) {
        return;
    }

    pw_buffer* pwBuffer = pw_stream_dequeue_buffer(m_stream);
    if (!pwBuffer) {
        return;
    }

    spa_buffer* buffer = pwBuffer->buffer;
    if (!buffer || buffer->n_datas == 0 || !buffer->datas[0].data || buffer->datas[0].maxsize < m_bufferSize) {
        pw_stream_return_buffer(m_stream, pwBuffer);
        return;
    }

    auto& data = buffer->datas[0];
    if (!fillFrameData(data.data, data.maxsize, data.chunk)) {
        pw_stream_return_buffer(m_stream, pwBuffer);
        return;
    }

    const Frame& frame = m_lastFrame;
    auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));
    if (header) {
        header->flags = 0;
        header->offset = 0;
        header->pts = int64_t(pw_stream_get_nsec(m_stream));
        header->dts_offset = 0;
        header->seq = uint64_t(frame.sequence);
    }

    if (m_cursorMetadata) {
        fillCursorMetadata(buffer, frame, uint64_t(frame.sequence));
    }

    pwBuffer->size = uint64_t(safeHeight(m_size));
    pw_stream_queue_buffer(m_stream, pwBuffer);
}

void PipeWireStream::notifyWaiters()
{
    m_stateWait.wakeAll();
    if (m_loop) {
        pw_thread_loop_signal(m_loop, false);
    }
}

std::optional<PipeWireStream::Frame> PipeWireStream::nextFrame()
{
    std::optional<Frame> frame;
    if (m_frameProvider) {
        FrameProviderResult result = m_frameProvider();
        switch (result.status) {
        case FrameProviderStatus::FrameReady:
            frame = std::move(result.frame);
            break;
        case FrameProviderStatus::Retry:
            break;
        case FrameProviderStatus::FatalError:
            m_frameProvider = {};
            fail(result.error.isEmpty() ? QStringLiteral("Frame provider failed") : result.error);
            closeFromPipeWireThread();
            return std::nullopt;
        }
    }
    if (!frame) {
        frame = m_exchange.takeLatest();
    }
    if (frame) {
        m_lastFrame = std::move(*frame);
        m_haveLastFrame = true;
    }

    if (!m_haveLastFrame) {
        m_lastFrame = Frame{QByteArray(qsizetype(m_bufferSize), char(0)), m_size, qsizetype(m_stride), m_format, {}, ++m_sequence};
        m_haveLastFrame = true;
    }
    return m_lastFrame;
}

bool PipeWireStream::fillFrameData(void* data, uint32_t maxsize, spa_chunk* chunk)
{
    if (!data || maxsize < m_bufferSize) {
        return false;
    }

    std::optional<Frame> frame = nextFrame();
    if (!frame) {
        return false;
    }

    const qsizetype sourceStride = frame->stride > 0 ? frame->stride : qsizetype(m_stride);
    const qsizetype sourceHeight = frame->size.isValid() ? frame->size.height() : m_size.height();
    if (sourceStride <= 0 || sourceHeight <= 0 || sourceHeight > std::numeric_limits<qsizetype>::max() / sourceStride) {
        return false;
    }

    const qsizetype requiredSourceSize = sourceStride * sourceHeight;
    const qsizetype copyStride = std::min<qsizetype>(sourceStride, m_stride);
    const int copyHeight = std::min<int>(m_size.height(), int(sourceHeight));
    memset(data, 0, size_t(m_bufferSize));
    if (sourceStride > 0 && sourceHeight > 0 && frame->data.size() >= requiredSourceSize) {
        auto* destination = static_cast<char*>(data);
        for (int y = 0; y < copyHeight; ++y) {
            memcpy(destination + qsizetype(y) * qsizetype(m_stride), frame->data.constData() + qsizetype(y) * sourceStride, size_t(copyStride));
        }
    }

    if (m_lastFrame.sequence == 0) {
        m_lastFrame.sequence = ++m_sequence;
    }
    m_lastFrame.format = m_format;
    m_lastFrame.size = m_size;
    m_lastFrame.stride = qsizetype(m_stride);
    m_lastFrame.data = QByteArray(static_cast<const char*>(data), qsizetype(m_bufferSize));
    if (chunk) {
        chunk->offset = 0;
        chunk->size = m_bufferSize;
        chunk->stride = int32_t(m_stride);
        chunk->flags = SPA_CHUNK_FLAG_NONE;
    }
    return true;
}

void PipeWireStream::closeFromPipeWireThread()
{
    QPointer<PipeWireStream> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (guard) {
            guard->close();
        }
    });
}
