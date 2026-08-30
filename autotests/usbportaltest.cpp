// SPDX-License-Identifier: LGPL-2.0-or-later

#include "usb.h"

#include <QDBusMessage>
#include <QWidget>
#include <QTest>

class UsbPortalTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void createsDialogBeforeApplyingX11Parent()
    {
        QObject parent;
        QWidget parentWindow;
        parentWindow.show();
        QTest::qWait(50);
        UsbPortal portal(&parent);
        uint response = 0;
        QVariantMap results;
        const QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.example.Portal"),
            QStringLiteral("/org/example/Portal"),
            QStringLiteral("org.freedesktop.impl.portal.Usb"),
            QStringLiteral("AcquireDevices"));

        portal.AcquireDevices(QDBusObjectPath(QStringLiteral("/org/freedesktop/portal/desktop/request/test/usb")),
                              QStringLiteral("x11:%1").arg(parentWindow.winId(), 0, 16),
            QStringLiteral("org.example.App"),
            {},
            {},
            message,
            response,
            results);

        QVERIFY(QGuiApplication::topLevelWindows().size() >= 2);
    }
};

QTEST_MAIN(UsbPortalTest)
#include "usbportaltest.moc"
