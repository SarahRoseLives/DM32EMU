// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "packet_frontend.hpp"

#include <cmath>

namespace {

double level_of(uint8_t dibit)
{
	switch (dibit & 3u) {
	case 1: return 3.0;
	case 0: return 1.0;
	case 2: return -1.0;
	default: return -3.0;
	}
}

} // namespace

PacketFrontend::PacketFrontend(uint32_t symbol_rate, uint32_t deviation_hz)
	: symbol_rate_(symbol_rate), deviation_hz_(deviation_hz)
{
}

void PacketFrontend::transmit_dibits(const uint8_t *dibits, size_t count)
{
	if (dibits == nullptr || count == 0) return;
	std::vector<IqSample> iq;
	iq.reserve(count * kOversample);
	for (size_t k = 0; k < count; k++) {
		const double step = 2.0 * M_PI * level_of(dibits[k]) * (double)deviation_hz_ / 3.0 / kSampleRate;
		for (uint32_t n = 0; n < kOversample; n++) {
			iq.push_back({(float)std::cos(phase_), (float)std::sin(phase_)});
			phase_ += step;
			if (phase_ > M_PI) phase_ -= 2.0 * M_PI;
			if (phase_ < -M_PI) phase_ += 2.0 * M_PI;
		}
	}
	emit_iq(iq.data(), iq.size());
}

void PacketFrontend::receive_iq(const IqSample *samples, size_t count)
{
	std::vector<uint8_t> dibits;
	dibits.reserve(count / kOversample + 1);
	for (size_t n = 0; n < count; n++) {
		const double i = samples[n].i;
		const double q = samples[n].q;
		const double dphi = std::atan2(q * last_i_ - i * last_q_, i * last_i_ + q * last_q_);
		last_i_ = i;
		last_q_ = q;
		sym_acc_ += dphi;
		if (++sym_count_ == kOversample) {
			const double scale = 3.0 * kSampleRate /
					     (2.0 * M_PI * (double)deviation_hz_ * kOversample);
			const double level = sym_acc_ * scale;
			dibits.push_back(level > 2.0 ? 1u : level > 0.0 ? 0u : level > -2.0 ? 2u : 3u);
			sym_acc_ = 0.0;
			sym_count_ = 0;
		}
	}
	if (dibit_sink_ && !dibits.empty()) dibit_sink_(dibits.data(), dibits.size());
}

void PacketFrontend::reset()
{
	phase_ = 0.0;
	last_i_ = 1.0;
	last_q_ = 0.0;
	sym_acc_ = 0.0;
	sym_count_ = 0;
}
