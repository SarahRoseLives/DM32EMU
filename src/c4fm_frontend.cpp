// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "c4fm_frontend.hpp"

#include <algorithm>
#include <cmath>

namespace {

double sym_level(uint8_t dibit)
{
	switch (dibit & 3u) {
	case 1: return 3.0;	// 01
	case 0: return 1.0;	// 00
	case 2: return -1.0;	// 10
	default: return -3.0;	// 11
	}
}

} // namespace

C4fmFrontend::C4fmFrontend(uint32_t symbol_rate, uint32_t deviation_hz)
	: symbol_rate_(symbol_rate), deviation_hz_(deviation_hz)
{
}

const std::vector<double> &C4fmFrontend::pulse()
{
	static const std::vector<double> h = [] {
		constexpr int n = 2 * kSpan * kOversample + 1;
		const double rate = (double)kSampleRate / kOversample;
		const double alpha = 0.2;
		const double f1 = rate * (1.0 - alpha) / 2.0;
		const double f2 = rate * (1.0 + alpha) / 2.0;
		std::vector<double> p(n);
		double sum = 0.0;
		for (int i = 0; i < n; i++) {
			const double t = (double)(i - kSpan * kOversample) / kSampleRate;
			double v = 0.0;
			for (double f = 2.5; f < f2; f += 5.0) {
				const double hf = f < f1 ? 1.0 : 0.5 + 0.5 * std::cos(M_PI * (f - f1) / (f2 - f1));
				const double xx = M_PI * f / rate;
				v += hf * xx / std::sin(xx) * std::cos(2.0 * M_PI * f * t);
			}
			p[i] = v;
			sum += v;
		}
		for (double &v : p) v *= kOversample / sum;
		// The chip's transmit filter: the same windowed-sinc low-pass the
		// reference C4FM model applies before the discriminator.
		constexpr int lpf_n = 241;
		const double hi = 6000.0;
		double lpf[lpf_n];
		double lsum = 0.0;
		for (int i = -120; i <= 120; i++) {
			const double xx = 2.0 * M_PI * hi / kSampleRate * i;
			const double win = 0.54 + 0.46 * std::cos(M_PI * i / 120.0);
			lpf[i + 120] = (i ? std::sin(xx) / xx : 1.0) * win;
			lsum += lpf[i + 120];
		}
		std::vector<double> filtered(p.size() + lpf_n - 1, 0.0);
		for (size_t i = 0; i < p.size(); i++)
			for (int j = 0; j < lpf_n; j++)
				filtered[i + j] += p[i] * lpf[j] / lsum;
		return filtered;
	}();
	return h;
}

void C4fmFrontend::ensure(uint64_t end)
{
	if (freq_samples_.size() < end) freq_samples_.resize(end + kSampleRate, 0.0f);
}

void C4fmFrontend::add_dibits(const uint8_t *dibits, size_t count, uint64_t start_sample)
{
	if (dibits == nullptr || count == 0) return;
	const auto &h = pulse();
	const double dev = (double)deviation_hz_ * 3.0 / 3.0;
	const uint64_t last = start_sample + count * kOversample + h.size();
	ensure(last);
	for (size_t k = 0; k < count; k++) {
		const double level = sym_level(dibits[k]) * dev / 3.0;
		const int64_t base = (int64_t)(start_sample + k * kOversample) - kSpan * kOversample;
		for (size_t i = 0; i < h.size(); i++) {
			const int64_t at = base + (int64_t)i;
			if (at >= 0) freq_samples_[(size_t)at] += (float)(level * h[i]);
		}
	}
}

void C4fmFrontend::transmit_dibits(const uint8_t *dibits, size_t count)
{
	if (dibits == nullptr || count == 0) return;
	add_dibits(dibits, count, write_sample_);
	write_sample_ += count * kOversample;
	generate(count * kOversample);
}

void C4fmFrontend::transmit_phy_block(const uint8_t block[36])
{
	uint8_t dibits[144];
	for (size_t k = 0; k < 144; k++) {
		const size_t bit = k * 2;
		dibits[k] = (uint8_t)((block[bit / 8] >> (6 - (bit & 6))) & 3u);
	}
	transmit_dibits(dibits, 144);
}

void C4fmFrontend::generate(size_t count)
{
	ensure(next_sample_ + count);
	std::vector<IqSample> iq;
	iq.reserve(count);
	for (size_t n = 0; n < count; n++) {
		const double f = freq_samples_[next_sample_++];
		phase_ += 2.0 * M_PI * f / kSampleRate;
		if (phase_ > M_PI) phase_ -= 2.0 * M_PI;
		if (phase_ < -M_PI) phase_ += 2.0 * M_PI;
		iq.push_back({(float)std::cos(phase_), (float)std::sin(phase_)});
	}
	emit_iq(iq.data(), iq.size());
}

void C4fmFrontend::receive_iq(const IqSample *samples, size_t count)
{
	constexpr unsigned ratio = kSampleRate / 8000;
	std::vector<int16_t> out;
	out.reserve(count / ratio + 1);
	for (size_t n = 0; n < count; n++) {
		const double i = samples[n].i;
		const double q = samples[n].q;
		const double dphi = std::atan2(q * last_i_ - i * last_q_, i * last_i_ + q * last_q_);
		last_i_ = i;
		last_q_ = q;
		disc_acc_ += dphi;
		if (++disc_count_ == ratio) {
			const double scale = 32768.0 * kSampleRate /
					     (2.0 * M_PI * (double)deviation_hz_ * ratio);
			double v = disc_acc_ * scale;
			v = std::clamp(v, -32768.0, 32767.0);
			out.push_back((int16_t)std::lround(v));
			disc_acc_ = 0.0;
			disc_count_ = 0;
		}
	}
	if (disc_sink_ && !out.empty()) disc_sink_(out.data(), out.size());
}

void C4fmFrontend::reset()
{
	next_sample_ = 0;
	write_sample_ = 0;
	phase_ = 0.0;
	last_i_ = 1.0;
	last_q_ = 0.0;
	disc_acc_ = 0.0;
	disc_count_ = 0;
	freq_samples_.clear();
}
