/*
     SPDX-FileCopyrightText: 2026 SonicDE Project

     SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "outputsmodel.h"
#include "screencast.h"
#include "utils.h"
#include "x11/x11types.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

class QWindow;
class X11Controller;

class ScreenSelectionRequest : public QObject {
    Q_OBJECT
public:
    using WindowOutputTrigger = std::function<void(const QList<X11Types::WindowDescriptor>&)>;

    explicit ScreenSelectionRequest(QObject* parent = nullptr);
    ~ScreenSelectionRequest() override;

    virtual QList<Output> selectedOutputs() const;
    virtual QList<X11Types::WindowDescriptor> selectedWindows() const;
    virtual bool allowRestore() const;
    virtual QRect selectedRegion() const;
    virtual QWindow* windowHandle() const;

public Q_SLOTS:
    virtual void reject();

Q_SIGNALS:
    void finished(DialogResult result);
};

class ScreenSelectionProvider : public QObject {
    Q_OBJECT
public:
    explicit ScreenSelectionProvider(QObject* parent = nullptr);
    ~ScreenSelectionProvider() override;

    virtual ScreenSelectionRequest* createRequest(const QString& appName,
        bool multiple,
        ScreenCastPortal::SourceTypes types,
        X11Controller* controller,
        QObject* parent) = 0;
};

class DialogScreenSelectionProvider final : public ScreenSelectionProvider {
    Q_OBJECT
public:
    using ScreenSelectionProvider::ScreenSelectionProvider;

    ScreenSelectionRequest* createRequest(const QString& appName,
        bool multiple,
        ScreenCastPortal::SourceTypes types,
        X11Controller* controller,
        QObject* parent) override;
};
