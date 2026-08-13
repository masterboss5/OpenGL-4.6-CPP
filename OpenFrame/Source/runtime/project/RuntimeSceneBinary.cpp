#include "RuntimeSceneBinary.h"

#include "Source/core/io/CompressedArchive.h"
#include "Source/util/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <limits>

namespace runtime::project
{
namespace
{
using Json = nlohmann::json;
constexpr std::array<uint8, 8> Magic{'O', 'G', 'L', 'S', 'C', 'E', 'N', 'E'};
constexpr usize HeaderSize = Magic.size() + sizeof(uint32) + sizeof(uint64) * 2U;
constexpr usize MaximumSceneBytes = 256U * 1'024U * 1'024U;
constexpr usize MaximumJsonDepth = 64U;
constexpr usize MaximumJsonNodes = 2'000'000U;
constexpr usize MaximumStringBytes = 16U * 1'024U * 1'024U;

void ValidateJsonBudget(const Json &Value, const usize Depth, usize &Nodes, usize &StringBytes)
{
	if (Depth > MaximumJsonDepth || ++Nodes > MaximumJsonNodes)
		throw RuntimeSceneBinaryException("Scene document exceeds its structural budget");
	if (Value.is_string())
	{
		const usize Bytes = Value.get_ref<const string &>().size();
		if (Bytes > MaximumStringBytes - std::min(MaximumStringBytes, StringBytes))
			throw RuntimeSceneBinaryException("Scene document exceeds its string-data budget");
		StringBytes += Bytes;
	}
	else if (Value.is_object())
	{
		for (const auto &[Key, Child] : Value.items())
		{
			if (Key.size() > MaximumStringBytes - std::min(MaximumStringBytes, StringBytes))
				throw RuntimeSceneBinaryException("Scene document exceeds its string-data budget");
			StringBytes += Key.size();
			ValidateJsonBudget(Child, Depth + 1U, Nodes, StringBytes);
		}
	}
	else if (Value.is_array())
	{
		for (const Json &Child : Value)
			ValidateJsonBudget(Child, Depth + 1U, Nodes, StringBytes);
	}
}

template <std::unsigned_integral ValueType> void Append(std::vector<uint8> &Destination, const ValueType Value)
{
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Destination.push_back(static_cast<uint8>(Value >> (ByteIndex * 8U)));
}

template <std::unsigned_integral ValueType> [[nodiscard]] ValueType Read(const std::span<const uint8> Source, usize &Offset)
{
	if (Source.size() - std::min(Source.size(), Offset) < sizeof(ValueType))
		throw RuntimeSceneBinaryException("Cooked scene header is truncated");
	ValueType Value = 0;
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Value |= static_cast<ValueType>(Source[Offset++]) << (ByteIndex * 8U);
	return Value;
}
} // namespace

bool RuntimeSceneBinary::IsBinary(const std::span<const uint8> Bytes) noexcept
{
	return Bytes.size() >= Magic.size() && std::equal(Magic.begin(), Magic.end(), Bytes.begin());
}

std::vector<uint8> RuntimeSceneBinary::Compile(const std::span<const uint8> JsonSource)
{
	try
	{
		if (JsonSource.empty() || JsonSource.size() > MaximumSceneBytes)
			throw RuntimeSceneBinaryException("Scene JSON exceeds the configured source-size budget");
		const Json Root = Json::parse(JsonSource.begin(), JsonSource.end(), nullptr, true, true);
		usize Nodes = 0;
		usize StringBytes = 0;
		ValidateJsonBudget(Root, 0, Nodes, StringBytes);
		if (!Root.is_object() || Root.value("FormatVersion", uint32{0}) != RuntimeSceneBinary::FormatVersion || !Root.contains("ID") ||
			!util::UUID::TryParse(Root.at("ID").get<string>()).has_value() || Root.value("Name", string{}).empty() ||
			!Root.contains("Objects") || !Root.at("Objects").is_array())
			throw RuntimeSceneBinaryException("Scene source identity, version, name, or object table is invalid");
		const std::vector<uint8> Payload = Json::to_cbor(Root);
		std::vector<uint8> Result;
		Result.reserve(HeaderSize + Payload.size());
		Result.insert(Result.end(), Magic.begin(), Magic.end());
		Append(Result, FormatVersion);
		Append(Result, static_cast<uint64>(Payload.size()));
		Append(Result, core::io::CompressedArchive::CalculateChecksum(Payload));
		Result.insert(Result.end(), Payload.begin(), Payload.end());
		return Result;
	}
	catch (const RuntimeSceneBinaryException &)
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		throw RuntimeSceneBinaryException("Could not compile scene JSON: " + string(Exception.what()));
	}
}

string RuntimeSceneBinary::DecodeToJson(const std::span<const uint8> BinaryScene)
{
	if (!IsBinary(BinaryScene) || BinaryScene.size() < HeaderSize)
		throw RuntimeSceneBinaryException("Cooked scene magic or header is invalid");
	usize Offset = Magic.size();
	const uint32 Version = Read<uint32>(BinaryScene, Offset);
	const uint64 PayloadSize = Read<uint64>(BinaryScene, Offset);
	const uint64 Checksum = Read<uint64>(BinaryScene, Offset);
	if (Version != FormatVersion)
		throw RuntimeSceneBinaryException("Cooked scene version is unsupported");
	if (PayloadSize > MaximumSceneBytes || PayloadSize != BinaryScene.size() - Offset)
		throw RuntimeSceneBinaryException("Cooked scene payload size does not match its header");
	const std::span<const uint8> Payload = BinaryScene.subspan(Offset);
	if (core::io::CompressedArchive::CalculateChecksum(Payload) != Checksum)
		throw RuntimeSceneBinaryException("Cooked scene checksum validation failed");
	try
	{
		const Json Root = Json::from_cbor(Payload.begin(), Payload.end(), true, true);
		usize Nodes = 0;
		usize StringBytes = 0;
		ValidateJsonBudget(Root, 0, Nodes, StringBytes);
		const string Result = Root.dump();
		if (Result.size() > MaximumSceneBytes)
			throw RuntimeSceneBinaryException("Decoded scene exceeds the configured JSON-size budget");
		return Result;
	}
	catch (const std::exception &Exception)
	{
		throw RuntimeSceneBinaryException("Could not decode cooked scene: " + string(Exception.what()));
	}
}
} // namespace runtime::project
