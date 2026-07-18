/*
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "x11/pipewirestream.h"

#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVariantMap>

#include <memory>

class ScreencastingStream : public QObject
{
    Q_OBJECT
public:
    enum CursorMode {
        Hidden = 1,
        Embedded = 2,
        Metadata = 4,
    };
    Q_ENUM(CursorMode)

    explicit ScreencastingStream(QObject* parent);
    ~ScreencastingStream() override;

    quint32 nodeid() const;

    QRect geometry() const
    {
        return m_geometry;
    }
    void setGeometry(const QRect& rect);

    QVariantMap metaData() const
    {
        return m_metadata;
    }
    void setMetaData(const QVariantMap& m);

    bool start(PipeWireStream::FrameProvider provider = {});
    void pushFrame(PipeWireStream::Frame frame);
    void closeStream();

Q_SIGNALS:
    void created(quint32 nodeid);
    void failed(const QString& error);
    void closed();

private:
    void configurePipeWireStream();

    QRect m_geometry;
    QVariantMap m_metadata;
    std::unique_ptr<PipeWireStream> m_pipeWireStream;
    PipeWireStream::FrameProvider m_provider;
};
