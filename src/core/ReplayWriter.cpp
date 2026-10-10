// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayWriter.hpp"

#include "core/ReplayNames.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>

namespace tapeloop {
namespace {

constexpr std::string_view kManifestExtension = ".tplp";
constexpr std::string_view kPartExtension = ".tplp.part";
constexpr std::string_view kDataFolder = "data";
// How many numbered names a manifest tries before the capture fails.
constexpr int kMaxNameTries = 1000;
// Larger files named as manifests are not read: a manifest of eight sources over ten
// minutes is a few MiB.
constexpr uint64_t kMaxManifestBytes = uint64_t{256} << 20;
// Long enough for any name OBS shows, short enough that the segment header always fits.
constexpr size_t kMaxSourceNameBytes = 1024;

// The text cut to at most size bytes, at a UTF-8 character boundary.
std::string shortened(const std::string &text, size_t size)
{
	if (text.size() <= size) {
		return text;
	}
	while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
		--size;
	}
	return text.substr(0, size);
}

std::string describe(std::string_view what, const std::filesystem::path &path, const std::error_code &error)
{
	std::string text(what);
	text += " ";
	text += utf8FromPath(path);
	text += ": ";
	text += error.message();
	return text;
}

std::vector<std::filesystem::path> listDirectory(const std::filesystem::path &directory,
						 std::vector<std::string> &errors)
{
	std::vector<std::filesystem::path> paths;
	std::error_code error;
	std::filesystem::directory_iterator it(directory, error);
	for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
		paths.push_back(it->path());
	}
	if (error) {
		errors.push_back(describe("cannot list", directory, error));
	}
	std::sort(paths.begin(), paths.end());
	return paths;
}

std::vector<uint8_t> readWhole(const std::filesystem::path &path)
{
	File file(path, File::Mode::ReadOnly);
	const uint64_t size = file.size();
	if (size > kMaxManifestBytes) {
		return {};
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	bytes.resize(file.readAt(0, bytes));
	return bytes;
}

bool segmentsExist(const std::filesystem::path &manifest, const ReplayIndex &index)
{
	for (const StoredSource &source : index.sources) {
		uint32_t checked = 0;
		for (const StoredGop &gop : source.gops) {
			if (gop.segment == checked) {
				continue;
			}
			checked = gop.segment;
			std::error_code error;
			if (!std::filesystem::is_regular_file(segmentPath(manifest, source, gop.segment), error)) {
				return false;
			}
		}
	}
	return true;
}

FoundReplay readReplay(const std::filesystem::path &manifest, const std::string &broadcast,
		       std::vector<std::string> &errors)
{
	FoundReplay found;
	found.manifest = manifest;
	found.broadcast = broadcast;
	try {
		const std::vector<uint8_t> bytes = readWhole(manifest);
		const std::optional<ReplayIndex> index = decodeManifest(bytes);
		if (!index || !segmentsExist(manifest, *index)) {
			return found;
		}
		for (const StoredSource &source : index->sources) {
			found.sources.push_back({source.key, source.name});
		}
		found.tags = decodeTags(bytes);
		found.id = index->id;
		found.capturedAtUtc = index->capturedAtUtc;
		found.intact = true;
	} catch (const std::exception &e) {
		errors.emplace_back(e.what());
		found.sources.clear();
		found.tags.clear();
	}
	return found;
}

// A source as its clip describes it, with no GOP placed in a segment yet.
StoredSource sourceIndexOf(const CaptureSource &source)
{
	StoredSource stored;
	stored.key = source.key;
	stored.name = shortened(source.name, kMaxSourceNameBytes);
	stored.in = source.clip.in();
	stored.out = source.clip.out();
	for (const std::shared_ptr<const Gop> &gop : source.clip.gops()) {
		const GopKey key = gopKeyOf(*gop);
		const auto run = std::find_if(stored.runs.begin(), stored.runs.end(), [&](const StoredRun &known) {
			return known.key == key.runKey && known.frameDuration == gop->frameDuration();
		});
		const auto runIndex = static_cast<uint32_t>(run - stored.runs.begin());
		if (run == stored.runs.end()) {
			const CodecConfig *config = gop->codecConfig();
			stored.runs.push_back({gop->codec(), gop->frameDuration(),
					       config ? std::make_shared<const CodecConfig>(*config) : nullptr,
					       key.runKey});
		}
		stored.gops.push_back({0, key.packetCount, 0, 0, 0, runIndex});
		for (const PacketRecord &packet : gop->packets()) {
			stored.frameTimes.push_back(packet.time);
		}
	}
	return stored;
}

bool hasRun(const std::vector<std::pair<uint32_t, Nanoseconds>> &runs, const std::pair<uint32_t, Nanoseconds> &run)
{
	return std::find(runs.begin(), runs.end(), run) != runs.end();
}

} // namespace

ReplayId newReplayId()
{
	std::random_device random;
	ReplayId id{};
	for (size_t i = 0; i < id.size(); i += 4) {
		const uint32_t value = random();
		for (size_t byte = 0; byte < 4; ++byte) {
			id[i + byte] = static_cast<uint8_t>(value >> (8 * byte));
		}
	}
	return id;
}

ReplayIndex indexOf(const ReplayCapture &capture)
{
	ReplayIndex index;
	index.id = capture.id;
	index.capturedAtUtc = capture.capturedAtUtc;
	index.capturedAtClock = capture.capturedAtClock;
	index.start = capture.start;
	index.end = capture.end;
	for (const CaptureSource &source : capture.sources) {
		if (!source.clip.empty()) {
			index.sources.push_back(sourceIndexOf(source));
		}
	}
	return index;
}

size_t ReplayWriter::GopKeyHash::operator()(const GopKey &key) const noexcept
{
	size_t hash = std::hash<uint64_t>{}(uint64_t{key.runKey} << 32 | key.packetCount);
	for (const int64_t value : {key.firstPts, key.firstTime.count()}) {
		hash ^= std::hash<int64_t>{}(value) + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
	}
	return hash;
}

ReplayWriter::ReplayWriter(ReplayWriterConfig config) : config_(config) {}

WrittenReplay ReplayWriter::write(const ReplayCapture &capture)
{
	if (std::none_of(capture.sources.begin(), capture.sources.end(),
			 [](const CaptureSource &source) { return !source.clip.empty(); })) {
		throw std::invalid_argument("a replay needs a source with frames");
	}
	if (capture.folder != folder_) {
		sources_.clear();
		folder_ = capture.folder;
		broadcast_ = newReplayId();
	}

	WrittenReplay written;
	written.index = indexOf(capture);
	appended_ = 0;
	try {
		prepareFolder();
		size_t stored = 0;
		for (const CaptureSource &source : capture.sources) {
			if (!source.clip.empty()) {
				writeSource(source, written.index.sources[stored++], written);
			}
		}
		flushSegments();
		written.bytesAppended = appended_;
		written.manifest = publishManifest(capture.stem, encodeManifest(written.index, capture.tags));
	} catch (...) {
		// What is on disk of these sources may be half written or not durable: later
		// captures start over in new segments.
		for (const CaptureSource &source : capture.sources) {
			sources_.erase(source.key);
		}
		retired_.clear();
		batch_.clear();
		batchGops_.clear();
		throw;
	}
	return written;
}

void ReplayWriter::writeTags(const std::filesystem::path &manifest, std::span<const std::string> tags)
{
	File file(manifest, File::Mode::ReadWrite);
	std::vector<uint8_t> bytes(static_cast<size_t>(std::min(file.size(), kMaxManifestBytes)));
	bytes.resize(file.readAt(0, bytes));
	if (!decodeManifest(bytes)) {
		throw std::invalid_argument("not an intact replay manifest: " + utf8FromPath(manifest));
	}
	const TagWrite next = nextTagWrite(bytes);
	file.writeAt(next.offset, encodeTagSlot(tags, next.generation));
	file.flush();
}

void ReplayWriter::prepareFolder()
{
	const std::filesystem::path data = folder_ / pathFromUtf8(kDataFolder);
	if (std::filesystem::is_directory(data)) {
		return;
	}
	const bool newFolder = !std::filesystem::exists(folder_);
	std::filesystem::create_directories(data);
	if (newFolder) {
		syncDirectory(folder_.parent_path());
	}
	syncDirectory(folder_);
}

ReplayWriter::SourceState &ReplayWriter::stateOf(const std::string &key)
{
	const auto found = sources_.find(key);
	if (found != sources_.end()) {
		return found->second;
	}
	// Segments of this source already in the folder are left as they are.
	SourceState state;
	std::vector<std::string> errors;
	for (const std::filesystem::path &path : listDirectory(folder_ / pathFromUtf8(kDataFolder), errors)) {
		if (const std::optional<uint32_t> sequence = segmentSequenceOf(utf8FromPath(path.filename()), key)) {
			state.nextSequence = std::max(state.nextSequence, *sequence + 1);
		}
	}
	if (!errors.empty()) {
		throw std::runtime_error(errors.front());
	}
	return sources_.emplace(key, std::move(state)).first->second;
}

void ReplayWriter::writeSource(const CaptureSource &source, StoredSource &stored, WrittenReplay &written)
{
	SourceState &state = stateOf(source.key);
	const std::span<const std::shared_ptr<const Gop>> gops = source.clip.gops();
	for (size_t i = 0; i < gops.size(); ++i) {
		const GopKey key = gopKeyOf(*gops[i]);
		Place place;
		if (const auto found = state.written.find(key); found != state.written.end()) {
			place = found->second;
			++written.gopsShared;
		} else {
			place = append(state, source, *gops[i], key);
			++written.gopsAppended;
		}
		StoredGop &gop = stored.gops[i];
		gop.segment = place.segment;
		gop.offset = place.offset;
		gop.size = place.size;
		gop.headerCrc = place.headerCrc;
	}
	writeBatch(state);
}

ReplayWriter::Place ReplayWriter::append(SourceState &state, const CaptureSource &source, const Gop &gop,
					 const GopKey &key)
{
	const std::pair<uint32_t, Nanoseconds> run{key.runKey, gop.frameDuration()};
	if (state.open) {
		const uint64_t end = state.open->end + batch_.size();
		const uint64_t needed = gopChunkSize(gop) + (hasRun(state.open->runs, run) ? 0 : runChunkSize(gop));
		if (end > kReplayAlignment && end + needed > config_.segmentBytes) {
			writeBatch(state);
			retire(state);
		}
	}
	Segment &segment = state.open ? *state.open : openSegment(state, source);
	if (!hasRun(segment.runs, run)) {
		appendRunChunk(batch_, segment.chunks++, gop);
		segment.runs.push_back(run);
	}
	const ChunkPlace chunk = appendGopChunk(batch_, segment.chunks++, gop);
	const Place place{segment.sequence, segment.end + chunk.offset, chunk.size, chunk.headerCrc};
	batchGops_.emplace_back(key, place);
	if (batch_.size() >= config_.writeBytes) {
		writeBatch(state);
	}
	return place;
}

void ReplayWriter::writeBatch(SourceState &state)
{
	if (batch_.empty()) {
		return;
	}
	if (!state.open) {
		throw std::logic_error("a batch is written to the segment it was gathered for");
	}
	Segment &segment = *state.open;
	segment.file.writeAt(segment.end, batch_);
	segment.end += batch_.size();
	segment.unflushed = true;
	appended_ += batch_.size();
	for (const auto &[key, place] : batchGops_) {
		state.written.emplace(key, place);
	}
	batch_.clear();
	batchGops_.clear();
}

ReplayWriter::Segment &ReplayWriter::openSegment(SourceState &state, const CaptureSource &source)
{
	Segment segment;
	segment.sequence = state.nextSequence++;
	const std::filesystem::path path =
		folder_ / pathFromUtf8(kDataFolder) / pathFromUtf8(segmentFileName(source.key, segment.sequence));
	segment.file = File(path, File::Mode::CreateNew);
	newSegments_ = true;
	const std::vector<uint8_t> header = encodeSegmentHeader(
		{broadcast_, segment.sequence, source.key, shortened(source.name, kMaxSourceNameBytes)});
	segment.file.writeAt(0, header);
	segment.end = header.size();
	segment.unflushed = true;
	appended_ += header.size();
	return state.open.emplace(std::move(segment));
}

void ReplayWriter::retire(SourceState &state)
{
	if (!state.open) {
		return;
	}
	if (state.open->unflushed) {
		retired_.push_back(std::move(state.open->file));
	}
	state.open.reset();
}

void ReplayWriter::flushSegments()
{
	for (File &file : retired_) {
		file.flush();
	}
	retired_.clear();
	for (auto &[key, state] : sources_) {
		if (state.open && state.open->unflushed) {
			state.open->file.flush();
			state.open->unflushed = false;
		}
	}
	if (newSegments_) {
		syncDirectory(folder_ / pathFromUtf8(kDataFolder));
		newSegments_ = false;
	}
}

std::filesystem::path ReplayWriter::publishManifest(const std::string &stem, std::span<const uint8_t> bytes)
{
	for (int number = 1; number <= kMaxNameTries; ++number) {
		std::string name = stem;
		if (number > 1) {
			name += " (" + std::to_string(number) + ")";
		}
		name = safeFileName(name);
		const std::filesystem::path manifest = folder_ / pathFromUtf8(name + std::string(kManifestExtension));
		const std::filesystem::path part = folder_ / pathFromUtf8(name + std::string(kPartExtension));
		if (std::filesystem::exists(manifest) || std::filesystem::exists(part)) {
			continue;
		}
		try {
			File file(part, File::Mode::CreateNew);
			file.writeAt(0, bytes);
			file.flush();
			file.close();
			renameFile(part, manifest);
		} catch (...) {
			std::error_code ignored;
			std::filesystem::remove(part, ignored);
			throw;
		}
		return manifest;
	}
	throw std::runtime_error("no free name for a replay in " + utf8FromPath(folder_));
}

ReplayScan scanReplays(const std::filesystem::path &base)
{
	ReplayScan scan;
	std::error_code error;
	if (!std::filesystem::is_directory(base, error)) {
		return scan;
	}
	for (const std::filesystem::path &folder : listDirectory(base, scan.errors)) {
		if (!std::filesystem::is_directory(folder, error)) {
			continue;
		}
		const std::string broadcast = utf8FromPath(folder.filename());
		for (const std::filesystem::path &path : listDirectory(folder, scan.errors)) {
			const std::string name = utf8FromPath(path.filename());
			if (name.ends_with(kPartExtension)) {
				if (std::filesystem::remove(path, error)) {
					scan.removed.push_back(path);
				} else if (error) {
					scan.errors.push_back(describe("cannot delete", path, error));
				}
			} else if (name.ends_with(kManifestExtension) &&
				   std::filesystem::is_regular_file(path, error)) {
				scan.replays.push_back(readReplay(path, broadcast, scan.errors));
			}
		}
	}
	return scan;
}

std::optional<ReplayIndex> readReplayIndex(const std::filesystem::path &manifest)
{
	try {
		return decodeManifest(readWhole(manifest));
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

std::filesystem::path segmentPath(const std::filesystem::path &manifest, const StoredSource &source, uint32_t segment)
{
	return manifest.parent_path() / pathFromUtf8(kDataFolder) / pathFromUtf8(segmentFileName(source.key, segment));
}

} // namespace tapeloop
