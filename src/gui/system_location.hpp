// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "machine.hpp"

#include <QObject>
#include <QDBusObjectPath>

class SystemLocation : public QObject {
	Q_OBJECT
public:
	explicit SystemLocation(QObject *parent = nullptr);

signals:
	void fixReady(const Machine::GpsFix &fix);

private slots:
	void onLocation(const QDBusObjectPath &session, const QVariantMap &location);

private:
	void startPortal();
	void startFallback();

	QString session_path_;
};
