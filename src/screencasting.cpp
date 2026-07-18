/*
    SPDX-FileCopyrightText: 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "screencasting.h"

#include <QSize>

ScreencastingStream::ScreencastingStream(QObject* parent)
    : QObject(parent)
    , m_pipeWireStream(std::make_unique<PipeWireStream>())
{
    connect(m_pipeWireStream.get(), &PipeWireStream::created, this, &ScreencastingStream::created);
    connect(m_pipeWireStream.get(), &PipeWireStream::failed, this, &ScreencastingStream::failed);
    connect(m_pipeWireStream.get(), &PipeWireStream::closed, this, &ScreencastingStream::closed);
}

ScreencastingStream::~ScreencastingStream()
{
    closeStream();
}

quint32 ScreencastingStream::nodeid() const
{
    return m_pipeWireStream ? m_pipeWireStream->nodeId() : 0;
}

void ScreencastingStream::setGeometry(const QRect& rect)
{
    m_geometry = rect;
    configurePipeWireStream();
}

void ScreencastingStream::setMetaData(const QVariantMap& m)
{
    m_metadata = m;
    if (m_pipeWireStream) {
        m_pipeWireStream->setMetadata(m_metadata);
    }
}

bool ScreencastingStream::start(PipeWireStream::FrameProvider provider)
{
    m_provider = std::move(provider);
    configurePipeWireStream();

    if (!m_pipeWireStream) {
        return false;
    }

    const bool started = m_pipeWireStream->start(m_provider);
    if (!started) {
        return false;
    }
    return m_pipeWireStream->waitForReady();
}

void ScreencastingStream::pushFrame(PipeWireStream::Frame frame)
{
    if (!m_pipeWireStream) {
        return;
    }
    m_pipeWireStream->pushFrame(std::move(frame));
}

void ScreencastingStream::closeStream()
{
    if (m_pipeWireStream) {
        m_pipeWireStream->close();
    }
}

void ScreencastingStream::configurePipeWireStream()
{
    if (!m_pipeWireStream) {
        return;
    }
    const QSize size = m_geometry.isValid() ? m_geometry.size() : QSize(1, 1);
    m_pipeWireStream->configure(size, PipeWireStream::PixelFormat::RGBx, 30, false);
    m_pipeWireStream->setMetadata(m_metadata);
}

#include "moc_screencasting.cpp"
