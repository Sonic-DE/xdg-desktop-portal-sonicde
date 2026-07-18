/*
 * SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <QAbstractListModel>
#include <QRect>
#include <QSet>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

class QTemporaryFile;
class QScreen;

class Output
{
public:
    enum OutputType {
        Unknown,
        Laptop,
        Monitor,
        Television,
        Workspace,
        Region,
    };

    Output() = default;
    Output(OutputType outputType,
        const QString& display,
        const QString& uniqueId,
        const QString& name,
        const std::shared_ptr<QTemporaryFile>& temporaryFile)
        : m_outputType(outputType)
        , m_display(display)
        , m_uniqueId(uniqueId)
        , m_name(name)
        , m_temporaryFile(temporaryFile)
    {
    }

    QString name() const
    {
        return m_name;
    }

    [[nodiscard]] QString iconName() const;
    [[nodiscard]] QString description() const;
    [[nodiscard]] bool isSynthetic() const;

    QString display() const
    {
        return m_display;
    }

    QString uniqueId() const
    {
        return m_uniqueId;
    }

    OutputType outputType() const
    {
        return m_outputType;
    }

    int x() const
    {
        return m_nativeGeometry.x();
    }
    int y() const
    {
        return m_nativeGeometry.y();
    }
    int width() const
    {
        return m_nativeGeometry.width();
    }
    int height() const
    {
        return m_nativeGeometry.height();
    }
    int w() const
    {
        return m_nativeGeometry.width();
    }
    int h() const
    {
        return m_nativeGeometry.height();
    }

    void setIsSynthetic(bool s)
    {
        m_synthetic = s;
    }
    void setName(const QString& n)
    {
        m_name = n;
    }
    void setUniqueId(const QString& u)
    {
        m_uniqueId = u;
    }
    void setWidth(int w)
    {
        m_nativeGeometry.setWidth(w);
    }
    void setHeight(int h)
    {
        m_nativeGeometry.setHeight(h);
    }
    void setX(int x)
    {
        m_nativeGeometry.setX(x);
    }
    void setY(int y)
    {
        m_nativeGeometry.setY(y);
    }
    void setOutputType(OutputType t)
    {
        m_outputType = t;
    }
    void setDisplay(const QString& d)
    {
        m_display = d;
    }
    void setGeometry(const QRect& g)
    {
        m_nativeGeometry = g;
    }

    QRect geometry() const
    {
        return m_nativeGeometry;
    }

    /*! Returns the image associated with the output (may be isNull() if it is unknown)*/
    [[nodiscard]] QUrl imageUrl() const
    {
        return m_image;
    }

    /*! Sets a new image. Don't forget to update the model! */
    void setImage(const QUrl &image)
    {
        m_image = image;
    }

private:
    OutputType m_outputType = Unknown;
    QString m_display;
    QString m_uniqueId;
    QString m_name;
    QRect m_nativeGeometry;
    QUrl m_image;
    std::shared_ptr<QTemporaryFile> m_temporaryFile;
    bool m_synthetic = false;
};

class OutputsModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("OutputsModel is passed in through the root properties")
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY hasSelectionChanged)
    Q_PROPERTY(qsizetype outputCount READ outputCount NOTIFY countChanged)
    Q_PROPERTY(qsizetype syntheticCount READ syntheticCount NOTIFY countChanged)
public:
    enum Option {
        None = 0,
        WorkspaceIncluded = 0x1,
        RegionIncluded = 0x4,
        OutputsExcluded = 0x8
    };
    Q_ENUM(Option)
    Q_DECLARE_FLAGS(Options, Option)

    enum Roles {
        ScreenRole = Qt::UserRole,
        NameRole,
        IsSyntheticRole,
        DescriptionRole,
        GeometryRole,
        ImageUrlRole,
    };
    Q_ENUM(Roles)

    OutputsModel(Options o, QObject *parent);
    ~OutputsModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

    const Output &outputAt(int row) const;
    QList<Output> selectedOutputs() const;
    bool hasSelection() const
    {
        return !m_selectedRows.isEmpty();
    }

    Q_INVOKABLE [[nodiscard]] bool geometryIntersects(const QModelIndex &index, const QRect &geometry) const
    {
        if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
            qWarning() << "Invalid index for geometry intersection check:" << index;
            return false;
        }
        if (data(index, IsSyntheticRole).toBool()) {
            return false;
        }
        return data(index, GeometryRole).toRect() == geometry;
    }

    [[nodiscard]] qsizetype outputCount() const;
    [[nodiscard]] qsizetype syntheticCount() const;

public Q_SLOTS:
    void clearSelection();

Q_SIGNALS:
    void hasSelectionChanged();
    void countChanged();

private:
    QList<Output> m_outputs;
    QSet<quint32> m_selectedRows;
    QStringList m_sourceOrder;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(OutputsModel::Options)
