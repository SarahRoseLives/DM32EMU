// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "rf_channel.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// A radio front end presents one mode-independent IQ boundary to the RF
// channel. Modes only see their normal hardware interfaces above it.
class RfFrontend {
public:
	using IqSink = std::function<void(const IqSample *, size_t)>;

	virtual ~RfFrontend() = default;
	virtual void set_iq_sink(IqSink sink) { sink_ = std::move(sink); }
	virtual void receive_iq(const IqSample *samples, size_t count) = 0;
	virtual void set_frequency(uint32_t hz) = 0;
	virtual uint32_t frequency() const = 0;

protected:
	void emit_iq(const IqSample *samples, size_t count)
	{
		if (sink_) sink_(samples, count);
	}

	IqSink sink_;
};

// An FM/analog PHY used to exercise the IQ channel without any mode code.
// It is deliberately a modem primitive, not a mode: audio samples in,
// complex baseband out and back.
class FmFrontend : public RfFrontend {
public:
	explicit FmFrontend(uint32_t deviation_hz = 3000);

	void receive_iq(const IqSample *samples, size_t count) override;
	void set_frequency(uint32_t hz) override { frequency_ = hz; }
	uint32_t frequency() const override { return frequency_; }

	void modulate(const int16_t *audio, size_t count);
	void demodulate(const IqSample *samples, size_t count);
	void set_audio_sink(std::function<void(const int16_t *, size_t)> sink) { audio_sink_ = std::move(sink); }
	void reset();

private:
	uint32_t frequency_ = 0;
	uint32_t deviation_hz_;
	double phase_ = 0.0;
	double last_i_ = 1.0;
	double last_q_ = 0.0;
	std::vector<int16_t> audio_;
	std::function<void(const int16_t *, size_t)> audio_sink_;
};
