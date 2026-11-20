// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

struct VirtualRfBurst {
	uint8_t source = 0;
	uint8_t kind = 0;
	uint8_t voice_index = 0;
	uint8_t cc = 0;
	uint8_t ts = 1;
	uint8_t len = 0;
	uint8_t payload[27]{};
};

// A shared RF medium for two or more emulated radios. Each radio sees every
// burst except the ones it sent.
class VirtualRfBus {
public:
	void add_receiver(uint8_t id);
	void remove_receiver(uint8_t id);
	void send(const VirtualRfBurst &burst);
	bool receive(uint8_t receiver, VirtualRfBurst &burst);
	void clear();
	void set_observer(std::function<void(const VirtualRfBurst &)> obs) { observer_ = std::move(obs); }
	// 0 keeps each burst's own timeslot; 1 or 2 forces the link to that TS.
	void set_timeslot(uint8_t ts) { timeslot_.store(ts == 1 || ts == 2 ? ts : 0); }
	uint8_t timeslot() const { return timeslot_.load(); }

	uint64_t sent() const { return sent_.load(); }
	uint64_t delivered() const { return delivered_.load(); }
	uint64_t dropped() const { return dropped_.load(); }

private:
	static constexpr size_t MAX_QUEUE = 256;
	mutable std::mutex mutex_;
	std::map<uint8_t, std::deque<VirtualRfBurst>> mailboxes_;
	std::atomic<uint64_t> sent_{0};
	std::atomic<uint64_t> delivered_{0};
	std::atomic<uint64_t> dropped_{0};
	std::atomic<uint8_t> timeslot_{0};
	std::function<void(const VirtualRfBurst &)> observer_;
};
