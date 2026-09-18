/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "x11/x11worker.h"

#include <QSignalSpy>
#include <QTest>

class X11WorkerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void outputsAreNotPolled();
};

void X11WorkerTest::outputsAreNotPolled()
{
    X11Worker worker;
    QSignalSpy outputsSpy(&worker, &X11Worker::outputsReady);

    worker.initialize();
    if (!worker.isInitialized()) {
        QSKIP("No X11 connection is available");
    }

    QCOMPARE(outputsSpy.count(), 1);
    QTest::qWait(1200);
    QCOMPARE(outputsSpy.count(), 1);

    worker.shutdown();
}

QTEST_GUILESS_MAIN(X11WorkerTest)

#include "x11workertest.moc"
