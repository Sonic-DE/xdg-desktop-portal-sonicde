/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QTest>

#include <linux/input-event-codes.h>

#include "../src/x11/x11input.h"

class X11InputTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void linuxButtonMapping_data();
    void linuxButtonMapping();
    void evdevKeycodeValidation_data();
    void evdevKeycodeValidation();
};

void X11InputTest::linuxButtonMapping_data()
{
    QTest::addColumn<int>("linuxButton");
    QTest::addColumn<int>("xButton");

    QTest::newRow("left") << BTN_LEFT << 1;
    QTest::newRow("middle") << BTN_MIDDLE << 2;
    QTest::newRow("right") << BTN_RIGHT << 3;
    QTest::newRow("side") << BTN_SIDE << 8;
    QTest::newRow("extra") << BTN_EXTRA << 9;
    QTest::newRow("forward") << BTN_FORWARD << 10;
    QTest::newRow("back") << BTN_BACK << 11;
    QTest::newRow("task") << BTN_TASK << 12;
    QTest::newRow("unknown") << (BTN_TASK + 1) << 0;
}

void X11InputTest::linuxButtonMapping()
{
    QFETCH(int, linuxButton);
    QFETCH(int, xButton);

    QCOMPARE(X11Input::linuxButtonToXButton(linuxButton), xButton);
}

void X11InputTest::evdevKeycodeValidation_data()
{
    QTest::addColumn<int>("evdevKeycode");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<int>("xKeycode");

    QTest::newRow("negative") << -1 << false << -1;
    QTest::newRow("zero") << 0 << true << 8;
    QTest::newRow("escape") << KEY_ESC << true << 9;
    QTest::newRow("max") << 247 << true << 255;
    QTest::newRow("too-large") << 248 << false << -1;
}

void X11InputTest::evdevKeycodeValidation()
{
    QFETCH(int, evdevKeycode);
    QFETCH(bool, valid);
    QFETCH(int, xKeycode);

    QCOMPARE(X11Input::isValidEvdevKeycode(evdevKeycode), valid);
    QCOMPARE(X11Input::evdevKeycodeToXKeycode(evdevKeycode), xKeycode);
}

QTEST_GUILESS_MAIN(X11InputTest)

#include "x11inputtest.moc"
