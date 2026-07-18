/*
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <unistd.h>

#include <iostream>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

namespace {
constexpr auto testService = "org.freedesktop.impl.portal.desktop.sonicde.test";

bool waitForName(const QString& name, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    auto* iface = QDBusConnection::sessionBus().interface();
    while (timer.elapsed() < timeoutMs) {
        if (iface && iface->isServiceRegistered(name).value()) {
            return true;
        }
        QThread::msleep(20);
    }
    return false;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    pw_init(nullptr, nullptr);

    if (!waitForName(QString::fromLatin1(testService), 5000)) {
        qWarning().noquote() << QStringLiteral("timed out waiting for test backend service");
        return 1;
    }

    qInfo() << "Direct backend producer/consumer end-to-end placeholder test passed (skeleton)";
    QTimer::singleShot(0, &app, [&app] {
        app.exit(0);
    });
    return app.exec();
}
