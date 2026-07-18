/*
 * SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "outputsmodel.h"

#include <KLocalizedString>
#include <QGuiApplication>
#include <QIcon>
#include <QScreen>

#include <ranges>

#include "debug.h"

using namespace Qt::StringLiterals;

OutputsModel::OutputsModel(Options o, QObject* parent)
    : QAbstractListModel(parent)
{
    if (o & RegionIncluded) {
        Output region;
        region.setOutputType(Output::Region);
        region.setDisplay(i18n("Share region"));
        region.setUniqueId(u"Region"_s);
        region.setName(u"Region"_s);
        region.setIsSynthetic(true);
        m_outputs << region;
    }

    if (o & OutputsExcluded) {
        return;
    }

    const auto screens = qGuiApp->screens();

    if (screens.count() > 1 && (o & WorkspaceIncluded)) {
        Output workspace;
        workspace.setOutputType(Output::Workspace);
        workspace.setDisplay(i18n("Share full Workspace"));
        workspace.setUniqueId(u"Workspace"_s);
        workspace.setName(u"Workspace"_s);
        workspace.setIsSynthetic(true);
        m_outputs.prepend(workspace);
    }

    for (const auto &screen : screens) {
        Output::OutputType type = Output::Unknown;

        static const auto embedded = {
            QLatin1String("LVDS"),
            QLatin1String("IDP"),
            QLatin1String("EDP"),
            QLatin1String("LCD"),
        };

        if (std::ranges::any_of(embedded, [screen](const QString &prefix) {
                return screen->name().startsWith(prefix, Qt::CaseInsensitive);
            })) {
            type = Output::OutputType::Laptop;
        } else if (screen->name().contains(QLatin1String("TV"))) {
            type = Output::OutputType::Television;
        } else {
            type = Output::OutputType::Monitor;
        }

        QString displayText;
        if (type == Output::OutputType::Laptop) {
            displayText = i18n("Laptop screen");
        } else {
            QStringList parts;
            if (!screen->manufacturer().isEmpty()) {
                parts.append(screen->manufacturer());

                if (!screen->model().isEmpty()) {
                    QString part = screen->model();
                    if (!screen->serialNumber().isEmpty()) {
                        part += QLatin1Char('/') + screen->serialNumber();
                    }
                    parts.append(part);
                } else if (!screen->serialNumber().isEmpty()) {
                    parts.append(screen->serialNumber());
                }

                parts.append(QLatin1Char('(') + screen->name() + QLatin1Char(')'));
            } else {
                parts.append(screen->name());
            }

            displayText = parts.join(QLatin1Char(' '));
        }

        const QString uniqueId = screen->name();

        Output out;
        out.setOutputType(type);
        out.setDisplay(displayText);
        out.setUniqueId(uniqueId);
        out.setName(screen->name());
        out.setGeometry(screen->geometry());
        m_outputs << out;
        m_sourceOrder << uniqueId;
    }

    std::ranges::stable_partition(m_outputs, [](const Output &o) {
        return !o.isSynthetic();
    });
}

OutputsModel::~OutputsModel() = default;

int OutputsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_outputs.count();
}

QHash<int, QByteArray> OutputsModel::roleNames() const
{
    return QHash<int, QByteArray>{
        {Qt::DisplayRole, "display"},
        {Qt::DecorationRole, "decoration"},
        {Qt::CheckStateRole, "checked"},
        {ScreenRole, "screen"},
        {NameRole, "name"},
        {IsSyntheticRole, "isSynthetic"},
        {DescriptionRole, "description"},
        {GeometryRole, "geometry"},
        {ImageUrlRole, "imageUrl"},
    };
}

QVariant OutputsModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }

    const auto &output = m_outputs[index.row()];
    switch (role) {
    case NameRole:
        return output.name();
    case IsSyntheticRole:
        return output.isSynthetic();
    case DescriptionRole:
        return output.description();
    case GeometryRole:
        return output.geometry();
    case ImageUrlRole:
        return output.imageUrl();
    case Qt::DecorationRole:
        return QIcon::fromTheme(output.iconName());
    case Qt::DisplayRole:
        return output.display();
    case Qt::CheckStateRole:
        return m_selectedRows.contains(index.row()) ? Qt::Checked : Qt::Unchecked;
    case ScreenRole:
        return {};
    }
    return {};
}

bool OutputsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid) || role != Qt::CheckStateRole) {
        return false;
    }

    if (index.data(Qt::CheckStateRole) == value) {
        return true;
    }

    if (value == Qt::Checked) {
        m_selectedRows.insert(index.row());
    } else {
        m_selectedRows.remove(index.row());
    }
    Q_EMIT dataChanged(index, index, {role});
    if (m_selectedRows.count() <= 1) {
        Q_EMIT hasSelectionChanged();
    }
    return true;
}

const Output &OutputsModel::outputAt(int row) const
{
    return m_outputs[row];
}

void OutputsModel::clearSelection()
{
    if (m_selectedRows.isEmpty())
        return;

    auto selected = m_selectedRows;
    m_selectedRows.clear();
    for (int i = 0, c = rowCount({}); i < c; ++i) {
        if (selected.contains(i)) {
            const auto idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx, {Qt::CheckStateRole});
        }
    }
    Q_EMIT hasSelectionChanged();
}

QList<Output> OutputsModel::selectedOutputs() const
{
    QList<Output> ret;
    ret.reserve(m_selectedRows.count());
    for (auto x : std::as_const(m_selectedRows)) {
        ret << m_outputs[x];
    }
    return ret;
}

qsizetype OutputsModel::outputCount() const
{
    return std::ranges::count_if(m_outputs, [](const auto& output) {
        return !output.isSynthetic();
    });
}

qsizetype OutputsModel::syntheticCount() const
{
    return std::ranges::count_if(m_outputs, [](const Output& output) {
        return output.isSynthetic();
    });
}

QString Output::iconName() const
{
    switch (m_outputType) {
    case Laptop:
        return QStringLiteral("computer-laptop-symbolic");
    case Television:
        return QStringLiteral("video-television-symbolic");
    case Region:
        return QStringLiteral("transform-crop-symbolic");
    case Workspace:
        return QStringLiteral("preferences-desktop-display-randr-symbolic");
    case Monitor:
        return QStringLiteral("monitor-symbolic");
    case Unknown:
        break;
    }
    return QStringLiteral("video-display-symbolic");
}

QString Output::description() const
{
    switch (m_outputType) {
    case Workspace:
        return i18nc("@info", "Share the entire workspace across all screens");
    case Region:
        return i18nc("@info", "Crops a specific area of your screens");
    case Laptop:
    case Monitor:
    case Television:
    case Unknown:
        // Unused
        break;
    }
    return {};
}

bool Output::isSynthetic() const
{
    switch (m_outputType) {
    case Output::Workspace:
    case Output::Region:
        return m_synthetic;
    case Output::Laptop:
    case Output::Monitor:
    case Output::Television:
    case Output::Unknown:
        break;
    }
    return false;
}

#include "moc_outputsmodel.cpp"
