/*
 * SPDX-FileCopyrightText: 2026 David Redondo <kde@david-redondo.de>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "../src/settings.h"

#include <QDBusConnection>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <KConfigGroup>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

auto sorted(auto list)
{
    std::ranges::sort(list);
    return list;
};

static auto expectedVirtualKeyboardKeys =
    sorted(QStringList{u"active"_s, u"activeClientSupportsTextInput"_s, u"available"_s, u"enabled"_s, u"visible"_s, u"willShowOnActive"_s});
static auto expectedTabletmodeKeys = sorted(QStringList{u"enabled"_s, u"available"_s});
static auto expectedFdoKeys = sorted(QStringList{u"color-scheme"_s, u"accent-color"_s, u"reduced-motion"_s});

class SettingsPortalTest : public QObject
{
    Q_OBJECT
private:
    struct ErrorCatcher {
    public:
        QDBusError lastError()
        {
            return std::exchange(_lastError, {});
        }
        auto catcher()
        {
            return std::bind_front(&ErrorCatcher::catchError, this);
        };

    private:
        QDBusError _lastError;
        void catchError(const QDBusError &error)
        {
            _lastError = error;
        }
    } errorCatcher;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(QDBusConnection::sessionBus().isConnected());
    }

    void readAll()
    {
        auto globals = KSharedConfig::openConfig();
        globals->group(u"readAll"_s).writeEntry("needsToHave", "oneKey");
        globals->sync();

        // Inject error handling without constructing the full desktop portal.
        SettingsPortal portal(this, errorCatcher.catcher());

        const auto settings = portal.ReadAll({u""_s});

        QCOMPARE(sorted(settings.value(u"org.freedesktop.appearance"_s).keys()), expectedFdoKeys);
        QCOMPARE(sorted(settings.value(u"org.kde.VirtualKeyboard"_s).keys()), expectedVirtualKeyboardKeys);
        QCOMPARE(sorted(settings.value(u"org.kde.TabletMode"_s).keys()), expectedTabletmodeKeys);
        for (const auto &group : {u"org.kde.VirtualKeyboard"_s, u"org.kde.TabletMode"_s}) {
            for (const auto &value : settings.value(group)) {
                QCOMPARE(value.metaType().id(), QMetaType::Bool);
            }
        }
        auto filtered = settings.keys() | std::views::filter([](const QString &key) {
                            return key.startsWith(u"org.kde.kdeglobals"_s);
                        });
        std::vector<QString> kdeglobals(std::from_range, filtered);
        QCOMPARE_NE(kdeglobals, std::vector<QString>());

        const auto settings2 = portal.ReadAll({});

        QCOMPARE(settings2, settings);
        QVERIFY(!errorCatcher.lastError().isValid());
    }

    void readAllWithSpecificGroup()
    {
        SettingsPortal portal(this, errorCatcher.catcher());

        const auto settings = portal.ReadAll({u"org.freedesktop.appearance"_s});

        QCOMPARE(settings.keys(), QStringList{u"org.freedesktop.appearance"_s});
        QCOMPARE(sorted(settings.value(u"org.freedesktop.appearance"_s).keys()), expectedFdoKeys);
        QVERIFY(!errorCatcher.lastError().isValid());
    }

    void readAllWithGlob()
    {
        auto globals = KSharedConfig::openConfig();
        globals->group(u"Test1"_s).writeEntry("Foo", "Bar");
        globals->group(u"KDECommunity"_s).writeEntry("is", "awesome");
        globals->sync();

        SettingsPortal portal(this, errorCatcher.catcher());

        const auto settings = portal.ReadAll({u"org.kde.kdeglobals.*"_s});
        QVERIFY(!errorCatcher.lastError().isValid());

        QVERIFY(settings.contains(u"org.kde.kdeglobals.Test1"_s));
        QVERIFY(settings.contains(u"org.kde.kdeglobals.KDECommunity"_s));
        QCOMPARE(settings[u"org.kde.kdeglobals.Test1"_s], QVariantMap({{u"Foo"_s, u"Bar"_s}}));
        QCOMPARE(settings[u"org.kde.kdeglobals.KDECommunity"_s], QVariantMap({{u"is"_s, u"awesome"_s}}));
    }

    void readKdeGlobalsSubgroup()
    {
        auto globals = KSharedConfig::openConfig();
        globals->group(u"General"_s).writeEntry("SettingsPortalTest", "value");
        globals->sync();

        SettingsPortal portal(this, errorCatcher.catcher());

        QCOMPARE(portal.Read(u"org.kde.kdeglobals.General"_s, u"SettingsPortalTest"_s).variant().toString(), u"value"_s);
        QVERIFY(!errorCatcher.lastError().isValid());
    }

    void invalidReads_data()
    {
        QTest::addColumn<QString>("group");
        QTest::addColumn<QString>("key");
        QTest::newRow("unknown-property") << u"org.freedesktop.appearance"_s << u"doesnotexist"_s;
        QTest::newRow("unknown-namespace") << u"nonExistentGroup"_s << u"somekey"_s;
        QTest::newRow("empty-namespace") << QString() << u"somekey"_s;
        QTest::newRow("unknown-config-group") << u"org.kde.kdeglobals.MissingGroup"_s << u"somekey"_s;
        QTest::newRow("namespace-prefix-collision") << u"org.kde.kdeglobalsUnexpected.General"_s << u"SettingsPortalTest"_s;
        QTest::newRow("keyboard-subgroup") << u"org.kde.VirtualKeyboard.MissingGroup"_s << u"available"_s;
        QTest::newRow("tablet-subgroup") << u"org.kde.TabletMode.MissingGroup"_s << u"available"_s;
    }

    void invalidReads()
    {
        QFETCH(QString, group);
        QFETCH(QString, key);
        SettingsPortal portal(this, errorCatcher.catcher());
        QVERIFY(!portal.Read(group, key).variant().isValid());
        QCOMPARE(errorCatcher.lastError().type(), QDBusError::UnknownProperty);
    }
};

int main(int argc, char **argv)
{
    // Set configuration paths before QApplication or KConfig can cache them.
    QTemporaryDir configHome;
    if (!configHome.isValid()) {
        return 1;
    }
    const QByteArray configPath = QFile::encodeName(configHome.path());
    qputenv("XDG_CONFIG_HOME", configPath);
    qputenv("XDG_CONFIG_DIRS", configPath);
    QApplication app(argc, argv);
    SettingsPortalTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "settingsportaltest.moc"
