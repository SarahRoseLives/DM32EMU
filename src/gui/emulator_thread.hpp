// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "cpu.hpp"
#include "uart_bridge.hpp"
#include "virtual_rf.hpp"

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QMutex>
#include <QThread>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Machine;

Q_DECLARE_METATYPE(CpuSnapshot)

// Runs the CPU in a worker thread. The GUI queues debugger commands and key
// events; memory, registers and the LCD stay on this thread.
class EmulatorThread : public QThread {
	Q_OBJECT
public:
	explicit EmulatorThread(QObject *parent = nullptr);
	~EmulatorThread() override;

	bool loadFirmware(const QString &path, const QString &dataBackup, const QString &codecBackup, QString &error,
			  uint32_t dmrId = 0);
	void setKey(const QString &name, bool pressed);
	void setPaused(bool paused) { paused_.store(paused); }
	bool paused() const { return paused_.load(); }
	void stop();

	void step();
	void stepOver();
	void resumeExecution();
	void pause();
	void requestState();
	void requestMemory(uint32_t addr, uint32_t len);
	void addBreakpoint(uint32_t addr);
	void removeBreakpoint(uint32_t addr);
	void clearBreakpoints();
	void sendUart(const QByteArray &data);
	void setIdleSkip(bool on) { idle_skip_enabled_.store(on); }
	void setVirtualRf(std::shared_ptr<VirtualRfBus> bus, uint8_t id)
	{
		virtual_bus_ = std::move(bus);
		virtual_id_ = id;
	}
	void setVirtualRfEnabled(bool on) { virtual_enabled_.store(on); }

signals:
	void frameReady(const QImage &image, quint64 frames);
	void stateReady(const CpuSnapshot &state);
	void memoryReady(quint32 addr, const QByteArray &data);
	void breakpointHit(quint32 pc);
	void uartTx(const QByteArray &data);
	void uartRx(const QByteArray &data);
	void bridgeReady(const QString &path);
	void statusMessage(const QString &text);
	void fault(const QString &text);

protected:
	void run() override;

private:
	enum class CmdType { Step, StepOver, Run, Pause, State, ReadMemory, AddBreak, RemoveBreak, ClearBreak };
	struct Command {
		CmdType type;
		uint32_t a = 0;
		uint32_t b = 0;
	};

	void processCommands();
	void applyKeys();
	void applyUartInject();
	void flushUart();
	void emitState();
	void updateBreakpoints();
	bool isBreakpoint(uint32_t pc) const;
	bool executeOne();
	QImage makeFrameImage() const;
	void enqueue(Command cmd);

	std::unique_ptr<Machine> machine_;
	std::unique_ptr<Cpu> cpu_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> paused_{false};
	std::atomic<bool> idle_skip_enabled_{true};
	std::atomic<bool> virtual_enabled_{false};
	std::shared_ptr<VirtualRfBus> virtual_bus_;
	uint8_t virtual_id_ = 0;
	bool virtual_applied_ = false;

	std::mutex queue_mutex_;
	std::deque<std::pair<std::string, bool>> key_queue_;
	std::deque<std::pair<std::string, bool>> deferred_keys_;
	std::map<std::string, uint64_t> pressed_at_;

	std::mutex cmd_mutex_;
	std::deque<Command> commands_;
	std::mutex bp_mutex_;
	std::vector<uint32_t> breakpoints_;
	std::vector<uint32_t> local_breakpoints_;
	uint32_t temp_break_ = 0;
	uint32_t skip_break_pc_ = 0xFFFFFFFFu;

	std::mutex inject_mutex_;
	std::deque<uint8_t> uart_inject_;
	QByteArray uart_tx_;
	QByteArray uart_rx_;
	UartBridge bridge_;

	uint64_t last_generation_ = 0;
	uint32_t idle_lo_ = 0, idle_hi_ = 0;
	bool idle_skip_applied_ = true;
};
