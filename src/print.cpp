// SPDX-License-Identifier: LGPL-2.0-or-later

#include "print.h"
#include "dbushelpers.h"
#include "print_debug.h"

#include <QDBusMessage>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPrintDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

PrintPortal::PrintPortal(QObject *parent)
    : QDBusAbstractAdaptor(parent)
{
}

PrintPortal::~PrintPortal() = default;

static QString boundedProcessError(const QString& prefix, const QByteArray& data)
{
    QString message = QString::fromLocal8Bit(data.left(1024)).trimmed();
    if (message.isEmpty()) {
        return prefix;
    }
    return prefix + QStringLiteral(": ") + message;
}

PrintPortal::PrintCommand PrintPortal::commandForPdfPrint(const QString& printerName, const QString& title, const QString& filePath, const QVariantMap& options)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return {{}, {}, QStringLiteral("print file does not exist")};
    }

    const QString lp = QStandardPaths::findExecutable(QStringLiteral("lp"));
    if (!lp.isEmpty()) {
        QStringList args;
        if (!printerName.isEmpty()) {
            args << QStringLiteral("-d") << printerName;
        }
        if (!title.isEmpty()) {
            args << QStringLiteral("-t") << title;
        }
        if (options.value(QStringLiteral("modal"), false).toBool()) {
            args << QStringLiteral("-o") << QStringLiteral("job-hold-until=no-hold");
        }
        args << filePath;
        return {lp, args, {}};
    }

    const QString lpr = QStandardPaths::findExecutable(QStringLiteral("lpr"));
    if (!lpr.isEmpty()) {
        QStringList args;
        if (!printerName.isEmpty()) {
            args << QStringLiteral("-P") << printerName;
        }
        if (!title.isEmpty()) {
            args << QStringLiteral("-J") << title;
        }
        args << filePath;
        return {lpr, args, {}};
    }

    return {{}, {}, QStringLiteral("neither lp nor lpr is available")};
}

uint PrintPortal::Print(const QDBusObjectPath &handle,
                        const QString &app_id,
                        const QString &parent_window,
                        const QString &title,
                        const QDBusUnixFileDescriptor &fd,
                        const QVariantMap &options,
                        QVariantMap &results)
{
    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(parent_window)

    results.clear();
    if (!fd.isValid()) {
        results.insert(QStringLiteral("error"), QStringLiteral("invalid PDF file descriptor"));
        return PortalResponse::OtherError;
    }

    QTemporaryFile pdf(QDir::tempPath() + QStringLiteral("/xdg-desktop-portal-print-XXXXXX.pdf"));
    pdf.setAutoRemove(true);
    if (!pdf.open()) {
        results.insert(QStringLiteral("error"), QStringLiteral("failed to create temporary spool file"));
        return PortalResponse::OtherError;
    }

    QFile input;
    if (!input.open(fd.fileDescriptor(), QIODevice::ReadOnly)) {
        results.insert(QStringLiteral("error"), QStringLiteral("failed to read supplied PDF descriptor"));
        return PortalResponse::OtherError;
    }
    constexpr qint64 maxPdfBytes = 1024LL * 1024LL * 1024LL;
    qint64 copied = 0;
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFile::NoError) {
            results.insert(QStringLiteral("error"), QStringLiteral("failed while reading supplied PDF"));
            return PortalResponse::OtherError;
        }
        copied += chunk.size();
        if (copied > maxPdfBytes) {
            results.insert(QStringLiteral("error"), QStringLiteral("supplied PDF is too large to spool"));
            return PortalResponse::OtherError;
        }
        if (pdf.write(chunk) != chunk.size()) {
            results.insert(QStringLiteral("error"), QStringLiteral("failed to write temporary spool file"));
            return PortalResponse::OtherError;
        }
    }
    pdf.flush();

    const QString printerName = options.value(QStringLiteral("printer")).toString();
    const PrintCommand command = commandForPdfPrint(printerName, title, pdf.fileName(), options);
    if (!command.error.isEmpty()) {
        results.insert(QStringLiteral("error"), command.error.left(1024));
        return PortalResponse::OtherError;
    }

    QProcess process;
    process.start(command.program, command.arguments);
    if (!process.waitForStarted(5000)) {
        results.insert(QStringLiteral("error"), QStringLiteral("failed to start print command"));
        return PortalResponse::OtherError;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(30000)) {
        process.kill();
        results.insert(QStringLiteral("error"), QStringLiteral("print command timed out"));
        return PortalResponse::OtherError;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        results.insert(QStringLiteral("error"), boundedProcessError(QStringLiteral("print command failed"), process.readAllStandardError()).left(1024));
        return PortalResponse::OtherError;
    }

    return PortalResponse::Success;
}

void PrintPortal::PreparePrint(const QDBusObjectPath& handle,
    const QString& app_id,
    const QString& parent_window,
    const QString& title,
    const QVariantMap& settings,
    const QVariantMap& page_setup,
    const QVariantMap& options,
    const QDBusMessage& message,
    uint& replyResponse,
    QVariantMap& replyResults)
{
    Q_UNUSED(handle)
    Q_UNUSED(app_id)
    Q_UNUSED(parent_window)
    Q_UNUSED(page_setup)

    auto* printer = new QPrinter(QPrinter::HighResolution);
    printer->setDocName(title);
    const QString requestedPrinter = settings.value(QStringLiteral("printer")).toString();
    if (!requestedPrinter.isEmpty()) {
        const QPrinterInfo info = QPrinterInfo::printerInfo(requestedPrinter);
        if (!info.isNull()) {
            printer->setPrinterName(requestedPrinter);
        }
    }

    auto* dialog = new QPrintDialog(printer);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title.isEmpty() ? tr("Print") : title);
    dialog->setModal(options.value(QStringLiteral("modal"), true).toBool());

    message.setDelayedReply(true);
    connect(dialog, &QDialog::finished, this, [dialog, printer, message](int result) {
        QVariantMap replyResults;
        uint replyResponse = PortalResponse::Cancelled;
        if (result == QDialog::Accepted) {
            QVariantMap preparedSettings;
            preparedSettings.insert(QStringLiteral("printer"), printer->printerName());
            preparedSettings.insert(QStringLiteral("orientation"), static_cast<uint>(printer->pageLayout().orientation()));
            preparedSettings.insert(QStringLiteral("color-mode"), static_cast<uint>(printer->colorMode()));
            replyResults.insert(QStringLiteral("settings"), preparedSettings);
            replyResponse = PortalResponse::Success;
        }
        QDBusConnection::sessionBus().send(message.createReply({replyResponse, replyResults}));
        delete printer;
        dialog->deleteLater();
    });
    replyResponse = PortalResponse::Success;
    replyResults.clear();
    dialog->open();
}

#include "moc_print.cpp"
