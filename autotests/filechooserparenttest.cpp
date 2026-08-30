// SPDX-License-Identifier: LGPL-2.0-or-later

#include "filechooser.h"

#include <algorithm>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QWidget>
#include <QTest>

class FileChooserParentTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void createsNativeWindowBeforeApplyingX11Parent()
    {
        QObject parent;
        QWidget parentWindow;
        parentWindow.show();
        QTest::qWait(50);
        FileChooserPortal portal(&parent);
        uint response = 0;
        QVariantMap results;
        const QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.example.Portal"),
            QStringLiteral("/org/example/Portal"),
            QStringLiteral("org.freedesktop.impl.portal.FileChooser"),
            QStringLiteral("OpenFile"));

        portal.OpenFile(QDBusObjectPath(QStringLiteral("/org/freedesktop/portal/desktop/request/test/filechooser")),
            QStringLiteral("org.example.App"),
                        QStringLiteral("x11:%1").arg(parentWindow.winId(), 0, 16),
            QStringLiteral("Open File"),
            {},
            message,
            response,
            results);

        QVERIFY(fileDialogWindowExistsBesides(parentWindow));
    }

private:
    static bool fileDialogWindowExistsBesides(const QWidget &parentWindow)
    {
        const auto windows = QGuiApplication::topLevelWindows();
        return std::ranges::any_of(windows, [&parentWindow](QWindow *window) {
            return window != parentWindow.windowHandle();
        });
    }
};

QTEST_MAIN(FileChooserParentTest)
#include "filechooserparenttest.moc"
