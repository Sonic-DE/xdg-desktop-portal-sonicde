#include "pendingportalreply.h"
#include "debug.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QLoggingCategory>

namespace {
constexpr int kDefaultReplyTimeoutMs = 30000;
}

PendingPortalReply::PendingPortalReply(QDBusMessage message, QObject* context, BuildPayload callback, int timeoutMs, QObject* parent)
    : QObject(parent)
    , m_message(std::move(message))
    , m_callback(std::move(callback))
{
    m_message.setDelayedReply(true);
    if (context) {
        setParent(context);
    }
    if (timeoutMs <= 0) {
        timeoutMs = kDefaultReplyTimeoutMs;
    }
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(timeoutMs);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_completed) {
            return;
        }
        qCWarning(XdgDesktopPortalKde) << "PendingPortalReply timed out";
        QDBusMessage error = m_message.createErrorReply(QDBusError::Timeout, QStringLiteral("Reply timed out"));
        if (!QDBusConnection::sessionBus().send(error)) {
            qCWarning(XdgDesktopPortalKde) << "Failed to send timeout reply";
        }
        m_completed = true;
        deleteLater();
    });
    m_timer->start();
}

PendingPortalReply::~PendingPortalReply()
{
    if (!m_completed) {
        qCWarning(XdgDesktopPortalKde) << "PendingPortalReply destroyed without sending a reply";
    }
}

bool PendingPortalReply::complete()
{
    if (m_completed) {
        return false;
    }
    m_completed = true;
    if (m_timer) {
        m_timer->stop();
    }
    ReplyPayload payload;
    if (m_callback) {
        payload = m_callback();
    } else {
        payload = {0, QVariantMap{}};
    }
    QDBusMessage reply = m_message.createReply(payload);
    if (!QDBusConnection::sessionBus().send(reply)) {
        qCWarning(XdgDesktopPortalKde) << "Failed to send pending reply";
        return false;
    }
    deleteLater();
    return true;
}

bool PendingPortalReply::failWithError(const QString& name, const QString& message)
{
    if (m_completed) {
        return false;
    }
    m_completed = true;
    if (m_timer) {
        m_timer->stop();
    }
    QDBusMessage error = m_message.createErrorReply(name, message);
    if (!QDBusConnection::sessionBus().send(error)) {
        qCWarning(XdgDesktopPortalKde) << "Failed to send error reply" << name << message;
        return false;
    }
    deleteLater();
    return true;
}
