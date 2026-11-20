// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#pragma once

#include <QImage>
#include <QMutex>
#include <QThread>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>

class Machine;
class Cpu;

// Runs the CPU in a worker thread and hands the latest LCD frame to the GUI.
// All Machine access stays on this thread; the GUI only queues key events.
class EmulatorThread : public QThread {
	Q_OBJECT
public:
	explicit EmulatorThread(QObject *parent = nullptr);
	~EmulatorThread() override;

	bool loadFirmware(const QString &path, const QString &dataFlash, QString &error);
	void setKey(const QString &name, bool pressed);
	void setPaused(bool paused) { paused_.store(paused); }
	bool paused() const { return paused_.load(); }
	void stop();

signals:
	void frameReady(const QImage &image, quint64 frames);
	void statusMessage(const QString &text);
	void fault(const QString &text);

protected:
	void run() override;

private:
	void applyKeys();
	QImage makeFrameImage() const;

	std::unique_ptr<Machine> machine_;
	std::unique_ptr<Cpu> cpu_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> paused_{false};
	std::mutex queue_mutex_;
	std::deque<std::pair<std::string, bool>> key_queue_;
	std::deque<std::pair<std::string, bool>> deferred_keys_;
	std::map<std::string, uint64_t> pressed_at_;
	uint64_t last_generation_ = 0;
};
