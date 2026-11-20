// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "virtual_rf.hpp"

void VirtualRfBus::send(const VirtualRfBurst &burst)
{
	VirtualRfBurst queued = burst;
	const uint8_t forced_ts = timeslot_.load(std::memory_order_relaxed);
	if (forced_ts != 0) queued.ts = forced_ts;

	std::function<void(const VirtualRfBurst &)> obs;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sent_.fetch_add(1, std::memory_order_relaxed);
		if (queue_.size() >= MAX_QUEUE) {
			queue_.pop_front();
			dropped_.fetch_add(1, std::memory_order_relaxed);
		}
		queue_.push_back(queued);
		obs = observer_;
	}
	if (obs) obs(queued);
}

bool VirtualRfBus::receive(uint8_t receiver, VirtualRfBurst &burst)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = queue_.begin(); it != queue_.end(); ++it) {
		if (it->source == receiver) continue;
		burst = *it;
		queue_.erase(it);
		delivered_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	return false;
}

void VirtualRfBus::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	queue_.clear();
}
