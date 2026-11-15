// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayWriter.hpp"

#include "core/ReplayExport.hpp"
#include "core/ReplayNames.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace tapeloop {
namespace {

constexpr std::string_view kManifestExtension = ".tplp";
constexpr std::string_view kPartExtension = ".tplp.part";
constexpr std::string_view kDataFolder = "data";
// How many numbered names a manifest tries before the capture fails.
constexpr int kMaxNameTries = 1000;
// Larger indexes are not read: the index of eight sources over ten minutes is a few MiB.
constexpr uint64_t kMaxIndexBytes = uint64_t{256} << 20;
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

void removeHalfWritten(const std::filesystem::path &path, ReplayScan &scan)
{
	std::error_code error;
	if (std::filesystem::remove(path, error)) {
		scan.removed.push_back(path);
	} else if (error) {
		scan.errors.push_back(describe("cannot delete", path, error));
	}
}

// What a manifest or a self-contained replay holds besides GOPs: its head, its index
// chunk and its footer. The head is empty when the footer is not intact.
struct ManifestBytes {
	std::vector<uint8_t> head;
	std::vector<uint8_t> index;
	std::vector<uint8_t> footer;
	uint64_t size = 0;
};

ManifestBytes readManifestBytes(File &file)
{
	ManifestBytes bytes;
	bytes.size = file.size();
	if (bytes.size < kManifestIndexOffset + kManifestFooterSize) {
		return bytes;
	}
	bytes.footer.resize(kManifestFooterSize);
	bytes.footer.resize(file.readAt(bytes.size - kManifestFooterSize, bytes.footer));
	const std::optional<IndexPlace> place = decodeManifestFooter(bytes.footer, bytes.size);
	if (!place || place->size > kMaxIndexBytes) {
		return bytes;
	}
	bytes.index.resize(static_cast<size_t>(place->size));
	bytes.index.resize(file.readAt(place->offset, bytes.index));
	bytes.head.resize(kManifestIndexOffset);
	bytes.head.resize(file.readAt(0, bytes.head));
	return bytes;
}

std::optional<ReplayIndex> decodeManifestBytes(const ManifestBytes &bytes)
{
	return decodeManifest(bytes.head, bytes.index, bytes.footer, bytes.size);
}

#ifdef TAPELOOP_COUNT_LOOKUPS
std::atomic<size_t> &lookupCount() noexcept
{
	static std::atomic<size_t> count{0};
	return count;
}

void countLookup() noexcept
{
	lookupCount().fetch_add(1, std::memory_order_relaxed);
}
#else
void countLookup() noexcept {}
#endif

bool segmentsExist(const std::filesystem::path &manifest, const ReplayIndex &index)
{
	for (const SegmentFile &file : segmentFilesOf(index)) {
		countLookup();
		std::error_code error;
		if (!std::filesystem::is_regular_file(segmentPath(manifest, index.sources[file.source], file.segment),
						      error)) {
			return false;
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
		File file(manifest, File::Mode::ReadOnly);
		const ManifestBytes bytes = readManifestBytes(file);
		const std::optional<ReplayIndex> index = decodeManifestBytes(bytes);
		if (!index || !segmentsExist(manifest, *index)) {
			return found;
		}
		for (const StoredSource &source : index->sources) {
			found.sources.push_back({source.key, source.name});
		}
		found.tags = decodeTags(bytes.head);
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

void ReplayFiles::set(std::filesystem::path manifest, std::shared_ptr<const ReplayIndex> index)
{
	std::shared_ptr<const Files> files = std::make_shared<Files>(Files{std::move(manifest), std::move(index)});
	const std::lock_guard lock(mutex_);
	files_ = std::move(files);
}

std::shared_ptr<const ReplayFiles::Files> ReplayFiles::get() const
{
	const std::lock_guard lock(mutex_);
	return files_;
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
	if (!tagsFit(capture.tags)) {
		throw std::length_error("the tags of a replay do not fit its file");
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
	const ManifestBytes bytes = readManifestBytes(file);
	if (!decodeManifestBytes(bytes)) {
		throw std::invalid_argument("not an intact replay manifest: " + utf8FromPath(manifest));
	}
	const TagWrite next = nextTagWrite(bytes.head);
	const std::vector<uint8_t> slot = encodeTagSlot(tags, next.generation);
	file.writeAt(next.offset, slot);
	file.flush();
	if (next.then) {
		// The older slot is on the disk before the newer one is overwritten, so that a cut
		// between the writes leaves the newer slot's tags. The other order, or no flush on
		// a disk that reorders writes, could leave the older slot's instead, from before
		// the last edit.
		file.writeAt(*next.then, slot);
		file.flush();
	}
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
		const std::string suffix = number > 1 ? " (" + std::to_string(number) + ")" : std::string();
		const std::string name = safeFileName(stem, kMaxFileNameBytes - suffix.size()) + suffix;
		const std::filesystem::path manifest = folder_ / pathFromUtf8(name + std::string(kManifestExtension));
		const std::filesystem::path part = folder_ / pathFromUtf8(name + std::string(kPartExtension));
		if (nameTaken(manifest) || nameTaken(part)) {
			continue;
		}
		// Another writer in the folder can take either name right after the check. The
		// part is created only if its name is still free, and deleted only once this call
		// has created it. The manifest gets its name from renameIfFree, which replaces
		// nothing there, except where the file system can neither rename exclusively nor
		// make hard links: then a file another writer creates between renameIfFree's look
		// at the name and its rename is replaced.
		File file;
		try {
			file = File(part, File::Mode::CreateNew);
		} catch (const std::system_error &e) {
			if (e.code() == std::errc::file_exists) {
				continue;
			}
			throw;
		}
		try {
			file.writeAt(0, bytes);
			file.flush();
			file.close();
			if (renameIfFree(part, manifest)) {
				return manifest;
			}
		} catch (...) {
			std::error_code ignored;
			std::filesystem::remove(part, ignored);
			throw;
		}
		std::error_code ignored;
		std::filesystem::remove(part, ignored);
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
		// A link or junction in place of a broadcast folder is listed, but nothing is
		// deleted through it, since it can lead to anyone's files.
		const bool linked = !std::filesystem::is_directory(std::filesystem::symlink_status(folder, error));
		const std::string broadcast = utf8FromPath(folder.filename());
		for (const std::filesystem::path &path : listDirectory(folder, scan.errors)) {
			const std::string name = utf8FromPath(path.filename());
			if (name.ends_with(kPartExtension)) {
				if (!linked) {
					removeHalfWritten(path, scan);
				}
			} else if (name.ends_with(kManifestExtension) &&
				   std::filesystem::is_regular_file(path, error)) {
				scan.replays.push_back(readReplay(path, broadcast, scan.errors));
			}
		}
		// An export a crash stopped left its file under a temporary name. A link in place
		// of the Export folder is not followed either.
		const std::filesystem::path exports = folder / pathFromUtf8(kExportFolderName);
		if (!linked && std::filesystem::is_directory(std::filesystem::symlink_status(exports, error))) {
			for (const std::filesystem::path &path : listDirectory(exports, scan.errors)) {
				std::error_code removeError;
				if (removeStaleExportPart(path, removeError)) {
					scan.removed.push_back(path);
				} else if (removeError) {
					scan.errors.push_back(describe("cannot delete", path, removeError));
				}
			}
		}
	}
	return scan;
}

std::optional<ReplayIndex> readReplayIndex(const std::filesystem::path &manifest)
{
	try {
		File file(manifest, File::Mode::ReadOnly);
		return decodeManifestBytes(readManifestBytes(file));
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

std::filesystem::path segmentPath(const std::filesystem::path &manifest, const StoredSource &source, uint32_t segment)
{
	if (segment == kOwnFile) {
		return manifest;
	}
	return manifest.parent_path() / pathFromUtf8(kDataFolder) / pathFromUtf8(segmentFileName(source.key, segment));
}

std::vector<SegmentFile> segmentFilesOf(const ReplayIndex &index)
{
	// A segment's file name is the safe form of its key, so keys that differ can share it.
	std::vector<std::pair<std::string, uint32_t>> names;
	names.reserve(index.sources.size());
	for (size_t s = 0; s < index.sources.size(); ++s) {
		names.emplace_back(safeFileName(index.sources[s].key), static_cast<uint32_t>(s));
	}
	std::sort(names.begin(), names.end());
	std::vector<uint32_t> namedBy(names.size());
	for (size_t i = 0; i < names.size(); ++i) {
		const bool shared = i > 0 && names[i].first == names[i - 1].first;
		namedBy[names[i].second] = shared ? namedBy[names[i - 1].second] : names[i].second;
	}

	std::vector<SegmentFile> files;
	for (size_t s = 0; s < index.sources.size(); ++s) {
		std::optional<uint32_t> last;
		for (const StoredGop &gop : index.sources[s].gops) {
			if (gop.segment != last) {
				last = gop.segment;
				// Every GOP of a self-contained replay is in the one file.
				files.push_back({gop.segment == kOwnFile ? 0 : namedBy[s], gop.segment});
			}
		}
	}
	std::sort(files.begin(), files.end(), [](const SegmentFile &a, const SegmentFile &b) {
		return std::tie(a.source, a.segment) < std::tie(b.source, b.segment);
	});
	files.erase(std::unique(files.begin(), files.end()), files.end());
	return files;
}

#ifdef TAPELOOP_COUNT_LOOKUPS
size_t segmentLookups() noexcept
{
	return lookupCount().load(std::memory_order_relaxed);
}
#endif

} // namespace tapeloop
