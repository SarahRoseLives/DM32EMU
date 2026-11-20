// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "rf_channel.hpp"
#include "rf_frontend.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// A hardware 4FSK burst modem. It carries symbols, not modes, and presents the
// same IQ boundary as every other front end.
class PacketFrontend : public RfFrontend {
public:
	explicit PacketFrontend(uint32_t symbol_rate = 4800, uint32_t deviation_hz = 1800);

	void receive_iq(const IqSample *samples, size_t count) override;
	void set_frequency(uint32_t hz) override { frequency_ = hz; }
	uint32_t frequency() const override { return frequency_; }

	void transmit_dibits(const uint8_t *dibits, size_t count);
	void set_dibit_sink(std::function<void(const uint8_t *, size_t)> sink) { dibit_sink_ = std::move(sink); }
	void reset();

private:
	static constexpr uint32_t kSampleRate = VirtualRfIqChannel::SAMPLE_RATE_HZ;
	static constexpr uint32_t kOversample = 50;

	uint32_t symbol_rate_;
	uint32_t deviation_hz_;
	uint32_t frequency_ = 0;
	double phase_ = 0.0;
	double last_i_ = 1.0;
	double last_q_ = 0.0;
	double sym_acc_ = 0.0;
	unsigned sym_count_ = 0;
	std::function<void(const uint8_t *, size_t)> dibit_sink_;
};
