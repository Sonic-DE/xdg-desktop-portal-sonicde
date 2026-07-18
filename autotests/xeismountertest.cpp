/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QTest>

#include "../src/x11/xeismounter.h"

class XEisMounterTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void roleValidation_data();
    void roleValidation();
    void regionNormalization();
};

void XEisMounterTest::roleValidation_data()
{
    QTest::addColumn<XEisMounter::Mode>("mode");
    QTest::addColumn<bool>("clientIsSender");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("remote-desktop-sender") << XEisMounter::Mode::RemoteDesktopReceiver << true << true;
    QTest::newRow("remote-desktop-receiver") << XEisMounter::Mode::RemoteDesktopReceiver << false << false;
    QTest::newRow("input-capture-receiver") << XEisMounter::Mode::InputCaptureSender << false << true;
    QTest::newRow("input-capture-sender") << XEisMounter::Mode::InputCaptureSender << true << false;
}

void XEisMounterTest::roleValidation()
{
    QFETCH(XEisMounter::Mode, mode);
    QFETCH(bool, clientIsSender);
    QFETCH(bool, accepted);

    QCOMPARE(XEisMounter::clientRoleMatchesMode(mode, clientIsSender), accepted);
}

void XEisMounterTest::regionNormalization()
{
    const QList<QRect> regions = {
        QRect(0, 0, 1920, 1080),
        QRect(0, 0, 1920, 1080),
        QRect(),
        QRect(1920, 0, 1280, 1024),
    };

    QCOMPARE(XEisMounter::normalizedRegions(regions), (QList<QRect>{QRect(0, 0, 1920, 1080), QRect(1920, 0, 1280, 1024)}));
    QCOMPARE(XEisMounter::normalizedRegions({}), (QList<QRect>{QRect(0, 0, 1, 1)}));
}

QTEST_GUILESS_MAIN(XEisMounterTest)

#include "xeismountertest.moc"
