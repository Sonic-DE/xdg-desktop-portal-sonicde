/*
     SPDX-FileCopyrightText: 2026 SonicDE Project

     SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "screenselectionprovider.h"

#include "screenchooserdialog.h"

ScreenSelectionRequest::ScreenSelectionRequest(QObject* parent)
    : QObject(parent)
{
}

ScreenSelectionRequest::~ScreenSelectionRequest() = default;

QList<Output> ScreenSelectionRequest::selectedOutputs() const
{
    return {};
}

QList<X11Types::WindowDescriptor> ScreenSelectionRequest::selectedWindows() const
{
    return {};
}

bool ScreenSelectionRequest::allowRestore() const
{
    return false;
}

QRect ScreenSelectionRequest::selectedRegion() const
{
    return {};
}

QWindow* ScreenSelectionRequest::windowHandle() const
{
    return nullptr;
}

void ScreenSelectionRequest::reject()
{
    Q_EMIT finished(DialogResult::Rejected);
}

ScreenSelectionProvider::ScreenSelectionProvider(QObject* parent)
    : QObject(parent)
{
}

ScreenSelectionProvider::~ScreenSelectionProvider() = default;

class DialogScreenSelectionRequest final : public ScreenSelectionRequest {
    Q_OBJECT
public:
    DialogScreenSelectionRequest(const QString& appName,
        bool multiple,
        ScreenCastPortal::SourceTypes types,
        X11Controller* controller,
        QObject* parent)
        : ScreenSelectionRequest(parent)
        , m_controller(controller)
    {
        Q_UNUSED(types);
        m_dialog = std::make_unique<ScreenChooserDialog>(appName, multiple, ScreenCastPortal::Monitor, m_controller);
        m_dialog->setParent(this);
        QObject::connect(m_dialog.get(), &ScreenChooserDialog::finished, this, [this](DialogResult result) {
            Q_EMIT finished(result);
        });
    }

    QList<Output> selectedOutputs() const override
    {
        return m_dialog ? m_dialog->selectedOutputs() : QList<Output>{};
    }

    QList<X11Types::WindowDescriptor> selectedWindows() const override
    {
        return m_dialog ? m_dialog->selectedWindows() : QList<X11Types::WindowDescriptor>{};
    }

    bool allowRestore() const override
    {
        return m_dialog ? m_dialog->allowRestore() : false;
    }

    QRect selectedRegion() const override
    {
        return m_dialog ? m_dialog->selectedRegion() : QRect{};
    }

    QWindow* windowHandle() const override
    {
        return m_dialog ? m_dialog->windowHandle() : nullptr;
    }

public Q_SLOTS:
    void reject() override
    {
        if (m_dialog) {
            m_dialog->reject();
        }
        ScreenSelectionRequest::reject();
    }

private:
    X11Controller* m_controller = nullptr;
    std::unique_ptr<ScreenChooserDialog> m_dialog;
};

ScreenSelectionRequest* DialogScreenSelectionProvider::createRequest(const QString& appName,
    bool multiple,
    ScreenCastPortal::SourceTypes types,
    X11Controller* controller,
    QObject* parent)
{
    return new DialogScreenSelectionRequest(appName, multiple, types, controller, parent);
}

#include "screenselectionprovider.moc"
