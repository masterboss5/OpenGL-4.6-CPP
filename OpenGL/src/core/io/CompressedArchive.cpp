#include "CompressedArchive.h"

#include "SecurePath.h"
#include "src/util/UUID.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <limits>

namespace core::io
{
namespace
{
constexpr std::array<uint8, 8> Magic{'O', 'G', 'L', 'C', 'O', 'O', 'K', 0};
constexpr usize HeaderSize = Magic.size() + sizeof(uint32) * 2U + sizeof(uint64) * 3U;
constexpr uint32 CompressionZstandard = 1;
constexpr usize StreamBufferBytes = 1U * 1'024U * 1'024U;

template <std::unsigned_integral ValueType> void AppendLittleEndian(std::vector<uint8> &Destination, const ValueType Value)
{
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Destination.push_back(static_cast<uint8>(Value >> (ByteIndex * 8U)));
}

template <std::unsigned_integral ValueType> [[nodiscard]] ValueType ReadLittleEndian(const std::span<const uint8> Source, usize &Offset)
{
	if (Source.size() - std::min(Source.size(), Offset) < sizeof(ValueType))
		throw CompressedArchiveException("Compressed archive header is truncated");
	ValueType Value = 0;
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Value |= static_cast<ValueType>(Source[Offset++]) << (ByteIndex * 8U);
	return Value;
}

[[nodiscard]] uint64 UpdateChecksum(uint64 Hash, const std::span<const uint8> Bytes) noexcept
{
	for (const uint8 Byte : Bytes)
	{
		Hash ^= Byte;
		Hash *= 1'099'511'628'211ULL;
	}
	return Hash;
}

} // namespace

std::vector<uint8> CompressedArchive::Encode(const std::span<const uint8> Bytes, const int32 CompressionLevel)
{
	if (Bytes.size() > static_cast<usize>(std::numeric_limits<uint64>::max()))
		throw CompressedArchiveException("Decoded archive payload is too large");
	const usize Bound = ZSTD_compressBound(Bytes.size());
	std::vector<uint8> Compressed(Bound);
	const usize CompressedSize = ZSTD_compress(Compressed.data(), Compressed.size(), Bytes.data(), Bytes.size(), CompressionLevel);
	if (ZSTD_isError(CompressedSize) != 0)
		throw CompressedArchiveException("Zstandard compression failed: " + string(ZSTD_getErrorName(CompressedSize)));
	Compressed.resize(CompressedSize);

	std::vector<uint8> Archive;
	Archive.reserve(HeaderSize + Compressed.size());
	Archive.insert(Archive.end(), Magic.begin(), Magic.end());
	AppendLittleEndian(Archive, FormatVersion);
	AppendLittleEndian(Archive, CompressionZstandard);
	AppendLittleEndian(Archive, static_cast<uint64>(Bytes.size()));
	AppendLittleEndian(Archive, static_cast<uint64>(Compressed.size()));
	AppendLittleEndian(Archive, CompressedArchive::CalculateChecksum(Bytes));
	Archive.insert(Archive.end(), Compressed.begin(), Compressed.end());
	return Archive;
}

CompressedArchiveEncodeResult CompressedArchive::EncodeStream(const uint64 SourceBytes, const CompressedArchiveRangeReader &Reader,
															  const CompressedArchiveRangeWriter &Writer, const int32 CompressionLevel)
{
	if (!Reader || !Writer)
		throw CompressedArchiveException("Compressed archive streaming encode requires valid reader and writer callbacks");
	ZSTD_CCtx *Context = ZSTD_createCCtx();
	if (Context == nullptr)
		throw CompressedArchiveException("Could not allocate a Zstandard streaming compression context");
	struct ContextScope final
	{
		ZSTD_CCtx *Value;
		~ContextScope()
		{
			(void)ZSTD_freeCCtx(this->Value);
		}
	} ContextGuard{Context};
	if (ZSTD_isError(ZSTD_CCtx_setParameter(Context, ZSTD_c_compressionLevel, CompressionLevel)) != 0 ||
		ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(Context, SourceBytes)) != 0)
	{
		throw CompressedArchiveException("Could not configure the Zstandard streaming compression context");
	}
	std::vector<uint8> InputBytes(static_cast<usize>(std::min<uint64>(StreamBufferBytes, std::max<uint64>(SourceBytes, 1U))));
	std::vector<uint8> OutputBytes(StreamBufferBytes);
	uint64 SourceOffset = 0;
	uint64 CompressedOffset = 0;
	uint64 Checksum = 14'695'981'039'346'656'037ULL;
	auto FlushOutput = [&Writer, &OutputBytes, &CompressedOffset](const usize OutputSize)
	{
		if (OutputSize == 0)
			return;
		if (CompressedOffset > std::numeric_limits<uint64>::max() - HeaderSize ||
			OutputSize > std::numeric_limits<uint64>::max() - HeaderSize - CompressedOffset)
		{
			throw CompressedArchiveException("Compressed archive output size overflowed");
		}
		Writer(HeaderSize + CompressedOffset, std::span<const uint8>(OutputBytes.data(), OutputSize));
		CompressedOffset += OutputSize;
	};
	while (SourceOffset < SourceBytes)
	{
		const usize InputCount = static_cast<usize>(std::min<uint64>(InputBytes.size(), SourceBytes - SourceOffset));
		Reader(SourceOffset, std::span(InputBytes.data(), InputCount));
		const std::span<const uint8> SourceChunk(InputBytes.data(), InputCount);
		Checksum = UpdateChecksum(Checksum, SourceChunk);
		ZSTD_inBuffer Input{InputBytes.data(), InputCount, 0};
		while (Input.pos < Input.size)
		{
			ZSTD_outBuffer Output{OutputBytes.data(), OutputBytes.size(), 0};
			const usize PreviousInput = Input.pos;
			const usize Remaining = ZSTD_compressStream2(Context, &Output, &Input, ZSTD_e_continue);
			if (ZSTD_isError(Remaining) != 0)
				throw CompressedArchiveException("Zstandard streaming compression failed: " + string(ZSTD_getErrorName(Remaining)));
			FlushOutput(Output.pos);
			if (Input.pos == PreviousInput && Output.pos == 0)
				throw CompressedArchiveException("Zstandard stream made no compression progress");
		}
		SourceOffset += InputCount;
	}
	ZSTD_inBuffer EmptyInput{nullptr, 0, 0};
	usize Remaining = 1;
	while (Remaining != 0)
	{
		ZSTD_outBuffer Output{OutputBytes.data(), OutputBytes.size(), 0};
		Remaining = ZSTD_compressStream2(Context, &Output, &EmptyInput, ZSTD_e_end);
		if (ZSTD_isError(Remaining) != 0)
			throw CompressedArchiveException("Zstandard streaming finalization failed: " + string(ZSTD_getErrorName(Remaining)));
		FlushOutput(Output.pos);
		if (Remaining != 0 && Output.pos == 0)
			throw CompressedArchiveException("Zstandard stream made no finalization progress");
	}
	std::vector<uint8> Header;
	Header.reserve(HeaderSize);
	Header.insert(Header.end(), Magic.begin(), Magic.end());
	AppendLittleEndian(Header, FormatVersion);
	AppendLittleEndian(Header, CompressionZstandard);
	AppendLittleEndian(Header, SourceBytes);
	AppendLittleEndian(Header, CompressedOffset);
	AppendLittleEndian(Header, Checksum);
	Writer(0, Header);
	return {.ArchiveBytes = HeaderSize + CompressedOffset,
			.SourceBytes = SourceBytes,
			.CompressedBytes = CompressedOffset,
			.Checksum = Checksum};
}

std::vector<uint8> CompressedArchive::Decode(const std::span<const uint8> Archive, const uint64 MaximumDecodedBytes,
											 const uint64 MaximumCompressedBytes, const uint32 MaximumCompressionRatio)
{
	std::vector<uint8> Decoded;
	const CompressedArchiveDecodeResult Result = CompressedArchive::DecodeStream(
		static_cast<uint64>(Archive.size()),
		[&Archive](const uint64 Offset, const std::span<uint8> Destination)
		{
			if (Offset > Archive.size() || Destination.size() > Archive.size() - static_cast<usize>(Offset))
				throw CompressedArchiveException("Compressed archive reader exceeded the source range");
			std::ranges::copy(Archive.subspan(static_cast<usize>(Offset), Destination.size()), Destination.begin());
		},
		[&Decoded](const std::span<const uint8> Chunk) { Decoded.insert(Decoded.end(), Chunk.begin(), Chunk.end()); }, MaximumDecodedBytes,
		MaximumCompressedBytes, MaximumCompressionRatio);
	if (Result.DecodedBytes > static_cast<uint64>(std::numeric_limits<usize>::max()))
		throw CompressedArchiveException("Decoded archive payload is outside the addressable limit");
	if (Decoded.size() != static_cast<usize>(Result.DecodedBytes))
		throw CompressedArchiveException("Decoded archive payload assembly failed");
	return Decoded;
}

CompressedArchiveDecodeResult CompressedArchive::DecodeStream(const uint64 ArchiveBytes, const CompressedArchiveRangeReader &Reader,
															  const CompressedArchiveChunkVisitor &Visitor,
															  const uint64 MaximumDecodedBytes, const uint64 MaximumCompressedBytes,
															  const uint32 MaximumCompressionRatio)
{
	if (!Reader || !Visitor || MaximumDecodedBytes == 0 || MaximumCompressedBytes == 0 || MaximumCompressionRatio == 0)
		throw CompressedArchiveException("Compressed archive stream and budgets must be valid");
	if (ArchiveBytes < HeaderSize || ArchiveBytes > MaximumCompressedBytes)
		throw CompressedArchiveException("Compressed archive exceeds the configured encoded-size limit");
	std::array<uint8, HeaderSize> Header{};
	Reader(0, Header);
	if (!std::equal(Magic.begin(), Magic.end(), Header.begin()))
		throw CompressedArchiveException("Compressed archive magic is invalid");
	usize Offset = Magic.size();
	const uint32 Version = ReadLittleEndian<uint32>(Header, Offset);
	const uint32 Compression = ReadLittleEndian<uint32>(Header, Offset);
	const uint64 DecodedSize = ReadLittleEndian<uint64>(Header, Offset);
	const uint64 CompressedSize = ReadLittleEndian<uint64>(Header, Offset);
	const uint64 ExpectedChecksum = ReadLittleEndian<uint64>(Header, Offset);
	if (Version != FormatVersion)
		throw CompressedArchiveException("Compressed archive version is unsupported");
	if (Compression != CompressionZstandard)
		throw CompressedArchiveException("Compressed archive uses an unsupported compression codec");
	if (DecodedSize > MaximumDecodedBytes || CompressedSize > MaximumCompressedBytes - HeaderSize ||
		CompressedSize != ArchiveBytes - HeaderSize)
		throw CompressedArchiveException("Compressed archive sizes exceed their configured boundaries");
	constexpr uint64 RatioSlackBytes = 1ULL * 1'024ULL * 1'024ULL;
	const uint64 RatioLimit = CompressedSize > (std::numeric_limits<uint64>::max() - RatioSlackBytes) / MaximumCompressionRatio
								  ? std::numeric_limits<uint64>::max()
								  : CompressedSize * MaximumCompressionRatio + RatioSlackBytes;
	if (DecodedSize > RatioLimit)
		throw CompressedArchiveException("Compressed archive exceeds the configured compression-ratio limit");

	ZSTD_DCtx *Context = ZSTD_createDCtx();
	if (Context == nullptr)
		throw CompressedArchiveException("Could not allocate a Zstandard streaming decompression context");
	struct ContextScope final
	{
		ZSTD_DCtx *Value;
		~ContextScope()
		{
			(void)ZSTD_freeDCtx(this->Value);
		}
	} ContextGuard{Context};
	std::vector<uint8> InputBytes(static_cast<usize>(std::min<uint64>(StreamBufferBytes, std::max<uint64>(CompressedSize, 1U))));
	std::vector<uint8> OutputBytes(StreamBufferBytes);
	uint64 InputOffset = 0;
	uint64 TotalDecoded = 0;
	uint64 Checksum = 14'695'981'039'346'656'037ULL;
	usize FrameRemaining = 1;
	while (InputOffset < CompressedSize)
	{
		const usize InputCount = static_cast<usize>(std::min<uint64>(InputBytes.size(), CompressedSize - InputOffset));
		Reader(HeaderSize + InputOffset, std::span(InputBytes.data(), InputCount));
		ZSTD_inBuffer Input{InputBytes.data(), InputCount, 0};
		while (Input.pos < Input.size)
		{
			ZSTD_outBuffer Output{OutputBytes.data(), OutputBytes.size(), 0};
			const usize PreviousInput = Input.pos;
			FrameRemaining = ZSTD_decompressStream(Context, &Output, &Input);
			if (ZSTD_isError(FrameRemaining) != 0)
				throw CompressedArchiveException("Zstandard streaming decompression failed: " + string(ZSTD_getErrorName(FrameRemaining)));
			if (Input.pos == PreviousInput && Output.pos == 0)
				throw CompressedArchiveException("Zstandard stream made no decompression progress");
			if (Output.pos != 0)
			{
				if (Output.pos > DecodedSize - std::min<uint64>(DecodedSize, TotalDecoded))
					throw CompressedArchiveException("Zstandard stream exceeds its declared decoded size");
				const std::span<const uint8> Chunk(OutputBytes.data(), Output.pos);
				Checksum = UpdateChecksum(Checksum, Chunk);
				Visitor(Chunk);
				TotalDecoded += Output.pos;
			}
			if (FrameRemaining == 0 && (Input.pos != Input.size || InputOffset + InputCount != CompressedSize))
				throw CompressedArchiveException("Compressed archive contains trailing stream data");
		}
		InputOffset += InputCount;
	}
	if (FrameRemaining != 0 || TotalDecoded != DecodedSize)
		throw CompressedArchiveException("Zstandard decoded size does not match the archive header");
	if (Checksum != ExpectedChecksum)
		throw CompressedArchiveException("Compressed archive checksum validation failed");
	return {.DecodedBytes = TotalDecoded, .Checksum = Checksum};
}

void CompressedArchive::Save(const std::filesystem::path &Path, const std::span<const uint8> Bytes, const int32 CompressionLevel)
{
	if (Path.empty())
		throw CompressedArchiveException("Compressed archive path cannot be empty");
	const std::vector<uint8> Archive = CompressedArchive::Encode(Bytes, CompressionLevel);
	const std::filesystem::path Root = std::filesystem::absolute(Path.parent_path()).lexically_normal();
	const std::filesystem::path Destination = Path.filename();
	const std::filesystem::path Temporary = Destination.string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	try
	{
		SecurePath::CreateTrustedRoot(Root, "compressed archive root");
		SecurePath::WriteFileWithin(Root, Temporary, Archive, false, true, "compressed archive temporary file");
		SecurePath::ReplaceWithin(Root, Temporary, Destination, "compressed archive publication");
	}
	catch (const SecurePathException &Exception)
	{
		try
		{
			SecurePath::RemoveWithin(Root, Temporary, false, "compressed archive temporary cleanup");
		}
		catch (...)
		{
		}
		throw CompressedArchiveException("Could not securely publish compressed archive '" + Path.string() + "': " + Exception.what());
	}
}

std::vector<uint8> CompressedArchive::Load(const std::filesystem::path &Path, const uint64 MaximumDecodedBytes,
										   const uint64 MaximumCompressedBytes, const uint32 MaximumCompressionRatio)
{
	if (Path.empty())
		throw CompressedArchiveException("Compressed archive path cannot be empty");
	const std::filesystem::path Root = std::filesystem::absolute(Path.parent_path()).lexically_normal();
	try
	{
		return CompressedArchive::Decode(SecurePath::ReadFileWithin(Root, Path.filename(), MaximumCompressedBytes, "compressed archive"),
										 MaximumDecodedBytes, MaximumCompressedBytes, MaximumCompressionRatio);
	}
	catch (const SecurePathException &Exception)
	{
		throw CompressedArchiveException("Could not securely read compressed archive '" + Path.string() + "': " + Exception.what());
	}
}

uint64 CompressedArchive::CalculateChecksum(const std::span<const uint8> Bytes) noexcept
{
	return UpdateChecksum(14'695'981'039'346'656'037ULL, Bytes);
}
} // namespace core::io
