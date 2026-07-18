#include "../src/x11/x11clipboard.h"

#include <QTest>

using namespace X11ClipboardProtocol;
using namespace Qt::StringLiterals;

class X11ClipboardProtocolTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void canonicalMimeAliases();
    void targetMapping();
    void advertisedTargetsContainProtocolTargets();
    void multiplePairsRejectOddCount();
    void multiplePairsNullUnsupportedTargets();
    void transferModeThreshold();
    void incrChunksAdvanceAndTerminate();
    void receiveEmptyChunkCompletesTransfer();
};

static Atoms testAtoms()
{
    Atoms atoms;
    atoms.targets = 1;
    atoms.timestamp = 2;
    atoms.multiple = 3;
    atoms.incr = 4;
    atoms.utf8String = 5;
    atoms.text = 6;
    atoms.textPlain = 7;
    atoms.textPlainUtf8 = 8;
    atoms.applicationOctetStream = 9;
    atoms.atom = XCB_ATOM_ATOM;
    return atoms;
}

void X11ClipboardProtocolTest::canonicalMimeAliases()
{
    QCOMPARE(canonicalMimeType(u"text/plain"_s), u"text/plain;charset=utf-8"_s);
    QCOMPARE(canonicalMimeType(u"UTF8_STRING"_s), u"text/plain;charset=utf-8"_s);
    QCOMPARE(canonicalMimeType({}), u"application/octet-stream"_s);

    const QStringList aliases = mimeAliases(u"text/plain"_s);
    QVERIFY(aliases.contains(u"text/plain;charset=utf-8"_s));
    QVERIFY(aliases.contains(u"UTF8_STRING"_s));
    QVERIFY(aliases.contains(u"STRING"_s));
}

void X11ClipboardProtocolTest::targetMapping()
{
    const Atoms atoms = testAtoms();
    QCOMPARE(targetsForMime(atoms, u"text/plain"_s).first(), atoms.textPlainUtf8);
    QCOMPARE(mimeForTarget(atoms, atoms.utf8String), u"text/plain;charset=utf-8"_s);
    QCOMPARE(mimeForTarget(atoms, atoms.applicationOctetStream), u"application/octet-stream"_s);
    QVERIFY(mimeForTarget(atoms, 999).isEmpty());
}

void X11ClipboardProtocolTest::advertisedTargetsContainProtocolTargets()
{
    const QVector<xcb_atom_t> targets = advertisedTargets(testAtoms(), u"text/plain"_s);
    QVERIFY(targets.contains(testAtoms().targets));
    QVERIFY(targets.contains(testAtoms().timestamp));
    QVERIFY(targets.contains(testAtoms().multiple));
    QVERIFY(targets.contains(testAtoms().textPlainUtf8));
}

void X11ClipboardProtocolTest::multiplePairsRejectOddCount()
{
    QVERIFY(!rewriteMultiplePairs({1, 2, 3}, QSet<xcb_atom_t>{1, 3}).has_value());
}

void X11ClipboardProtocolTest::multiplePairsNullUnsupportedTargets()
{
    const std::optional<QVector<xcb_atom_t>> pairs = rewriteMultiplePairs({1, 20, 2, 21}, QSet<xcb_atom_t>{1});
    QVERIFY(pairs.has_value());
    QCOMPARE((*pairs)[0], xcb_atom_t(1));
    QCOMPARE((*pairs)[1], xcb_atom_t(20));
    QCOMPARE((*pairs)[2], xcb_atom_t(2));
    QCOMPARE((*pairs)[3], XCB_ATOM_NONE);
}

void X11ClipboardProtocolTest::transferModeThreshold()
{
    QCOMPARE(chooseTransferMode(10, 100), TransferMode::Normal);
    QCOMPARE(chooseTransferMode(101, 100), TransferMode::Incr);
}

void X11ClipboardProtocolTest::incrChunksAdvanceAndTerminate()
{
    SendTransfer transfer;
    transfer.data = QByteArray("abcdef");
    transfer.chunkSize = 2;
    QCOMPARE(nextIncrChunk(transfer), QByteArray("ab"));
    QCOMPARE(nextIncrChunk(transfer), QByteArray("cd"));
    QCOMPARE(nextIncrChunk(transfer), QByteArray("ef"));
    QVERIFY(nextIncrChunk(transfer).isEmpty());
}

void X11ClipboardProtocolTest::receiveEmptyChunkCompletesTransfer()
{
    ReceiveTransfer transfer;
    QVERIFY(appendReceivedChunk(transfer, QByteArray("abc")));
    QCOMPARE(transfer.bytesReceived, qsizetype(3));
    QVERIFY(appendReceivedChunk(transfer, QByteArray()));
    QVERIFY(transfer.done);
}

QTEST_GUILESS_MAIN(X11ClipboardProtocolTest)

#include "x11clipboardprotocoltest.moc"
