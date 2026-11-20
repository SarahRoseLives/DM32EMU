// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "system_location.hpp"

#include <QDBusConnection>
#include <QGuiApplication>
#include <QDBusMessage>
#include <QDebug>
#include <QTimer>
#include <QVariantMap>

#include <cmath>

namespace {

constexpr const char *kPortal = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kLocation = "org.freedesktop.portal.Location";

} // namespace

SystemLocation::SystemLocation(QObject *parent) : QObject(parent)
{
	if (qEnvironmentVariableIsSet("DM32EMU_GPS_LAT") && qEnvironmentVariableIsSet("DM32EMU_GPS_LON")) {
		startFallback();
		return;
	}
	if (QGuiApplication::platformName() == QStringLiteral("offscreen") ||
	    qEnvironmentVariableIsSet("DM32EMU_GPS_OFF")) {
		return;
	}
	startPortal();
}

void SystemLocation::startFallback()
{
	Machine::GpsFix fix;
	fix.valid = true;
	fix.latitude = qEnvironmentVariable("DM32EMU_GPS_LAT").toDouble();
	fix.longitude = qEnvironmentVariable("DM32EMU_GPS_LON").toDouble();
	fix.altitude_m = qEnvironmentVariable("DM32EMU_GPS_ALT", "0").toDouble();
	fix.speed_mps = qEnvironmentVariable("DM32EMU_GPS_SPEED", "0").toDouble();
	fix.course_deg = qEnvironmentVariable("DM32EMU_GPS_COURSE", "0").toDouble();
	fix.satellites = (uint8_t)qEnvironmentVariable("DM32EMU_GPS_SATS", "8").toUInt();
	QTimer::singleShot(500, this, [this, fix] { emit fixReady(fix); });
}

void SystemLocation::startPortal()
{
	QDBusConnection bus = QDBusConnection::sessionBus();
	if (!bus.isConnected()) {
		qWarning() << "GPS: session bus unavailable";
		return;
	}

	QVariantMap create;
	create[QStringLiteral("session_handle_token")] = QStringLiteral("dm32emu_gps");
	create[QStringLiteral("accuracy")] = 5u;
	QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kLocation,
							   QStringLiteral("CreateSession"));
	call << create;
	QDBusMessage reply = bus.call(call, QDBus::Block, 5000);
	if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
		qWarning() << "GPS: location portal unavailable:" << reply.errorMessage();
		return;
	}
	session_path_ = reply.arguments().first().value<QDBusObjectPath>().path();
	if (session_path_.isEmpty()) {
		qWarning() << "GPS: location portal returned no session";
		return;
	}

	if (!bus.connect(kPortal, kPortalPath, kLocation, QStringLiteral("LocationUpdated"),
			 this, SLOT(onLocation(QDBusObjectPath, QVariantMap)))) {
		qWarning() << "GPS: cannot watch location updates";
		return;
	}

	QVariantMap start;
	start[QStringLiteral("handle_token")] = QStringLiteral("dm32emu_gps_start");
	QDBusMessage start_call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kLocation,
								 QStringLiteral("Start"));
	start_call << QVariant::fromValue(QDBusObjectPath(session_path_)) << QString() << start;
	reply = bus.call(start_call, QDBus::Block, 5000);
	if (reply.type() == QDBusMessage::ErrorMessage) {
		qWarning() << "GPS: location Start failed:" << reply.errorMessage();
	}
}

void SystemLocation::onLocation(const QDBusObjectPath &session, const QVariantMap &location)
{
	if (!session_path_.isEmpty() && session.path() != session_path_) return;
	if (!location.contains(QStringLiteral("Latitude")) || !location.contains(QStringLiteral("Longitude"))) return;

	Machine::GpsFix fix;
	fix.valid = true;
	fix.latitude = location.value(QStringLiteral("Latitude")).toDouble();
	fix.longitude = location.value(QStringLiteral("Longitude")).toDouble();
	fix.altitude_m = location.value(QStringLiteral("Altitude")).toDouble();
	fix.speed_mps = location.value(QStringLiteral("Speed")).toDouble();
	fix.course_deg = location.value(QStringLiteral("Heading")).toDouble();
	fix.satellites = 8;
	emit fixReady(fix);
}
