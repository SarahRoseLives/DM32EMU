// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "virtual_rf.hpp"

void VirtualRfBus::add_receiver(uint8_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	mailboxes_.try_emplace(id);
}

void VirtualRfBus::remove_receiver(uint8_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	mailboxes_.erase(id);
}

void VirtualRfBus::send(const VirtualRfBurst &burst)
{
	VirtualRfBurst queued = burst;
	const uint8_t forced_ts = timeslot_.load(std::memory_order_relaxed);
	if (forced_ts != 0) queued.ts = forced_ts;

	std::function<void(const VirtualRfBurst &)> obs;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sent_.fetch_add(1, std::memory_order_relaxed);
		for (auto &[id, queue] : mailboxes_) {
			if (id == queued.source) continue;
			if (queue.size() >= MAX_QUEUE) {
				queue.pop_front();
				dropped_.fetch_add(1, std::memory_order_relaxed);
			}
			queue.push_back(queued);
		}
		obs = observer_;
	}
	if (obs) obs(queued);
}

bool VirtualRfBus::receive(uint8_t receiver, VirtualRfBurst &burst)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = mailboxes_.find(receiver);
	if (it == mailboxes_.end() || it->second.empty()) return false;
	burst = it->second.front();
	it->second.pop_front();
	delivered_.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void VirtualRfBus::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &[id, queue] : mailboxes_) queue.clear();
}
