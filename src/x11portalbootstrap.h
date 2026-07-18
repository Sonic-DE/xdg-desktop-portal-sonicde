#pragma once

#include <QObject>
#include <QString>

class X11Controller;
class X11Worker;
class QThread;
class XEisMounter;

class PortalBootstrap : public QObject {
    Q_OBJECT
public:
    explicit PortalBootstrap(QObject* parent = nullptr);
    ~PortalBootstrap() override;

    X11Controller* controller() const
    {
        return m_controller;
    }
    bool waitForReady(int timeoutMs, QString* diagnostic = nullptr);

private:
    QThread* m_thread = nullptr;
    X11Worker* m_worker = nullptr;
    X11Controller* m_controller = nullptr;
    XEisMounter* m_eisMounter = nullptr;
};
