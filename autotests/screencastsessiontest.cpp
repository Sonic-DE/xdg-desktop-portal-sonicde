// SPDX-License-Identifier: LGPL-2.0-or-later

#include "dbushelpers.h"
#include "remotedesktop.h"
#include "remotedesktopdialog.h"
#include "screencast.h"
#include "utils.h"

#include <KLocalizedString>

#include <QAction>
#include <QDBusConnection>
#include <QMenu>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

// Exercise the same synchronous startup path used by the X11 portal.
std::pair<PortalResponse::Response, QVariantMap> continueStart(RemoteDesktopSession* session);

class InspectableScreenCastSession : public ScreenCastSession
{
public:
    using ScreenCastSession::ScreenCastSession;
    KStatusNotifierItem* indicator() const { return m_item.get(); }
};

class InspectableRemoteDesktopSession : public RemoteDesktopSession
{
public:
    using RemoteDesktopSession::RemoteDesktopSession;
    KStatusNotifierItem* indicator() const { return m_item.get(); }
};

class ScreenCastSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(QDBusConnection::sessionBus().isConnected());
    }

    void unstartedSessionHasNoIndicator()
    {
        auto session = new InspectableScreenCastSession(nullptr, {}, QStringLiteral("/test/unstarted"));
        QVERIFY(session->isValid());
        QVERIFY(!session->indicator());
        session->showStatusNotifier();
        QVERIFY(!session->indicator());
        QPointer<ScreenCastSession> guard(session);
        session->close();
        QTRY_VERIFY(guard.isNull());
    }

    void screencastIndicator_data()
    {
        QTest::addColumn<uint>("sourceType");
        QTest::addColumn<QString>("overlay");
        QTest::addColumn<int>("streamCount");
        QTest::newRow("monitor") << uint(ScreenCastPortal::Monitor) << QStringLiteral("monitor") << 1;
        QTest::newRow("window") << uint(ScreenCastPortal::Window) << QStringLiteral("window") << 1;
        QTest::newRow("two-monitors") << uint(ScreenCastPortal::Monitor) << QStringLiteral("monitor") << 2;
    }

    void screencastIndicator()
    {
        QFETCH(uint, sourceType);
        QFETCH(QString, overlay);
        QFETCH(int, streamCount);
        const QString appId = QStringLiteral("org.sonicde.PortalIndicatorTest");
        auto session = new InspectableScreenCastSession(nullptr, appId, QStringLiteral("/test/screencast"));
        QVERIFY(session->isValid());
        std::vector<std::unique_ptr<ScreencastingStream>> streams;
        for (int i = 0; i < streamCount; ++i) {
            auto stream = std::make_unique<ScreencastingStream>(nullptr);
            stream->setMetaData({{QStringLiteral("source_type"), sourceType}});
            streams.push_back(std::move(stream));
        }
        session->setStreams(std::move(streams));
        QVERIFY(!session->indicator());

        session->showStatusNotifier();
        QPointer<KStatusNotifierItem> indicator(session->indicator());
        QVERIFY(indicator);
        QCOMPARE(indicator->status(), KStatusNotifierItem::Active);
        QCOMPARE(indicator->iconName(), QStringLiteral("media-record"));
        QCOMPARE(indicator->overlayIconName(), overlay);
        QCOMPARE(indicator->toolTipSubTitle(),
            i18nc("@info:tooltip %1 the app that receives the screencast", "Sharing contents to %1", Utils::applicationName(appId)));
        session->showStatusNotifier();
        QCOMPARE(session->indicator(), indicator.data());

        QPointer<ScreenCastSession> guard(session);
        QSignalSpy closedSpy(session, &Session::closed);
        auto menu = indicator->contextMenu();
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 1);
        menu->actions().front()->trigger();
        QCOMPARE(closedSpy.count(), 1);
        QTRY_VERIFY(guard.isNull());
        QVERIFY(indicator.isNull());
    }

    void lastStreamClosingEndsSession()
    {
        auto session = new InspectableScreenCastSession(nullptr, {}, QStringLiteral("/test/streamclosed"));
        QVERIFY(session->isValid());
        auto stream = std::make_unique<ScreencastingStream>(nullptr);
        auto* streamPointer = stream.get();
        std::vector<std::unique_ptr<ScreencastingStream>> streams;
        streams.push_back(std::move(stream));
        session->setStreams(std::move(streams));
        QPointer<ScreenCastSession> guard(session);
        QSignalSpy closedSpy(session, &Session::closed);
        streamPointer->closeStream();
        QCOMPARE(closedSpy.count(), 1);
        QTRY_VERIFY(guard.isNull());
    }

    void inputOnlyRemoteDesktopHasIndicatorAfterStart()
    {
        auto session = new InspectableRemoteDesktopSession(nullptr, {}, QStringLiteral("/test/inputonly"));
        QVERIFY(session->isValid());
        session->setDeviceTypes(RemoteDesktopPortal::Keyboard | RemoteDesktopPortal::Pointer);
        QVERIFY(!session->indicator());
        const auto [response, results] = continueStart(session);
        QCOMPARE(response, PortalResponse::Success);
        QVERIFY(session->started());
        QVERIFY(session->streams().empty());
        QVERIFY(!results.contains(QStringLiteral("streams")));
        QPointer<KStatusNotifierItem> indicator(session->indicator());
        QVERIFY(indicator);
        QCOMPARE(indicator->status(), KStatusNotifierItem::Active);
        QCOMPARE(indicator->iconName(), QStringLiteral("krfb"));
        QCOMPARE(indicator->toolTipSubTitle(), RemoteDesktopDialog::buildNotificationDescription({}, session->deviceTypes(), false));
        session->showStatusNotifier();
        QCOMPARE(session->indicator(), indicator.data());

        auto menu = indicator->contextMenu();
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 1);
        QPointer<RemoteDesktopSession> guard(session);
        QSignalSpy closedSpy(session, &Session::closed);
        menu->actions().front()->trigger();
        QCOMPARE(closedSpy.count(), 1);
        QTRY_VERIFY(guard.isNull());
        QVERIFY(indicator.isNull());
    }

    void failedRemoteDesktopStartHasNoIndicator()
    {
        auto session = new InspectableRemoteDesktopSession(nullptr, {}, QStringLiteral("/test/failed"));
        QVERIFY(session->isValid());
        session->setScreenSharingEnabled(true);
        const auto [response, results] = continueStart(session);
        QCOMPARE(response, PortalResponse::OtherError);
        QVERIFY(results.isEmpty());
        QVERIFY(!session->started());
        QVERIFY(!session->indicator());
        QPointer<RemoteDesktopSession> guard(session);
        session->close();
        QTRY_VERIFY(guard.isNull());
    }
};

QTEST_MAIN(ScreenCastSessionTest)
#include "screencastsessiontest.moc"
