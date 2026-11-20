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
	float frequency_offset_hz = 0.0f;
	float sample_clock_ppm = 0.0f;
	float fading_depth = 0.0f;	// 0 = flat, 1 = full null
	float fading_rate_hz = 0.0f;
	uint32_t delay_samples = 0;
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
	void transmit(uint8_t source, const IqSample *samples, size_t count, uint64_t start_sample);
	bool receive(uint8_t receiver, std::vector<IqSample> &out);
	bool receive_at(uint8_t receiver, uint64_t time, std::vector<IqSample> &out);
	void set_jitter_samples(uint64_t samples) { jitter_samples_ = samples; }
	uint64_t jitter_samples() const { return jitter_samples_; }
	uint64_t sample_time(uint64_t cpu_cycles) const { return cpu_cycles / 700u; }

	uint64_t transmitted_samples() const { return transmitted_samples_.load(); }
	uint64_t delivered_samples() const { return delivered_samples_.load(); }
	uint64_t dropped_blocks() const { return dropped_blocks_.load(); }

private:
	struct Block {
		uint8_t source = 0;
		uint64_t start_sample = 0;
		std::vector<IqSample> samples;
	};

	std::vector<IqSample> impair(const std::vector<IqSample> &in, uint64_t start_sample);

	mutable std::mutex mutex_;
	std::map<uint8_t, std::deque<Block>> mailboxes_;
	RfImpairments impairments_;
	double impair_phase_ = 0.0;
	double fading_phase_ = 0.0;
	double sample_phase_ = 0.0;
	uint64_t jitter_samples_ = 0;
	std::mt19937 rng_{0x4D333245u};
	std::atomic<uint64_t> transmitted_samples_{0};
	std::atomic<uint64_t> delivered_samples_{0};
	std::atomic<uint64_t> dropped_blocks_{0};
};
