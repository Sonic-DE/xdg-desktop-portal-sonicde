/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QTest>

#include "../src/x11/x11capture.h"

extern "C" {
#include <xcb/xcb.h>
}

class X11CaptureTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testBounds();
    void testRgb32Conversion();
    void testRgb24Conversion();
    void testInvalidConversion();
    void testCursorComposition();
    void testCursorCompositionClipped();
};

void X11CaptureTest::testBounds()
{
    QCOMPARE(X11Capture::boundedCaptureRect(QRect(10, 10, 20, 20), QRect(0, 0, 100, 100)), QRect(10, 10, 20, 20));
    QCOMPARE(X11Capture::boundedCaptureRect(QRect(-10, -5, 20, 20), QRect(0, 0, 100, 100)), QRect(0, 0, 10, 15));
    QVERIFY(X11Capture::boundedCaptureRect(QRect(120, 120, 10, 10), QRect(0, 0, 100, 100)).isEmpty());
    QVERIFY(X11Capture::boundedCaptureRect(QRect(0, 0, 0, 10), QRect(0, 0, 100, 100)).isEmpty());
}

void X11CaptureTest::testRgb32Conversion()
{
    QByteArray data;
    data.resize(8);
    auto* bytes = reinterpret_cast<uchar*>(data.data());
    bytes[0] = 0x33;
    bytes[1] = 0x22;
    bytes[2] = 0x11;
    bytes[3] = 0x00;
    bytes[4] = 0xcc;
    bytes[5] = 0xbb;
    bytes[6] = 0xaa;
    bytes[7] = 0x00;

    const QImage image = X11Capture::convertToRgb32(data, QSize(2, 1), 8, 32, XCB_IMAGE_ORDER_LSB_FIRST, 0x00ff0000, 0x0000ff00, 0x000000ff);
    QCOMPARE(image.format(), QImage::Format_RGB32);
    QCOMPARE(image.size(), QSize(2, 1));
    QCOMPARE(image.pixel(0, 0), qRgb(0x11, 0x22, 0x33));
    QCOMPARE(image.pixel(1, 0), qRgb(0xaa, 0xbb, 0xcc));
}

void X11CaptureTest::testRgb24Conversion()
{
    QByteArray data;
    data.resize(6);
    auto* bytes = reinterpret_cast<uchar*>(data.data());
    bytes[0] = 0x12;
    bytes[1] = 0x34;
    bytes[2] = 0x56;
    bytes[3] = 0xab;
    bytes[4] = 0xcd;
    bytes[5] = 0xef;

    const QImage image = X11Capture::convertToRgb32(data, QSize(2, 1), 6, 24, XCB_IMAGE_ORDER_MSB_FIRST, 0x00ff0000, 0x0000ff00, 0x000000ff);
    QCOMPARE(image.format(), QImage::Format_RGB32);
    QCOMPARE(image.pixel(0, 0), qRgb(0x12, 0x34, 0x56));
    QCOMPARE(image.pixel(1, 0), qRgb(0xab, 0xcd, 0xef));
}

void X11CaptureTest::testInvalidConversion()
{
    QVERIFY(X11Capture::convertToRgb32({}, QSize(1, 1), 4, 32, XCB_IMAGE_ORDER_LSB_FIRST, 0xff0000, 0x00ff00, 0x0000ff).isNull());
    QVERIFY(X11Capture::convertToRgb32(QByteArray(4, '\0'), QSize(1, 1), 4, 8, XCB_IMAGE_ORDER_LSB_FIRST, 0xff0000, 0x00ff00, 0x0000ff).isNull());
    QVERIFY(X11Capture::convertToRgb32(QByteArray(4, '\0'), QSize(1, 1), 4, 32, XCB_IMAGE_ORDER_LSB_FIRST, 0, 0x00ff00, 0x0000ff).isNull());
}

void X11CaptureTest::testCursorComposition()
{
    QImage target(QSize(3, 3), QImage::Format_RGB32);
    target.fill(qRgb(10, 20, 30));

    QImage cursor(QSize(2, 2), QImage::Format_ARGB32_Premultiplied);
    cursor.fill(Qt::transparent);
    cursor.setPixel(0, 0, qRgba(100, 0, 0, 255));
    cursor.setPixel(1, 0, qRgba(0, 50, 0, 128));

    X11Capture::compositeCursor(&target, cursor, QPoint(0, 0), QPoint(1, 1), QRect(0, 0, 3, 3));

    QCOMPARE(target.pixel(1, 1), qRgb(100, 0, 0));
    QCOMPARE(target.pixel(2, 1), qRgb(5, 60, 15));
    QCOMPARE(target.pixel(0, 0), qRgb(10, 20, 30));
}

void X11CaptureTest::testCursorCompositionClipped()
{
    QImage target(QSize(2, 2), QImage::Format_RGB32);
    target.fill(qRgb(1, 2, 3));

    QImage cursor(QSize(3, 3), QImage::Format_ARGB32_Premultiplied);
    cursor.fill(qRgba(20, 30, 40, 255));

    X11Capture::compositeCursor(&target, cursor, QPoint(1, 1), QPoint(0, 0), QRect(0, 0, 2, 2));

    QCOMPARE(target.pixel(0, 0), qRgb(20, 30, 40));
    QCOMPARE(target.pixel(1, 1), qRgb(20, 30, 40));
}

QTEST_GUILESS_MAIN(X11CaptureTest)

#include "x11capturetest.moc"
