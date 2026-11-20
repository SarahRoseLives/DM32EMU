// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "rf_channel.hpp"
#include "rf_frontend.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <vector>

// A hardware C4FM/4FSK front end. It knows dibits, not modes: a PHY block is
// just a run of symbols, and the receive side emits discriminator samples.
class C4fmFrontend : public RfFrontend {
public:
	explicit C4fmFrontend(uint32_t symbol_rate = 4800, uint32_t deviation_hz = 1800);

	void receive_iq(const IqSample *samples, size_t count) override;
	void set_frequency(uint32_t hz) override { frequency_ = hz; }
	uint32_t frequency() const override { return frequency_; }

	// Add symbols at an absolute 240 kHz sample position. A PHY block's 36
	// bytes are 144 of these dibits (first in bits 7-6).
	void add_dibits(const uint8_t *dibits, size_t count, uint64_t start_sample);
	void transmit_dibits(const uint8_t *dibits, size_t count);
	void transmit_phy_block(const uint8_t block[36]);
	void generate(size_t count);
	void set_discriminator_sink(std::function<void(const int16_t *, size_t)> sink) { disc_sink_ = std::move(sink); }
	void reset();

private:
	static constexpr uint32_t kSampleRate = VirtualRfIqChannel::SAMPLE_RATE_HZ;
	static constexpr uint32_t kOversample = 50;
	static constexpr int kSpan = 8;

	void ensure(uint64_t end);
	static const std::vector<double> &pulse();

	uint32_t symbol_rate_;
	uint32_t deviation_hz_;
	uint32_t frequency_ = 0;
	uint64_t next_sample_ = 0;
	uint64_t write_sample_ = 0;
	double phase_ = 0.0;
	double last_i_ = 1.0;
	double last_q_ = 0.0;
	double disc_acc_ = 0.0;
	unsigned disc_count_ = 0;
	std::vector<float> freq_samples_;
	std::function<void(const int16_t *, size_t)> disc_sink_;
};
