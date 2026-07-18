// SPDX-License-Identifier: LGPL-2.0-or-later

#include "background.h"
#include "print.h"
#include "screenshot.h"
#include "wallpaper.h"

#include <QTemporaryFile>
#include <QTest>

class PortalHelpersTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void backgroundStates()
    {
        X11Types::WindowDescriptor active;
        active.appId = QStringLiteral("org.example.Active");
        active.mapped = true;
        active.active = true;

        X11Types::WindowDescriptor running;
        running.wmClass = QStringLiteral("running-app");
        running.mapped = true;

        X11Types::WindowDescriptor hidden = running;
        hidden.wmClass = QStringLiteral("hidden-app");
        hidden.skipTaskbar = true;

        const QVariantMap states = BackgroundPortal::appStatesFromWindows({active, running, hidden}, active.appId);
        QCOMPARE(states.value(active.appId).toUInt(), uint(BackgroundPortal::Active));
        QCOMPARE(states.value(running.wmClass).toUInt(), uint(BackgroundPortal::Running));
        QVERIFY(!states.contains(hidden.wmClass));
    }

    void screenshotHelpers()
    {
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::black);
        image.setPixelColor(QPoint(1, 0), QColor::fromRgbF(0.25, 0.5, 0.75));
        const auto color = ScreenshotPortal::colorAtImagePosition(image, QPoint(1, 0));
        QVERIFY(qAbs(color.red - 0.25) < 0.01);
        QVERIFY(qAbs(color.green - 0.5) < 0.01);
        QVERIFY(qAbs(color.blue - 0.75) < 0.01);

        const auto outside = ScreenshotPortal::colorAtImagePosition(image, QPoint(5, 5));
        QCOMPARE(outside.red, 0.0);
        const QString path = ScreenshotPortal::screenshotFilePath(
            QStringLiteral("/tmp"), QDateTime(QDate(2026, 7, 19), QTime(12, 34, 56, 789)));
        QVERIFY(path.endsWith(QStringLiteral("Screenshot_20260719_123456_789.png")));
    }

    void wallpaperLocations()
    {
        bool ok = false;
        QCOMPARE(WallpaperPortal::locationFromSetOn(QStringLiteral("background"), &ok), WallpaperLocation::Desktop);
        QVERIFY(ok);
        QCOMPARE(WallpaperPortal::locationFromSetOn(QStringLiteral("lockscreen"), &ok), WallpaperLocation::Lockscreen);
        QVERIFY(ok);
        QCOMPARE(WallpaperPortal::locationFromSetOn(QStringLiteral("both"), &ok), WallpaperLocation::Both);
        QVERIFY(ok);
        WallpaperPortal::locationFromSetOn(QStringLiteral("invalid"), &ok);
        QVERIFY(!ok);
    }

    void printCommandValidation()
    {
        const auto missing = PrintPortal::commandForPdfPrint({}, {}, QStringLiteral("/definitely/missing/document.pdf"), {});
        QVERIFY(!missing.error.isEmpty());
        QVERIFY(missing.program.isEmpty());

        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("%PDF-1.4\n");
        file.flush();
        const auto command = PrintPortal::commandForPdfPrint(
            QStringLiteral("printer name"), QStringLiteral("title;not-a-command"), file.fileName(), {{QStringLiteral("modal"), true}});
        if (command.error.isEmpty()) {
            QVERIFY(!command.program.isEmpty());
            QVERIFY(command.arguments.contains(file.fileName()));
            QVERIFY(command.arguments.contains(QStringLiteral("title;not-a-command")));
            QVERIFY(!command.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("sh -c")));
        } else {
            QVERIFY(command.error.contains(QStringLiteral("lp")));
        }
    }
};

QTEST_GUILESS_MAIN(PortalHelpersTest)
#include "portalhelperstest.moc"
