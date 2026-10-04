// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MomentList.hpp"

#include "AllocationCounter.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

using tapeloop::Clip;
using tapeloop::Gop;
using tapeloop::Moment;
using tapeloop::MomentList;
using tapeloop::MomentListConfig;
using tapeloop::test::SyntheticEncoder;

namespace {

constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
// Default SyntheticEncoder GOP: one 4096-byte keyframe and 29 frames of 1024 bytes.
constexpr size_t kGopBytes = 4096 + 29 * 1024;

class Source {
public:
	explicit Source(int gopCount)
	{
		tapeloop::GopBuilder builder(encoder_.frameDuration());
		while (gops_.size() < static_cast<size_t>(gopCount)) {
			builder.append(encoder_.next());
			if (encoder_.isKeyframe(encoder_.nextFrame())) {
				gops_.push_back(builder.seal());
			}
		}
	}

	// A clip over GOPs first to last, both included.
	Clip clip(int first, int last) const
	{
		return Clip(gops_, encoder_.timeOf(first * 30), encoder_.timeOf(last * 30 + 29));
	}

private:
	SyntheticEncoder encoder_{SyntheticEncoder::Config{}};
	std::vector<std::shared_ptr<const Gop>> gops_;
};

MomentListConfig limits(size_t maxMoments, size_t maxBytes)
{
	MomentListConfig config;
	config.maxMoments = maxMoments;
	config.maxBytes = maxBytes;
	return config;
}

Moment momentOf(std::vector<Clip> clips)
{
	Moment moment;
	for (size_t i = 0; i < clips.size(); ++i) {
		moment.clips.push_back({"source " + std::to_string(i), std::move(clips[i])});
	}
	return moment;
}

std::vector<uint64_t> ids(const MomentList &list)
{
	std::vector<uint64_t> result;
	for (const Moment &moment : list.moments()) {
		result.push_back(moment.id);
	}
	return result;
}

} // namespace

TEST_CASE("MomentList gives increasing ids that are never reused")
{
	const Source source(1);
	MomentList list(limits(2, kUnlimited));

	CHECK(list.add(momentOf({source.clip(0, 0)})) == 1);
	CHECK(list.add(momentOf({source.clip(0, 0)})) == 2);
	CHECK(list.add(momentOf({source.clip(0, 0)})) == 3);
	CHECK(list.remove(3));
	list.clear();
	CHECK(list.add(momentOf({source.clip(0, 0)})) == 4);
	CHECK(ids(list) == std::vector<uint64_t>{4});
}

TEST_CASE("MomentList does not store a moment without clips")
{
	const Source source(1);
	MomentList list(limits(1, kUnlimited));
	list.add(momentOf({source.clip(0, 0)}));

	CHECK(list.add({}) == 0);
	CHECK(ids(list) == std::vector<uint64_t>{1});
	CHECK(list.add(momentOf({source.clip(0, 0)})) == 2);
}

TEST_CASE("MomentList drops the oldest moments beyond its count")
{
	const Source source(1);
	MomentList list(limits(3, kUnlimited));
	for (int i = 0; i < 5; ++i) {
		list.add(momentOf({source.clip(0, 0)}));
	}

	CHECK(list.size() == 3);
	CHECK(ids(list) == std::vector<uint64_t>{3, 4, 5});
	CHECK(list.find(1) == nullptr);
	REQUIRE(list.find(4) != nullptr);
	CHECK(list.find(4)->id == 4);
	CHECK(list.byteSize() == kGopBytes);
}

TEST_CASE("MomentList counts GOPs shared between moments once")
{
	const Source source(6);
	MomentList list(limits(10, 4 * kGopBytes));

	list.add(momentOf({source.clip(0, 2)}));
	CHECK(list.byteSize() == 3 * kGopBytes);

	list.add(momentOf({source.clip(1, 3)}));
	CHECK(list.byteSize() == 4 * kGopBytes);
	CHECK(list.size() == 2);

	// Six GOPs held would exceed four: the first moment goes, then the second.
	list.add(momentOf({source.clip(3, 5)}));
	CHECK(ids(list) == std::vector<uint64_t>{3});
	CHECK(list.byteSize() == 3 * kGopBytes);
}

TEST_CASE("MomentList counts GOPs of every source in a moment")
{
	const Source wide(4);
	const Source close(4);
	MomentList list(limits(10, kUnlimited));

	list.add(momentOf({wide.clip(0, 1), close.clip(1, 3)}));
	CHECK(list.byteSize() == 5 * kGopBytes);
	CHECK(list.moments()[0].clips[1].sourceKey == "source 1");

	list.add(momentOf({wide.clip(1, 2), close.clip(2, 2)}));
	CHECK(list.byteSize() == 6 * kGopBytes);

	CHECK(list.remove(1));
	CHECK(list.byteSize() == 3 * kGopBytes);
	CHECK_FALSE(list.remove(1));
}

TEST_CASE("MomentList keeps a new moment larger than its budget")
{
	const Source source(4);
	MomentList list(limits(10, kGopBytes));

	list.add(momentOf({source.clip(0, 0)}));
	list.add(momentOf({source.clip(1, 3)}));
	CHECK(ids(list) == std::vector<uint64_t>{2});
	CHECK(list.byteSize() == 3 * kGopBytes);
}

TEST_CASE("MomentList releases everything on clear")
{
	const Source source(3);
	MomentList list(limits(10, kUnlimited));
	list.add(momentOf({source.clip(0, 2)}));
	list.clear();

	CHECK(list.size() == 0);
	CHECK(list.byteSize() == 0);
	list.add(momentOf({source.clip(0, 0)}));
	CHECK(list.byteSize() == kGopBytes);
}

TEST_CASE("MomentList counts a GOP held twice by one moment once")
{
	const Source source(4);
	MomentList list(limits(10, kUnlimited));

	list.add(momentOf({source.clip(0, 2), source.clip(1, 3)}));
	CHECK(list.byteSize() == 4 * kGopBytes);
	CHECK(list.remove(1));
	CHECK(list.byteSize() == 0);
}

TEST_CASE("MomentList is unchanged when adding fails")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocation failures cannot be injected in this configuration");
	}

	const Source source(6);

	// Fail each allocation of add() in turn, then let it through.
	for (size_t skip = 0; skip < 12; ++skip) {
		CAPTURE(skip);
		MomentList list(limits(10, kUnlimited));
		list.add(momentOf({source.clip(0, 0)}));
		Moment moment = momentOf({source.clip(1, 3), source.clip(2, 4)});

		bool failed = false;
		{
			tapeloop::test::AllocationFailure failure(skip);
			try {
				list.add(std::move(moment));
			} catch (const std::bad_alloc &) {
				failed = true;
			}
		}

		if (failed) {
			CHECK(ids(list) == std::vector<uint64_t>{1});
			CHECK(list.byteSize() == kGopBytes);
			CHECK(list.remove(1));
			CHECK(list.byteSize() == 0);
		} else {
			CHECK(ids(list) == std::vector<uint64_t>{1, 2});
			CHECK(list.byteSize() == 5 * kGopBytes);
		}
	}
}
