// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "rf_frontend.hpp"

#include <algorithm>
#include <cmath>

FmFrontend::FmFrontend(uint32_t deviation_hz) : deviation_hz_(deviation_hz) {}

void FmFrontend::reset()
{
	phase_ = 0.0;
	last_i_ = 1.0;
	last_q_ = 0.0;
	audio_.clear();
}

void FmFrontend::modulate(const int16_t *audio, size_t count)
{
	constexpr size_t ratio = VirtualRfIqChannel::SAMPLE_RATE_HZ / 8000;
	std::vector<IqSample> iq;
	iq.reserve(count * ratio);
	for (size_t n = 0; n < count; n++) {
		const double deviation = (double)audio[n] / 32768.0 * deviation_hz_;
		const double step = 2.0 * M_PI * deviation / VirtualRfIqChannel::SAMPLE_RATE_HZ;
		for (size_t k = 0; k < ratio; k++) {
			iq.push_back({(float)std::cos(phase_), (float)std::sin(phase_)});
			phase_ += step;
			if (phase_ > M_PI) phase_ -= 2.0 * M_PI;
			if (phase_ < -M_PI) phase_ += 2.0 * M_PI;
		}
	}
	emit_iq(iq.data(), iq.size());
}

void FmFrontend::receive_iq(const IqSample *samples, size_t count)
{
	constexpr size_t ratio = VirtualRfIqChannel::SAMPLE_RATE_HZ / 8000;
	demodulate(samples, count);
	if (audio_.size() < count / ratio) return;
	if (audio_sink_) audio_sink_(audio_.data(), audio_.size());
	audio_.clear();
}

void FmFrontend::demodulate(const IqSample *samples, size_t count)
{
	constexpr size_t ratio = VirtualRfIqChannel::SAMPLE_RATE_HZ / 8000;
	for (size_t n = 0; n < count; n++) {
		const double i = samples[n].i;
		const double q = samples[n].q;
		double dphi = std::atan2(q * last_i_ - i * last_q_, i * last_i_ + q * last_q_);
		last_i_ = i;
		last_q_ = q;
		if ((n + 1) % ratio == 0) {
			const double scale = 32768.0 * VirtualRfIqChannel::SAMPLE_RATE_HZ /
					     (2.0 * M_PI * (double)deviation_hz_);
			double v = dphi * scale;
			v = std::clamp(v, -32768.0, 32767.0);
			audio_.push_back((int16_t)std::lround(v));
		}
	}
}
