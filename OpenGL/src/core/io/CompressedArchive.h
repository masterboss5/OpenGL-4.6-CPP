#pragma once

#include "src/types.h"

#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace core::io
{
using CompressedArchiveRangeReader = std::function<void(uint64, std::span<uint8>)>;
using CompressedArchiveRangeWriter = std::function<void(uint64, std::span<const uint8>)>;
using CompressedArchiveChunkVisitor = std::function<void(std::span<const uint8>)>;

struct CompressedArchiveEncodeResult final
{
	uint64 ArchiveBytes = 0;
	uint64 SourceBytes = 0;
	uint64 CompressedBytes = 0;
	uint64 Checksum = 0;
};

struct CompressedArchiveDecodeResult final
{
	uint64 DecodedBytes = 0;
	uint64 Checksum = 0;
};

class ENGINE_API CompressedArchiveException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API CompressedArchive final
{
  public:
	static constexpr uint32 FormatVersion = 1;

	[[nodiscard]] static std::vector<uint8> Encode(std::span<const uint8> Bytes, int32 CompressionLevel = 12);
	[[nodiscard]] static CompressedArchiveEncodeResult EncodeStream(uint64 SourceBytes, const CompressedArchiveRangeReader &Reader,
																	const CompressedArchiveRangeWriter &Writer,
																	int32 CompressionLevel = 12);
	[[nodiscard]] static std::vector<uint8> Decode(std::span<const uint8> Archive,
												   uint64 MaximumDecodedBytes = 4ULL * 1'024ULL * 1'024ULL * 1'024ULL,
												   uint64 MaximumCompressedBytes = 1ULL * 1'024ULL * 1'024ULL * 1'024ULL,
												   uint32 MaximumCompressionRatio = 1'024U);
	[[nodiscard]] static CompressedArchiveDecodeResult DecodeStream(uint64 ArchiveBytes, const CompressedArchiveRangeReader &Reader,
																	const CompressedArchiveChunkVisitor &Visitor,
																	uint64 MaximumDecodedBytes = 4ULL * 1'024ULL * 1'024ULL * 1'024ULL,
																	uint64 MaximumCompressedBytes = 1ULL * 1'024ULL * 1'024ULL * 1'024ULL,
																	uint32 MaximumCompressionRatio = 1'024U);
	static void Save(const std::filesystem::path &Path, std::span<const uint8> Bytes, int32 CompressionLevel = 12);
	[[nodiscard]] static std::vector<uint8> Load(const std::filesystem::path &Path,
												 uint64 MaximumDecodedBytes = 4ULL * 1'024ULL * 1'024ULL * 1'024ULL,
												 uint64 MaximumCompressedBytes = 1ULL * 1'024ULL * 1'024ULL * 1'024ULL,
												 uint32 MaximumCompressionRatio = 1'024U);
	[[nodiscard]] static uint64 CalculateChecksum(std::span<const uint8> Bytes) noexcept;
};
} // namespace core::io
