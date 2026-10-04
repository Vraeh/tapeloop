// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MomentList.hpp"

#include <algorithm>
#include <utility>

namespace tapeloop {

MomentList::MomentList(MomentListConfig config) : config_(config) {}

uint64_t MomentList::add(Moment moment)
{
	if (moment.clips.empty()) {
		return 0;
	}

	moment.id = nextId_;
	moments_.push_back(std::move(moment));
	try {
		hold(moments_.back());
	} catch (...) {
		moments_.pop_back();
		throw;
	}
	++nextId_;

	while (moments_.size() > 1 && (moments_.size() > config_.maxMoments || bytes_ > config_.maxBytes)) {
		dropOldest();
	}

	return moments_.back().id;
}

bool MomentList::remove(uint64_t id)
{
	const auto found =
		std::find_if(moments_.begin(), moments_.end(), [id](const Moment &moment) { return moment.id == id; });
	if (found == moments_.end()) {
		return false;
	}

	release(*found);
	moments_.erase(found);
	return true;
}

void MomentList::clear()
{
	moments_.clear();
	gopHolders_.clear();
	bytes_ = 0;
}

const Moment *MomentList::find(uint64_t id) const
{
	const auto found =
		std::find_if(moments_.begin(), moments_.end(), [id](const Moment &moment) { return moment.id == id; });
	return found == moments_.end() ? nullptr : &*found;
}

void MomentList::hold(const Moment &moment)
{
	size_t counted = 0;
	try {
		for (const MomentClip &entry : moment.clips) {
			for (const auto &gop : entry.clip.gops()) {
				if (gopHolders_[gop.get()]++ == 0) {
					bytes_ += gop->byteSize();
				}
				++counted;
			}
		}
	} catch (...) {
		release(moment, counted);
		throw;
	}
}

void MomentList::release(const Moment &moment, size_t count) noexcept
{
	for (const MomentClip &entry : moment.clips) {
		for (const auto &gop : entry.clip.gops()) {
			if (count-- == 0) {
				return;
			}
			const auto holders = gopHolders_.find(gop.get());
			if (--holders->second == 0) {
				bytes_ -= gop->byteSize();
				gopHolders_.erase(holders);
			}
		}
	}
}

void MomentList::dropOldest()
{
	release(moments_.front());
	moments_.erase(moments_.begin());
}

} // namespace tapeloop
