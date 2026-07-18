#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QTimer>

class PendingPortalReply : public QObject {
    Q_OBJECT
public:
    using ReplyPayload = QVariantList;
    using BuildPayload = std::function<ReplyPayload()>;

    PendingPortalReply(QDBusMessage message, QObject* context, BuildPayload callback, int timeoutMs = 0, QObject* parent = nullptr);
    ~PendingPortalReply() override;

    bool complete();
    bool failWithError(const QString& name, const QString& message);

private:
    QDBusMessage m_message;
    QTimer* m_timer = nullptr;
    BuildPayload m_callback;
    bool m_completed = false;
};
