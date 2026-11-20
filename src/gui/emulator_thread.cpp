// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "emulator_thread.hpp"

#include "cpu.hpp"
#include "firmware.hpp"
#include "machine.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <QString>

EmulatorThread::EmulatorThread(QObject *parent) : QThread(parent) {}

EmulatorThread::~EmulatorThread() { stop(); }

bool EmulatorThread::loadFirmware(const QString &path, const QString &dataFlash, QString &error)
{
	if (isRunning()) {
		error = QStringLiteral("the emulator is still running");
		return false;
	}

	auto machine = std::make_unique<Machine>();
	machine->set_uart_echo(false);
	std::string err;
	if (!dataFlash.isEmpty() && !machine->load_data_flash(dataFlash.toStdString(), err)) {
		error = QString::fromStdString(err);
		return false;
	}
	FirmwareInfo info;
	if (!load_firmware(*machine, path.toStdString(), info, err)) {
		error = QString::fromStdString(err);
		return false;
	}
	auto cpu = std::make_unique<Cpu>(*machine);
	cpu->reset(info.entry);

	machine_ = std::move(machine);
	cpu_ = std::move(cpu);
	pressed_at_.clear();
	deferred_keys_.clear();
	last_generation_ = 0;
	stop_.store(false);
	paused_.store(false);
	return true;
}

void EmulatorThread::setKey(const QString &name, bool pressed)
{
	std::lock_guard<std::mutex> lock(queue_mutex_);
	key_queue_.emplace_back(name.toStdString(), pressed);
}

void EmulatorThread::stop()
{
	stop_.store(true);
	if (isRunning()) wait();
}

void EmulatorThread::applyKeys()
{
	static constexpr uint64_t MIN_KEY_HOLD = 10000000ULL; // about 60 ms of emulated time

	std::deque<std::pair<std::string, bool>> pending;
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		pending.swap(key_queue_);
	}
	if (!machine_) return;

	for (const auto &k : pending) {
		std::string err;
		if (k.second) {
			deferred_keys_.erase(
				std::remove_if(deferred_keys_.begin(), deferred_keys_.end(),
					       [&](const auto &d) { return d.first == k.first; }),
				deferred_keys_.end());
			machine_->set_key(k.first, true, err);
			pressed_at_[k.first] = cpu_->cycles();
		} else {
			auto it = pressed_at_.find(k.first);
			if (it != pressed_at_.end() && cpu_->cycles() < it->second + MIN_KEY_HOLD) {
				deferred_keys_.push_back(k);
			} else {
				machine_->set_key(k.first, false, err);
				pressed_at_.erase(k.first);
			}
		}
	}

	std::deque<std::pair<std::string, bool>> keep;
	for (const auto &k : deferred_keys_) {
		auto it = pressed_at_.find(k.first);
		if (it == pressed_at_.end()) continue;
		if (cpu_->cycles() >= it->second + MIN_KEY_HOLD) {
			std::string err;
			machine_->set_key(k.first, false, err);
			pressed_at_.erase(it);
		} else {
			keep.push_back(k);
		}
	}
	deferred_keys_.swap(keep);
}

QImage EmulatorThread::makeFrameImage() const
{
	QImage image(Machine::DISPLAY_WIDTH, Machine::DISPLAY_HEIGHT, QImage::Format_RGB32);
	const auto &fb = machine_->framebuffer();
	if (fb.size() != (size_t)Machine::DISPLAY_WIDTH * Machine::DISPLAY_HEIGHT) {
		image.fill(Qt::black);
		return image;
	}
	for (uint32_t y = 0; y < Machine::DISPLAY_HEIGHT; y++) {
		QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
		const uint16_t *src = fb.data() + y * Machine::DISPLAY_WIDTH;
		for (uint32_t x = 0; x < Machine::DISPLAY_WIDTH; x++) {
			const uint16_t c = src[x];
			const int r = ((c >> 11) & 0x1f) * 255 / 31;
			const int g = ((c >> 5) & 0x3f) * 255 / 63;
			const int b = (c & 0x1f) * 255 / 31;
			line[x] = qRgb(r, g, b);
		}
	}
	return image;
}

void EmulatorThread::run()
{
	if (!machine_ || !cpu_) return;

	emit statusMessage(QStringLiteral("Running"));
	QElapsedTimer frameTimer;
	frameTimer.start();

	while (!stop_.load()) {
		applyKeys();
		if (paused_.load()) {
			msleep(10);
			continue;
		}

		const int batch = 500000;
		bool stop_after = false;
		for (int i = 0; i < batch && !stop_.load(); i++) {
			if ((i & 0x3fff) == 0 && paused_.load()) break;
			try {
				cpu_->step();
				machine_->tick(cpu_->cycles());
			} catch (const CpuFault &f) {
				emit fault(QString::fromStdString(f.what()));
				stop_.store(true);
				stop_after = true;
				break;
			}
		}
		if (stop_after) break;

		const uint64_t gen = machine_->framebuffer_generation();
		if (gen != last_generation_ && frameTimer.elapsed() >= 30) {
			last_generation_ = gen;
			frameTimer.restart();
			emit frameReady(makeFrameImage(), machine_->frame_count());
		}
	}
	emit statusMessage(stop_.load() ? QStringLiteral("Stopped") : QStringLiteral("Finished"));
}
