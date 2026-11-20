// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "emulator_thread.hpp"

#include "codec_backup.hpp"
#include "firmware.hpp"
#include "machine.hpp"

#include <QElapsedTimer>
#include <QString>

#include <cstdio>

#include <algorithm>

EmulatorThread::EmulatorThread(QObject *parent) : QThread(parent) {}

EmulatorThread::~EmulatorThread() { stop(); }

bool EmulatorThread::loadFirmware(const QString &path, const QString &dataBackup, const QString &codecBackup,
				  QString &error, uint32_t dmrId)
{
	if (isRunning()) {
		error = QStringLiteral("the emulator is still running");
		return false;
	}

	auto machine = std::make_unique<Machine>();
	machine->set_uart_echo(false);
	std::string err;
	if (!dataBackup.isEmpty()) {
		std::vector<uint8_t> data;
		uint32_t offset = 0;
		if (!load_data_backup(dataBackup.toStdString(), data, offset, err) ||
		    !machine->install_data_flash(data, offset, err)) {
			error = QString::fromStdString(err);
			return false;
		}
	}
	if (dmrId != 0 && !machine->set_dmr_id(dmrId, err)) {
		error = QString::fromStdString(err);
		return false;
	}
	FirmwareInfo info;
	if (!load_firmware(*machine, path.toStdString(), info, err)) {
		error = QString::fromStdString(err);
		return false;
	}
	if (!codecBackup.isEmpty()) {
		std::vector<uint8_t> slot;
		CodecBackupInfo ci;
		if (!build_codec_slot(codecBackup.toStdString(), slot, ci, err) ||
		    !machine->install_codec_slot(slot, err)) {
			error = QString::fromStdString(err);
			return false;
		}
	}
	if (iq_channel_) {
		machine->attach_iq_channel(iq_channel_, iq_id_);
		machine->set_iq_enabled(iq_enabled_.load());
		machine->set_dmr_timeslot(dmr_ts_.load());
		iq_applied_ = iq_enabled_.load();
	}
	auto cpu = std::make_unique<Cpu>(*machine);
	cpu->reset(info.entry);
	idle_lo_ = idle_hi_ = 0;
	idle_skip_applied_ = idle_skip_enabled_.load();
	find_function_range(path.toStdString(), "prvIdleTask", idle_lo_, idle_hi_);
	if (idle_skip_applied_ && idle_hi_ > idle_lo_) cpu->set_idle_range(idle_lo_, idle_hi_);

	machine_ = std::move(machine);
	cpu_ = std::move(cpu);
	pressed_at_.clear();
	deferred_keys_.clear();
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		key_queue_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(cmd_mutex_);
		commands_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(bp_mutex_);
		breakpoints_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(inject_mutex_);
		uart_inject_.clear();
	}
	local_breakpoints_.clear();
	breakpoints_.clear();
	temp_break_ = 0;
	skip_break_pc_ = 0xFFFFFFFFu;
	uart_tx_.clear();
	uart_rx_.clear();
	last_generation_ = 0;
	stop_.store(false);
	paused_.store(false);

	machine_->set_uart_tx_sink([this](uint8_t b) {
		uart_tx_.append(static_cast<char>(b));
		bridge_.write_byte(b);
	});
	bridge_.set_rx_callback([this](uint8_t b) { uart_rx_.append(static_cast<char>(b)); });
	bridge_.close();
	std::string pty_path, pty_err;
	if (bridge_.open_pty(pty_path, pty_err)) {
		std::fprintf(stderr, "uart pty: %s\n", pty_path.c_str());
		emit bridgeReady(QString::fromStdString(pty_path));
	} else {
		std::fprintf(stderr, "uart pty: %s\n", pty_err.c_str());
		emit bridgeReady(QString());
	}
	return true;
}

void EmulatorThread::stop()
{
	stop_.store(true);
	if (isRunning()) wait();
	bridge_.close();
}

void EmulatorThread::setGpsFix(const Machine::GpsFix &fix)
{
	std::lock_guard<std::mutex> lock(gps_mutex_);
	gps_fix_ = fix;
	gps_dirty_ = true;
}

void EmulatorThread::setKey(const QString &name, bool pressed)
{
	std::lock_guard<std::mutex> lock(queue_mutex_);
	key_queue_.emplace_back(name.toStdString(), pressed);
}

void EmulatorThread::enqueue(Command cmd)
{
	std::lock_guard<std::mutex> lock(cmd_mutex_);
	commands_.push_back(cmd);
}

void EmulatorThread::step()
{
	paused_.store(true);
	enqueue({CmdType::Step});
}

void EmulatorThread::stepOver()
{
	paused_.store(true);
	enqueue({CmdType::StepOver});
}

void EmulatorThread::resumeExecution()
{
	enqueue({CmdType::Run});
}

void EmulatorThread::pause()
{
	paused_.store(true);
	enqueue({CmdType::Pause});
}

void EmulatorThread::requestState() { enqueue({CmdType::State}); }

void EmulatorThread::requestMemory(uint32_t addr, uint32_t len)
{
	enqueue({CmdType::ReadMemory, addr, len});
}

void EmulatorThread::addBreakpoint(uint32_t addr) { enqueue({CmdType::AddBreak, addr}); }

void EmulatorThread::removeBreakpoint(uint32_t addr) { enqueue({CmdType::RemoveBreak, addr}); }

void EmulatorThread::clearBreakpoints() { enqueue({CmdType::ClearBreak}); }

void EmulatorThread::sendUart(const QByteArray &data)
{
	std::lock_guard<std::mutex> lock(inject_mutex_);
	for (char c : data) uart_inject_.push_back(static_cast<uint8_t>(c));
}

bool EmulatorThread::executeOne()
{
	try {
		cpu_->step();
		return true;
	} catch (const CpuFault &f) {
		emit fault(QString::fromStdString(f.what()));
		stop_.store(true);
		return false;
	}
}

void EmulatorThread::emitState()
{
	if (cpu_) emit stateReady(cpu_->snapshot());
}

void EmulatorThread::updateBreakpoints()
{
	std::lock_guard<std::mutex> lock(bp_mutex_);
	local_breakpoints_ = breakpoints_;
}

bool EmulatorThread::isBreakpoint(uint32_t pc) const
{
	return std::find(local_breakpoints_.begin(), local_breakpoints_.end(), pc) !=
	       local_breakpoints_.end();
}

void EmulatorThread::processCommands()
{
	std::deque<Command> pending;
	{
		std::lock_guard<std::mutex> lock(cmd_mutex_);
		pending.swap(commands_);
	}
	if (pending.empty()) return;

	for (const auto &cmd : pending) {
		switch (cmd.type) {
		case CmdType::Step:
			paused_.store(true);
			if (!executeOne()) return;
			machine_->tick(cpu_->cycles());
			emitState();
			break;
		case CmdType::StepOver:
			if (cpu_->is_call_instruction()) {
				temp_break_ = cpu_->pc() + (uint32_t)cpu_->instruction_size();
				skip_break_pc_ = cpu_->pc();
				paused_.store(false);
			} else {
				paused_.store(true);
				if (!executeOne()) return;
				machine_->tick(cpu_->cycles());
				emitState();
			}
			break;
		case CmdType::Run:
			temp_break_ = 0;
			skip_break_pc_ = cpu_->pc();
			paused_.store(false);
			break;
		case CmdType::Pause:
			paused_.store(true);
			temp_break_ = 0;
			emitState();
			break;
		case CmdType::State:
			emitState();
			break;
		case CmdType::ReadMemory: {
			const uint32_t len = std::min<uint32_t>(cmd.b, 4096);
			QByteArray data;
			data.reserve((int)len);
			for (uint32_t i = 0; i < len; i++) {
				data.append(static_cast<char>(machine_->peek8(cmd.a + i)));
			}
			emit memoryReady(cmd.a, data);
			break;
		}
		case CmdType::AddBreak: {
			std::lock_guard<std::mutex> lock(bp_mutex_);
			if (std::find(breakpoints_.begin(), breakpoints_.end(), cmd.a) == breakpoints_.end()) {
				breakpoints_.push_back(cmd.a);
			}
			break;
		}
		case CmdType::RemoveBreak: {
			std::lock_guard<std::mutex> lock(bp_mutex_);
			breakpoints_.erase(std::remove(breakpoints_.begin(), breakpoints_.end(), cmd.a),
					   breakpoints_.end());
			break;
		}
		case CmdType::ClearBreak: {
			std::lock_guard<std::mutex> lock(bp_mutex_);
			breakpoints_.clear();
			break;
		}
		}
	}
	updateBreakpoints();
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

void EmulatorThread::applyUartInject()
{
	std::deque<uint8_t> pending;
	{
		std::lock_guard<std::mutex> lock(inject_mutex_);
		pending.swap(uart_inject_);
	}
	if (!machine_) return;
	for (uint8_t b : pending) machine_->feed_uart(0, b);
}

void EmulatorThread::flushUart()
{
	if (!uart_tx_.isEmpty()) {
		emit uartTx(uart_tx_);
		uart_tx_.clear();
	}
	if (!uart_rx_.isEmpty()) {
		emit uartRx(uart_rx_);
		uart_rx_.clear();
	}
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
	emitState();
	QElapsedTimer frameTimer;
	QElapsedTimer stateTimer;
	frameTimer.start();
	stateTimer.start();
	auto emitFrameIfChanged = [&](bool force) {
		const uint64_t gen = machine_->framebuffer_generation();
		if (gen != last_generation_ && (force || frameTimer.elapsed() >= 30)) {
			last_generation_ = gen;
			frameTimer.restart();
			emit frameReady(makeFrameImage(), machine_->frame_count());
		}
	};

	while (!stop_.load()) {
		if (iq_channel_ && iq_enabled_.load() != iq_applied_) {
			iq_applied_ = iq_enabled_.load();
			machine_->set_iq_enabled(iq_applied_);
		}
		const bool want_idle = idle_skip_enabled_.load();
		if (want_idle != idle_skip_applied_) {
			idle_skip_applied_ = want_idle;
			cpu_->set_idle_range(want_idle ? idle_lo_ : 0, want_idle ? idle_hi_ : 0);
		}
		{
			std::lock_guard<std::mutex> lock(gps_mutex_);
			if (gps_dirty_ && machine_) {
				machine_->set_gps_fix(gps_fix_);
				gps_dirty_ = false;
			}
		}
		processCommands();
		applyKeys();
		applyUartInject();
		if (bridge_.active()) bridge_.poll(*machine_);
		flushUart();
		emitFrameIfChanged(paused_.load());

		if (paused_.load()) {
			msleep(5);
			continue;
		}

		updateBreakpoints();
		const int batch = 500000;
		uint64_t last_io = cpu_->cycles();
		bool stop_after = false;
		for (int i = 0; i < batch && !stop_.load(); i++) {
			if ((i & 0x3fff) == 0 && paused_.load()) break;
			if (temp_break_ != 0 && cpu_->pc() == temp_break_) {
				temp_break_ = 0;
				paused_.store(true);
				emitState();
				break;
			}
			const uint32_t pc = cpu_->pc();
			if (isBreakpoint(pc) && pc != skip_break_pc_) {
				temp_break_ = 0;
				skip_break_pc_ = 0xFFFFFFFFu;
				paused_.store(true);
				emit breakpointHit(pc);
				emitState();
				break;
			}
			if (pc == skip_break_pc_) skip_break_pc_ = 0xFFFFFFFFu;
			if (!executeOne()) {
				stop_after = true;
				break;
			}
			if ((i & 0x3FF) == 0) machine_->tick(cpu_->cycles());
			if (cpu_->cycles() - last_io >= 16384) {
				last_io = cpu_->cycles();
				applyKeys();
				applyUartInject();
				if (bridge_.active()) bridge_.poll(*machine_);
				flushUart();
			}
		}
		flushUart();
		if (stop_after) break;
		emitFrameIfChanged(paused_.load());

		if (!paused_.load() && stateTimer.elapsed() >= 250) {
			stateTimer.restart();
			emitState();
		}
	}
	flushUart();
	emit statusMessage(stop_.load() ? QStringLiteral("Stopped") : QStringLiteral("Finished"));
}
