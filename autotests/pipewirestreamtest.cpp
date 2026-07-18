/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QSignalSpy>
#include <QTest>

#include "../src/x11/pipewirestream.h"

#include <limits>

#include <spa/buffer/buffer.h>

class PipeWireStreamTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void frameExchangeKeepsLatestThreeAndTakesLatest();
    void frameExchangeRejectsAfterClose();
    void lifecycleTransitions();
    void rgbxByteOrder();
    void paddedRowStride();
    void producerBufferSizeAndChunkStride();
    void sizeOverflowRejection();
    void retryReusesLastOrBlankFrame();
    void fatalErrorClosesExactlyOnce();
};

static QByteArray renderFrame(PipeWireStream& stream, uint32_t size, spa_chunk* chunk = nullptr)
{
    QByteArray buffer(qsizetype(size), char(0));
    if (!stream.renderNextFrameForTest(buffer.data(), size, chunk)) {
        return {};
    }
    return buffer;
}

void PipeWireStreamTest::frameExchangeKeepsLatestThreeAndTakesLatest()
{
    PipeWireStream::FrameExchange exchange;

    for (int i = 1; i <= 5; ++i) {
        PipeWireStream::Frame frame;
        frame.sequence = i;
        frame.data = QByteArray::number(i);
        QVERIFY(exchange.push(std::move(frame)));
    }

    QCOMPARE(exchange.queuedCount(), 3);
    const auto latest = exchange.takeLatest();
    QVERIFY(latest.has_value());
    QCOMPARE(latest->sequence, 5);
    QCOMPARE(latest->data, QByteArray("5"));
    QCOMPARE(exchange.queuedCount(), 0);
    QVERIFY(!exchange.takeLatest().has_value());
}

void PipeWireStreamTest::frameExchangeRejectsAfterClose()
{
    PipeWireStream::FrameExchange exchange;
    PipeWireStream::Frame first;
    first.data = QByteArray("first");
    first.sequence = 1;
    QVERIFY(exchange.push(std::move(first)));

    exchange.close();

    QVERIFY(exchange.isClosed());
    QCOMPARE(exchange.queuedCount(), 0);
    PipeWireStream::Frame second;
    second.data = QByteArray("second");
    second.sequence = 2;
    QVERIFY(!exchange.push(std::move(second)));
    QVERIFY(!exchange.takeLatest().has_value());
}

void PipeWireStreamTest::lifecycleTransitions()
{
    PipeWireStream::LifecycleState state;

    QCOMPARE(state.state(), PipeWireStream::State::Idle);
    QVERIFY(!state.isReady());
    QVERIFY(state.markConnecting());
    QCOMPARE(state.state(), PipeWireStream::State::Connecting);
    QVERIFY(!state.markCreated(0));
    QVERIFY(state.markCreated(42));
    QCOMPARE(state.nodeId(), 42u);
    QVERIFY(state.isReady());
    QVERIFY(state.markStreaming());
    QCOMPARE(state.state(), PipeWireStream::State::Streaming);
    QVERIFY(state.markClosed());
    QVERIFY(state.isTerminal());
    QVERIFY(!state.markConnecting());
    QVERIFY(!state.markCreated(43));
    QCOMPARE(state.nodeId(), 42u);
}

void PipeWireStreamTest::rgbxByteOrder()
{
    PipeWireStream stream;
    stream.configure(QSize(3, 1), PipeWireStream::PixelFormat::RGBx, 30, false);
    stream.setFrameProviderForTest([] {
        PipeWireStream::Frame frame;
        frame.size = QSize(3, 1);
        frame.stride = 12;
        frame.format = PipeWireStream::PixelFormat::RGBx;
        frame.data = QByteArray::fromRawData("\xff\x00\x00\xff\x00\xff\x00\xff\x00\x00\xff\xff", 12);
        return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
    });

    const QByteArray rendered = renderFrame(stream, 12);
    QCOMPARE(rendered.mid(0, 4), QByteArray("\xff\x00\x00\xff", 4));
    QCOMPARE(rendered.mid(4, 4), QByteArray("\x00\xff\x00\xff", 4));
    QCOMPARE(rendered.mid(8, 4), QByteArray("\x00\x00\xff\xff", 4));
}

void PipeWireStreamTest::paddedRowStride()
{
    PipeWireStream stream;
    stream.configure(QSize(2, 2), PipeWireStream::PixelFormat::RGBx, 30, false);
    stream.setFrameProviderForTest([] {
        PipeWireStream::Frame frame;
        frame.size = QSize(2, 2);
        frame.stride = 12;
        frame.format = PipeWireStream::PixelFormat::RGBx;
        frame.data = QByteArray("abcdefgh1234ijklmnop5678", 24);
        return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
    });

    const QByteArray rendered = renderFrame(stream, 16);
    QCOMPARE(rendered, QByteArray("abcdefghijklmnop", 16));
}

void PipeWireStreamTest::producerBufferSizeAndChunkStride()
{
    PipeWireStream stream;
    stream.configure(QSize(5, 4), PipeWireStream::PixelFormat::RGBx, 30, false);
    stream.setFrameProviderForTest([] {
        return PipeWireStream::FrameProviderResult::retry();
    });

    spa_chunk chunk = {};
    const QByteArray rendered = renderFrame(stream, 80, &chunk);
    QCOMPARE(rendered.size(), 80);
    QCOMPARE(chunk.offset, 0u);
    QCOMPARE(chunk.size, 80u);
    QCOMPARE(chunk.stride, 20);
    QCOMPARE(chunk.flags, SPA_CHUNK_FLAG_NONE);
}

void PipeWireStreamTest::sizeOverflowRejection()
{
    PipeWireStream stream;
    QSignalSpy failedSpy(&stream, &PipeWireStream::failed);
    stream.configure(QSize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()), PipeWireStream::PixelFormat::RGBx, 30, false);

    QCOMPARE(stream.state(), PipeWireStream::State::Failed);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(stream.error().contains(QStringLiteral("Invalid PipeWire stream dimensions")));
}

void PipeWireStreamTest::retryReusesLastOrBlankFrame()
{
    PipeWireStream stream;
    stream.configure(QSize(1, 1), PipeWireStream::PixelFormat::RGBx, 30, false);
    int calls = 0;
    stream.setFrameProviderForTest([&calls] {
        ++calls;
        if (calls == 1) {
            PipeWireStream::Frame frame;
            frame.size = QSize(1, 1);
            frame.stride = 4;
            frame.format = PipeWireStream::PixelFormat::RGBx;
            frame.data = QByteArray("\x01\x02\x03\x04", 4);
            return PipeWireStream::FrameProviderResult::frameReady(std::move(frame));
        }
        return PipeWireStream::FrameProviderResult::retry();
    });

    QCOMPARE(renderFrame(stream, 4), QByteArray("\x01\x02\x03\x04", 4));
    QCOMPARE(renderFrame(stream, 4), QByteArray("\x01\x02\x03\x04", 4));

    PipeWireStream blankStream;
    blankStream.configure(QSize(1, 1), PipeWireStream::PixelFormat::RGBx, 30, false);
    blankStream.setFrameProviderForTest([] {
        return PipeWireStream::FrameProviderResult::retry();
    });
    QCOMPARE(renderFrame(blankStream, 4), QByteArray(4, char(0)));
}

void PipeWireStreamTest::fatalErrorClosesExactlyOnce()
{
    PipeWireStream stream;
    stream.configure(QSize(1, 1), PipeWireStream::PixelFormat::RGBx, 30, false);
    QSignalSpy failedSpy(&stream, &PipeWireStream::failed);
    QSignalSpy closedSpy(&stream, &PipeWireStream::closed);
    stream.setFrameProviderForTest([] {
        return PipeWireStream::FrameProviderResult::fatalError(QStringLiteral("provider stopped"));
    });

    QByteArray buffer(4, Qt::Uninitialized);
    QVERIFY(!stream.renderNextFrameForTest(buffer.data(), uint32_t(buffer.size())));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(stream.state(), PipeWireStream::State::Failed);
    QCOMPARE(stream.error(), QStringLiteral("provider stopped"));

    QTRY_COMPARE(closedSpy.count(), 1);
    stream.close();
    QCOMPARE(closedSpy.count(), 1);
}

QTEST_GUILESS_MAIN(PipeWireStreamTest)

#include "pipewirestreamtest.moc"
