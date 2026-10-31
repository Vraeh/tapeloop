// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "Mp4Reader.hpp"

#include <cstddef>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tapeloop::test {
namespace {

// Big-endian fields, each checked against what is left.
class Fields {
public:
	explicit Fields(std::span<const uint8_t> bytes) : bytes_(bytes) {}

	uint64_t take(size_t size)
	{
		need(size);
		uint64_t value = 0;
		for (size_t i = 0; i < size; ++i) {
			value = value << 8 | bytes_[at_ + i];
		}
		at_ += size;
		return value;
	}
	std::span<const uint8_t> bytes(uint64_t size)
	{
		need(size);
		const std::span<const uint8_t> taken = bytes_.subspan(at_, static_cast<size_t>(size));
		at_ += static_cast<size_t>(size);
		return taken;
	}
	void skip(size_t size)
	{
		need(size);
		at_ += size;
	}
	size_t left() const noexcept { return bytes_.size() - at_; }
	std::span<const uint8_t> rest() const noexcept { return bytes_.subspan(at_); }

private:
	void need(uint64_t size) const
	{
		if (size > left()) {
			throw std::runtime_error("an MP4 box is cut short");
		}
	}

	std::span<const uint8_t> bytes_;
	size_t at_ = 0;
};

struct Box {
	std::string type;
	std::span<const uint8_t> body;
};

std::vector<Box> boxesIn(std::span<const uint8_t> bytes)
{
	std::vector<Box> boxes;
	Fields fields(bytes);
	while (fields.left() > 0) {
		uint64_t size = fields.take(4);
		const std::span<const uint8_t> type = fields.bytes(4);
		uint64_t header = 8;
		if (size == 1) {
			size = fields.take(8);
			header = 16;
		} else if (size == 0) {
			size = fields.left() + header;
		}
		if (size < header) {
			throw std::runtime_error("an MP4 box has a size smaller than its header");
		}
		boxes.push_back({std::string(type.begin(), type.end()), fields.bytes(size - header)});
	}
	return boxes;
}

const Box *findBox(const std::vector<Box> &boxes, const std::string &type)
{
	for (const Box &box : boxes) {
		if (box.type == type) {
			return &box;
		}
	}
	return nullptr;
}

std::span<const uint8_t> child(std::span<const uint8_t> bytes, const std::string &type)
{
	const std::vector<Box> boxes = boxesIn(bytes);
	const Box *box = findBox(boxes, type);
	if (!box) {
		throw std::runtime_error("the MP4 has no " + type + " box");
	}
	return box->body;
}

void appendUnit(std::vector<uint8_t> &out, std::span<const uint8_t> unit)
{
	out.insert(out.end(), {0, 0, 0, 1});
	out.insert(out.end(), unit.begin(), unit.end());
}

// The parameter sets of an avcC or hvcC box in Annex B, and the size of the lengths in
// front of each NAL unit of the samples.
std::pair<std::vector<uint8_t>, size_t> parameterSets(const Box &box)
{
	Fields fields(box.body);
	std::vector<uint8_t> annexB;
	size_t lengthSize = 4;
	if (box.type == "avcC") {
		fields.skip(4);
		lengthSize = static_cast<size_t>(fields.take(1) & 3) + 1;
		const uint64_t sequenceSets = fields.take(1) & 0x1F;
		for (uint64_t i = 0; i < sequenceSets; ++i) {
			appendUnit(annexB, fields.bytes(fields.take(2)));
		}
		const uint64_t pictureSets = fields.take(1);
		for (uint64_t i = 0; i < pictureSets; ++i) {
			appendUnit(annexB, fields.bytes(fields.take(2)));
		}
	} else {
		fields.skip(21);
		lengthSize = static_cast<size_t>(fields.take(1) & 3) + 1;
		const uint64_t arrays = fields.take(1);
		for (uint64_t a = 0; a < arrays; ++a) {
			fields.skip(1);
			const uint64_t units = fields.take(2);
			for (uint64_t i = 0; i < units; ++i) {
				appendUnit(annexB, fields.bytes(fields.take(2)));
			}
		}
	}
	return {std::move(annexB), lengthSize};
}

Mp4SampleEntry sampleEntry(const Box &box)
{
	Mp4SampleEntry entry;
	entry.type = box.type;
	Fields fields(box.body);
	fields.skip(24);
	entry.width = static_cast<uint16_t>(fields.take(2));
	entry.height = static_cast<uint16_t>(fields.take(2));
	fields.skip(50);
	const std::vector<Box> children = boxesIn(fields.rest());
	const Box *config = findBox(children, box.type == "avc1" ? "avcC" : "hvcC");
	if (!config) {
		throw std::runtime_error("a sample entry has no configuration box");
	}
	std::tie(entry.config, entry.lengthSize) = parameterSets(*config);
	return entry;
}

std::vector<uint8_t> annexBOf(std::span<const uint8_t> sample, size_t lengthSize)
{
	std::vector<uint8_t> annexB;
	Fields fields(sample);
	while (fields.left() > 0) {
		appendUnit(annexB, fields.bytes(fields.take(lengthSize)));
	}
	return annexB;
}

} // namespace

Mp4Track readMp4(std::span<const uint8_t> file)
{
	Mp4Track track;
	const std::span<const uint8_t> moov = child(file, "moov");
	{
		Fields mvhd(child(moov, "mvhd"));
		const uint64_t version = mvhd.take(1);
		mvhd.skip(3 + (version == 1 ? 16 : 8));
		track.movieTimescale = static_cast<uint32_t>(mvhd.take(4));
	}
	const std::span<const uint8_t> trak = child(moov, "trak");
	const std::vector<Box> trakBoxes = boxesIn(trak);
	if (const Box *edts = findBox(trakBoxes, "edts")) {
		Fields elst(child(edts->body, "elst"));
		const uint64_t version = elst.take(1);
		elst.skip(3);
		const uint64_t count = elst.take(4);
		for (uint64_t i = 0; i < count; ++i) {
			Mp4Edit edit;
			edit.duration = static_cast<int64_t>(elst.take(version == 1 ? 8 : 4));
			edit.mediaTime = version == 1 ? static_cast<int64_t>(elst.take(8))
						      : static_cast<int32_t>(static_cast<uint32_t>(elst.take(4)));
			elst.skip(4);
			track.edits.push_back(edit);
		}
	}
	const std::span<const uint8_t> mdia = child(trak, "mdia");
	{
		Fields mdhd(child(mdia, "mdhd"));
		const uint64_t version = mdhd.take(1);
		mdhd.skip(3 + (version == 1 ? 16 : 8));
		track.timescale = static_cast<uint32_t>(mdhd.take(4));
	}
	const std::span<const uint8_t> stbl = child(child(mdia, "minf"), "stbl");
	const std::vector<Box> tables = boxesIn(stbl);

	Fields stsd(child(stbl, "stsd"));
	stsd.skip(8);
	for (const Box &box : boxesIn(stsd.rest())) {
		track.entries.push_back(sampleEntry(box));
	}

	std::vector<int64_t> dts;
	Fields stts(child(stbl, "stts"));
	stts.skip(4);
	int64_t time = 0;
	for (uint64_t entries = stts.take(4); entries > 0; --entries) {
		const uint64_t count = stts.take(4);
		const int64_t delta = static_cast<int64_t>(stts.take(4));
		for (uint64_t i = 0; i < count; ++i) {
			dts.push_back(time);
			time += delta;
		}
	}
	std::vector<int64_t> offsets(dts.size(), 0);
	if (const Box *ctts = findBox(tables, "ctts")) {
		Fields fields(ctts->body);
		fields.skip(4);
		size_t sample = 0;
		for (uint64_t entries = fields.take(4); entries > 0; --entries) {
			const uint64_t count = fields.take(4);
			const int64_t offset = static_cast<int32_t>(static_cast<uint32_t>(fields.take(4)));
			for (uint64_t i = 0; i < count && sample < offsets.size(); ++i) {
				offsets[sample++] = offset;
			}
		}
	}
	std::vector<bool> sync(dts.size(), true);
	if (const Box *stss = findBox(tables, "stss")) {
		sync.assign(dts.size(), false);
		Fields fields(stss->body);
		fields.skip(4);
		for (uint64_t entries = fields.take(4); entries > 0; --entries) {
			const uint64_t number = fields.take(4);
			if (number == 0 || number > sync.size()) {
				throw std::runtime_error("a sync sample is out of range");
			}
			sync[number - 1] = true;
		}
	}

	Fields stsz(child(stbl, "stsz"));
	stsz.skip(4);
	const uint64_t commonSize = stsz.take(4);
	std::vector<uint64_t> sizes(static_cast<size_t>(stsz.take(4)), commonSize);
	if (commonSize == 0) {
		for (uint64_t &size : sizes) {
			size = stsz.take(4);
		}
	}
	if (sizes.size() != dts.size()) {
		throw std::runtime_error("the MP4's tables disagree on the number of samples");
	}

	std::vector<uint64_t> chunks;
	const bool wide = findBox(tables, "co64") != nullptr;
	Fields stco(child(stbl, wide ? "co64" : "stco"));
	stco.skip(4);
	for (uint64_t entries = stco.take(4); entries > 0; --entries) {
		chunks.push_back(stco.take(wide ? 8 : 4));
	}
	struct ChunkRun {
		uint64_t firstChunk = 0;
		uint64_t samples = 0;
		uint32_t entry = 0;
	};
	std::vector<ChunkRun> runs;
	Fields stsc(child(stbl, "stsc"));
	stsc.skip(4);
	for (uint64_t entries = stsc.take(4); entries > 0; --entries) {
		ChunkRun run;
		run.firstChunk = stsc.take(4);
		run.samples = stsc.take(4);
		run.entry = static_cast<uint32_t>(stsc.take(4));
		runs.push_back(run);
	}

	size_t sample = 0;
	size_t run = 0;
	for (uint64_t chunk = 1; chunk <= chunks.size(); ++chunk) {
		while (run + 1 < runs.size() && runs[run + 1].firstChunk <= chunk) {
			++run;
		}
		if (runs.empty() || runs[run].entry == 0 || runs[run].entry > track.entries.size()) {
			throw std::runtime_error("a chunk names no sample entry");
		}
		uint64_t at = chunks[chunk - 1];
		for (uint64_t i = 0; i < runs[run].samples && sample < sizes.size(); ++i, ++sample) {
			if (at > file.size() || sizes[sample] > file.size() - at) {
				throw std::runtime_error("a sample lies outside the file");
			}
			const Mp4SampleEntry &entry = track.entries[runs[run].entry - 1];
			Mp4Sample read;
			read.data = annexBOf(file.subspan(static_cast<size_t>(at), static_cast<size_t>(sizes[sample])),
					     entry.lengthSize);
			read.entry = runs[run].entry;
			read.dts = dts[sample];
			read.pts = dts[sample] + offsets[sample];
			read.sync = sync[sample];
			track.samples.push_back(std::move(read));
			at += sizes[sample];
		}
	}
	if (sample != sizes.size()) {
		throw std::runtime_error("the MP4's chunks hold fewer samples than its tables");
	}
	return track;
}

std::vector<std::vector<uint8_t>> annexBUnits(std::span<const uint8_t> packet)
{
	std::vector<std::vector<uint8_t>> units;
	size_t start = std::string::npos;
	size_t at = 0;
	const auto close = [&](size_t end) {
		if (start != std::string::npos && end > start) {
			units.emplace_back(packet.begin() + static_cast<ptrdiff_t>(start),
					   packet.begin() + static_cast<ptrdiff_t>(end));
		}
	};
	while (at + 3 <= packet.size()) {
		if (packet[at] == 0 && packet[at + 1] == 0 && packet[at + 2] == 1) {
			// The zero in front of a four-byte start code is not part of the unit before.
			close(at > 0 && packet[at - 1] == 0 ? at - 1 : at);
			at += 3;
			start = at;
		} else {
			++at;
		}
	}
	close(packet.size());
	return units;
}

} // namespace tapeloop::test
