/*
 * SPDX-FileCopyrightText: 2018 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018 Jan Grulich <jgrulich@redhat.com>
 */

#include "screenchooserdialog.h"
#include "utils.h"
#include "x11/x11controller.h"

#include "region-select/SelectionEditor.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

using namespace Qt::StringLiterals;

class WindowModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY hasSelectionChanged)
public:
    explicit WindowModel(QObject* parent)
        : QAbstractListModel(parent)
    {
    }

    void setWindows(const QList<X11Types::WindowDescriptor>& windows)
    {
        beginResetModel();
        m_windows = windows;
        m_selectedRows.clear();
        endResetModel();
        Q_EMIT hasSelectionChanged();
    }

    QList<X11Types::WindowDescriptor> selectedWindows() const
    {
        QList<X11Types::WindowDescriptor> out;
        for (auto r : std::as_const(m_selectedRows)) {
            out.push_back(m_windows.value(r));
        }
        return out;
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : m_windows.count();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
            return {};
        }
        const auto& w = m_windows[index.row()];
        switch (role) {
        case Qt::DisplayRole:
            return w.title;
        case Qt::DecorationRole:
            return w.appId;
        case GeometryRole:
            return w.nativeGeometry;
        case AppIdRole:
            return w.appId;
        case WindowXIdRole:
            return w.xid;
        case WindowMappedRole:
            return w.mapped;
        }
        return {};
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override
    {
        if (!checkIndex(index, CheckIndexOption::IndexIsValid) || role != Qt::CheckStateRole) {
            return false;
        }
        if (value == Qt::Checked) {
            m_selectedRows.insert(index.row());
        } else {
            m_selectedRows.remove(index.row());
        }
        Q_EMIT dataChanged(index, index, {role});
        Q_EMIT hasSelectionChanged();
        return true;
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return QHash<int, QByteArray>{
            {Qt::DisplayRole, "display"},
            {Qt::DecorationRole, "decoration"},
            {Qt::CheckStateRole, "checked"},
            {GeometryRole, "geometry"},
            {AppIdRole, "appId"},
            {WindowXIdRole, "xid"},
            {WindowMappedRole, "mapped"},
        };
    }

    bool hasSelection() const
    {
        return !m_selectedRows.isEmpty();
    }

    Q_INVOKABLE [[nodiscard]] bool geometryIntersects(const QModelIndex &index, const QRect &geometry) const
    {
        if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
            qWarning() << "Invalid index for window geometry intersection:" << index;
            return false;
        }
        return data(index, GeometryRole).toRect().intersects(geometry);
    }

    enum Roles {
        AppIdRole = Qt::UserRole + 1,
        WindowXIdRole,
        GeometryRole,
        WindowMappedRole,
    };

Q_SIGNALS:
    void hasSelectionChanged();

private:
    QList<X11Types::WindowDescriptor> m_windows;
    QSet<quint32> m_selectedRows;
};

ScreenChooserDialog::ScreenChooserDialog(const QString& appName, bool multiple, ScreenCastPortal::SourceTypes types, X11Controller* controller)
    : QuickDialog()
{
    Q_UNUSED(types)
    QVariantMap props = {
        {u"title"_s, i18nc("@title:window %1 is an application name (e.g. Falkon)", "Share screen with %1", Utils::applicationName(appName))},
        {u"multiple"_s, multiple},
    };

    OutputsModel::Options options = OutputsModel::None;
    if (types & ScreenCastPortal::Monitor) {
        options |= OutputsModel::WorkspaceIncluded | OutputsModel::RegionIncluded;
    } else {
        options |= OutputsModel::OutputsExcluded;
    }
    auto outputsModel = new OutputsModel(options, this);
    props.insert(u"outputsModel"_s, QVariant::fromValue<QObject*>(outputsModel));
    connect(this, &ScreenChooserDialog::clearSelection, outputsModel, &OutputsModel::clearSelection);

    auto windowsModel = new WindowModel(this);
    windowsModel->setWindows(controller ? controller->windows() : QList<X11Types::WindowDescriptor>{});
    props.insert(u"windowsModel"_s, QVariant::fromValue<QObject*>(windowsModel));
    connect(this, &ScreenChooserDialog::clearSelection, windowsModel, [windowsModel]() {
        windowsModel->setWindows({});
    });

    props.insert(u"mainText"_s, i18nc("@info", "Choose what to share with %1", Utils::applicationName(appName)));

    create(QStringLiteral("ScreenChooserDialog"), props);
    connect(m_theDialog, SIGNAL(clearSelection()), this, SIGNAL(clearSelection()));

    connect(this, &QuickDialog::rejected, this, [this] {
        Q_EMIT finished(DialogResult::Rejected);
    });
    connect(this, &QuickDialog::accepted, this, [this] {
        Q_EMIT finished(DialogResult::Accepted);
    });
}

ScreenChooserDialog::~ScreenChooserDialog() = default;

QList<Output> ScreenChooserDialog::selectedOutputs() const
{
    OutputsModel *model = dynamic_cast<OutputsModel *>(m_theDialog->property("outputsModel").value<QObject *>());
    if (!model) {
        return {};
    }
    return model->selectedOutputs();
}

QList<X11Types::WindowDescriptor> ScreenChooserDialog::selectedWindows() const
{
    WindowModel* model = dynamic_cast<WindowModel*>(m_theDialog->property("windowsModel").value<QObject*>());
    if (!model) {
        return {};
    }
    return model->selectedWindows();
}

QRect ScreenChooserDialog::selectedRegion() const
{
    return m_region;
}

void ScreenChooserDialog::setRegion(const QRect region)
{
    m_region = region;
}

bool ScreenChooserDialog::allowRestore() const
{
    return m_theDialog->property("allowRestore").toBool();
}

void ScreenChooserDialog::accept()
{
    if (std::ranges::contains(selectedOutputs(), Output::OutputType::Region, &Output::outputType)) {
        auto selectionEditor = new SelectionEditor(this);
        connect(selectionEditor, &SelectionEditor::finished, this, [this, selectionEditor](DialogResult result) {
            selectionEditor->deleteLater();
            if (result == DialogResult::Accepted) {
                setRegion(selectionEditor->rect());
                QuickDialog::accept();
            } else {
                QTimer::singleShot(0, m_theDialog, SLOT(present()));
            }
        });
        return;
    }
    QuickDialog::accept();
}

#include "screenchooserdialog.moc"

#include "moc_screenchooserdialog.cpp"
