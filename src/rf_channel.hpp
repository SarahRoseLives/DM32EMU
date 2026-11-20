// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <atomic>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <vector>

// A mode-invisible RF channel. It knows only complex baseband samples; the
// radios' front ends decide what modulation and framing those samples carry.
struct IqSample {
	float i = 0.0f;
	float q = 0.0f;
};

struct RfImpairments {
	bool enabled = false;		// off = the perfect, deterministic channel
	float gain = 1.0f;
	float phase = 0.0f;		// radians
	float noise_rms = 0.0f;		// added to I and Q
};

class VirtualRfIqChannel {
public:
	static constexpr uint32_t SAMPLE_RATE_HZ = 240000;
	static constexpr size_t MAX_BLOCKS = 256;

	void add_receiver(uint8_t id);
	void remove_receiver(uint8_t id);
	void clear();

	void set_impairments(const RfImpairments &impairments);
	RfImpairments impairments() const;

	void transmit(uint8_t source, const IqSample *samples, size_t count);
	bool receive(uint8_t receiver, std::vector<IqSample> &out);

	uint64_t transmitted_samples() const { return transmitted_samples_.load(); }
	uint64_t delivered_samples() const { return delivered_samples_.load(); }
	uint64_t dropped_blocks() const { return dropped_blocks_.load(); }

private:
	struct Block {
		uint8_t source = 0;
		std::vector<IqSample> samples;
	};

	std::vector<IqSample> impair(const std::vector<IqSample> &in);

	mutable std::mutex mutex_;
	std::map<uint8_t, std::deque<Block>> mailboxes_;
	RfImpairments impairments_;
	std::mt19937 rng_{0x4D333245u};
	std::atomic<uint64_t> transmitted_samples_{0};
	std::atomic<uint64_t> delivered_samples_{0};
	std::atomic<uint64_t> dropped_blocks_{0};
};
