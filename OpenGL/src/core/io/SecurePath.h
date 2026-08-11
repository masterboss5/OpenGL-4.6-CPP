#pragma once

#include "src/core/EngineAPI.h"
#include "src/types.h"

#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace core::io
{
using SecureFileChunkVisitor = std::function<void(std::span<const uint8>)>;

class ENGINE_API SecurePathException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API SecurePath final
{
  public:
	[[nodiscard]] static std::filesystem::path ResolveWithin(const std::filesystem::path &TrustedRoot,
															 const std::filesystem::path &RelativePath, string_view Role);
	static void VerifyContained(const std::filesystem::path &TrustedRoot, const std::filesystem::path &Candidate, string_view Role);
	static void CreateTrustedRoot(const std::filesystem::path &TrustedRoot, string_view Role);
	static void CreateDirectoriesWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
										string_view Role);
	static void CopyWithin(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
						   const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative, bool Recursive,
						   bool ReplaceExisting, string_view Role);
	static void MoveWithin(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
						   const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative,
						   bool ReplaceExisting, string_view Role);
	static void RemoveWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath, bool Recursive,
							 string_view Role);
	static void ReplaceWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &TemporaryRelative,
							  const std::filesystem::path &DestinationRelative, string_view Role);
	[[nodiscard]] static std::vector<uint8> ReadFileWithin(const std::filesystem::path &TrustedRoot,
														   const std::filesystem::path &RelativePath, uint64 MaximumBytes,
														   string_view Role);
	[[nodiscard]] static uint64 ReadFileChunksWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
													 uint64 MaximumBytes, const SecureFileChunkVisitor &Visitor, string_view Role);
	[[nodiscard]] static std::vector<uint8> ReadFileRangeWithin(const std::filesystem::path &TrustedRoot,
																const std::filesystem::path &RelativePath, uint64 Offset, uint64 ByteCount,
																uint64 MaximumFileBytes, string_view Role);
	static void WriteFileWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
								std::span<const uint8> Bytes, bool ReplaceExisting, bool Durable, string_view Role);
	static void WriteFileAtWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath, uint64 Offset,
								  std::span<const uint8> Bytes, bool Durable, string_view Role);
};
} // namespace core::io
