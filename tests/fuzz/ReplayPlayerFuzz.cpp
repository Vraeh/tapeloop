// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayPlayerState.hpp"

#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using tapeloop::AirConfig;
using tapeloop::AirPhase;
using tapeloop::ReplayPlayerState;
using tapeloop::SequenceEntry;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;

namespace {

constexpr size_t kKeys = 6;

// One byte per choice, so hand-made seeds stay short.
size_t choose(FuzzInput &input, size_t count)
{
	return input.byte() % count;
}

std::string keyOf(size_t index)
{
	return std::string(1, static_cast<char>('a' + index));
}

// A list that may name a source twice or leave some out.
std::vector<SequenceEntry> readEntries(FuzzInput &input)
{
	std::vector<SequenceEntry> entries(choose(input, kKeys + 3));
	for (SequenceEntry &entry : entries) {
		entry.sourceKey = keyOf(choose(input, kKeys));
		entry.shown = input.flag();
	}
	return entries;
}

// The sources a replay has clips of, sorted.
std::vector<std::string> readSources(FuzzInput &input)
{
	std::vector<std::string> sources;
	const uint8_t which = input.byte();
	for (size_t i = 0; i < kKeys; ++i) {
		if ((which >> i) & 1) {
			sources.push_back(keyOf(i));
		}
	}
	return sources;
}

// The first shown source with a clip at or after `from`, as the list stands.
std::optional<size_t> nextShown(const ReplayPlayerState &player, size_t from, const std::vector<std::string> &sources)
{
	const auto entries = player.sequence().entries();
	for (size_t i = from; i < entries.size(); ++i) {
		if (entries[i].shown && std::binary_search(sources.begin(), sources.end(), entries[i].sourceKey)) {
			return i;
		}
	}
	return std::nullopt;
}

// The first shown entry with a clip at or after `from`, in a copy of the list.
std::optional<size_t> firstPlayable(const std::vector<SequenceEntry> &entries, size_t from,
				    const std::vector<std::string> &sources)
{
	for (size_t i = from; i < entries.size(); ++i) {
		if (entries[i].shown &&
		    std::find(sources.begin(), sources.end(), entries[i].sourceKey) != sources.end()) {
			return i;
		}
	}
	return std::nullopt;
}

std::optional<size_t> placeOf(const std::vector<SequenceEntry> &entries, const std::string &key)
{
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].sourceKey == key) {
			return i;
		}
	}
	return std::nullopt;
}

// What follows the sources: the outro if the replay has one, else live.
AirPhase afterSources(const AirConfig &airing)
{
	return airing.outro ? AirPhase::Outro : AirPhase::Live;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	AirConfig config{input.flag(), input.flag()};
	ReplayPlayerState player(config);
	// The model's view of the replay on air: its sources, its configuration and its speed.
	std::vector<std::string> sources;
	AirConfig airing;
	double speed = 1.0;

	while (!input.empty()) {
		const uint64_t token = player.token();
		const AirPhase phase = player.phase();
		const std::string source(player.source());
		const std::optional<size_t> index = player.sequence().indexOf(source);
		const std::vector<SequenceEntry> before(player.sequence().entries().begin(),
							player.sequence().entries().end());
		// Where the rules say the replay goes when the source on air leaves the list or is
		// hidden: from `from` in the list as it is after the edit. Empty to stay as it is.
		std::optional<size_t> expectedFrom;

		switch (choose(input, 8)) {
		case 0: {
			player.setEntries(readEntries(input));
			const std::vector<SequenceEntry> after(player.sequence().entries().begin(),
							       player.sequence().entries().end());
			if (phase == AirPhase::Source) {
				if (const std::optional<size_t> now = placeOf(after, source)) {
					if (!after[*now].shown) {
						expectedFrom = *now + 1;
					}
				} else {
					expectedFrom = after.size();
					for (size_t i = *index + 1; i < before.size(); ++i) {
						if (const std::optional<size_t> kept =
							    placeOf(after, before[i].sourceKey)) {
							expectedFrom = *kept;
							break;
						}
					}
				}
			}
			break;
		}
		case 1: {
			const size_t from = choose(input, kKeys + 1);
			const size_t to = choose(input, kKeys + 1);
			const size_t count = player.sequence().entries().size();
			require(player.move(from, to) == (from < count && to < count));
			// The source playing keeps playing wherever it goes.
			require(player.token() == token);
			break;
		}
		case 2: {
			const std::string key = keyOf(choose(input, kKeys));
			const bool known = placeOf(before, key).has_value();
			const bool shown = input.flag();
			require(player.setShown(key, shown) == known);
			if (phase == AirPhase::Source && key == source && !shown) {
				expectedFrom = *index + 1;
			}
			break;
		}
		case 3: {
			std::vector<std::string> offered = readSources(input);
			const bool expected = phase == AirPhase::Live && nextShown(player, 0, offered).has_value();
			const bool started = player.start(offered);
			require(started == expected);
			if (started) {
				sources = offered;
				airing = config;
				speed = 1.0;
				require(player.speed() == 1.0);
				require(player.phase() == (airing.intro ? AirPhase::Intro : AirPhase::Source));
			} else {
				require(player.token() == token);
			}
			break;
		}
		case 4: {
			if (input.flag()) {
				// A report for a part already gone, or for one yet to come, changes nothing.
				player.ended(input.flag() ? token + 1 : token - 1);
				require(player.token() == token);
				break;
			}
			player.ended(token);
			if (phase == AirPhase::Live) {
				require(player.token() == token);
			} else if (phase == AirPhase::Outro) {
				require(player.phase() == AirPhase::Live);
			} else {
				const size_t from = phase == AirPhase::Intro ? 0 : *index + 1;
				const std::optional<size_t> next = nextShown(player, from, sources);
				if (next) {
					require(player.phase() == AirPhase::Source);
					require(player.source() == player.sequence().entries()[*next].sourceKey);
				} else {
					require(player.phase() == afterSources(airing));
				}
			}
			break;
		}
		case 5:
			player.cutShort();
			if (phase == AirPhase::Live) {
				require(player.token() == token);
			} else if (phase == AirPhase::Outro) {
				require(player.phase() == AirPhase::Live);
			} else {
				require(player.phase() == afterSources(airing));
			}
			break;
		case 6: {
			const double wanted = static_cast<double>(static_cast<int8_t>(input.byte())) / 4.0;
			const bool valid = wanted >= ReplayPlayerState::kMinSpeed &&
					   wanted <= ReplayPlayerState::kMaxSpeed;
			require(player.setSpeed(wanted) == valid);
			if (valid) {
				speed = wanted;
			}
			break;
		}
		case 7:
			config = AirConfig{input.flag(), input.flag()};
			player.setConfig(config);
			require(player.token() == token);
			break;
		}

		if (expectedFrom) {
			const std::vector<SequenceEntry> after(player.sequence().entries().begin(),
							       player.sequence().entries().end());
			if (const std::optional<size_t> next = firstPlayable(after, *expectedFrom, sources)) {
				require(player.phase() == AirPhase::Source);
				require(player.source() == after[*next].sourceKey);
			} else {
				require(player.phase() == afterSources(airing));
			}
		}

		// A part on air is always one the caller can play, and the token changes exactly
		// when the part on air does.
		require(player.token() >= token);
		require((player.phase() != phase || player.source() != source) == (player.token() != token));
		require(player.speed() == speed);
		if (player.phase() == AirPhase::Source) {
			const std::optional<size_t> playing = player.sequence().indexOf(player.source());
			require(playing.has_value());
			require(player.sequence().entries()[*playing].shown);
			require(std::binary_search(sources.begin(), sources.end(), std::string(player.source())));
		} else {
			require(player.source().empty());
		}
		require(player.speed() > 0.0);
	}

	// Whatever happened, ending each part in turn reaches live.
	for (size_t i = 0; i < kKeys + 3 && player.phase() != AirPhase::Live; ++i) {
		player.ended(player.token());
	}
	require(player.phase() == AirPhase::Live);
	return 0;
}
