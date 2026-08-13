#include "AssetMetadata.h"

#include "Source/core/io/SecurePath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace editor::asset
{
namespace
{
using Json = nlohmann::json;
constexpr uint64 MaximumMetadataBytes = 16U * 1024U * 1024U;
constexpr uint64 MaximumSourceHashBytes = 16ULL * 1'024ULL * 1'024ULL * 1'024ULL;

[[nodiscard]] string Lowercase(string Value)
{
	std::ranges::transform(Value, Value.begin(),
						   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
	return Value;
}

[[nodiscard]] util::UUID HashPathIdentity(const std::filesystem::path &Path)
{
	const string Text = Lowercase(std::filesystem::absolute(Path).lexically_normal().generic_string());
	uint64 Left = 14'695'981'039'346'656'037ULL;
	uint64 Right = 1'099'511'628'211ULL;
	for (const uint8 Byte : std::span(reinterpret_cast<const uint8 *>(Text.data()), Text.size()))
	{
		Left = (Left ^ Byte) * 1'099'511'628'211ULL;
		Right ^= static_cast<uint64>(Byte) + 0x9e3779b97f4a7c15ULL + (Right << 6U) + (Right >> 2U);
	}
	return {Left, Right};
}

} // namespace

std::filesystem::path AssetMetadataStore::GetSidecarPath(const std::filesystem::path &AssetPath)
{
	return AssetPath.string() + ".assetmeta";
}

string AssetMetadataStore::CalculatePhysicalSourceIdentity(const std::filesystem::path &AssetPath)
{
	const HANDLE File = CreateFileW(AssetPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
									nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
		return HashPathIdentity(AssetPath).ToString();
	FILE_ID_INFO Identity{};
	if (GetFileInformationByHandleEx(File, FileIdInfo, &Identity, sizeof(Identity)) != FALSE)
	{
		(void)CloseHandle(File);
		uint64 Left = Identity.VolumeSerialNumber;
		uint64 Right = 0;
		uint64 Tail = 0;
		std::memcpy(&Right, Identity.FileId.Identifier, sizeof(Right));
		std::memcpy(&Tail, Identity.FileId.Identifier + sizeof(Right), sizeof(Tail));
		Left ^= Tail + 0x9e3779b97f4a7c15ULL + (Left << 6U) + (Left >> 2U);
		return util::UUID(Left, Right).ToString();
	}
	BY_HANDLE_FILE_INFORMATION Fallback{};
	if (GetFileInformationByHandle(File, &Fallback) != FALSE)
	{
		(void)CloseHandle(File);
		return util::UUID((static_cast<uint64>(Fallback.dwVolumeSerialNumber) << 32U) | Fallback.nFileIndexHigh, Fallback.nFileIndexLow)
			.ToString();
	}
	(void)CloseHandle(File);
	return HashPathIdentity(AssetPath).ToString();
}

string AssetMetadataStore::CalculateSourceHash(const std::filesystem::path &AssetPath)
{
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
		throw std::runtime_error("Could not initialize SHA-256");
	struct AlgorithmScope final
	{
		BCRYPT_ALG_HANDLE Handle;
		~AlgorithmScope()
		{
			if (this->Handle != nullptr)
				(void)BCryptCloseAlgorithmProvider(this->Handle, 0);
		}
	} AlgorithmGuard{Algorithm};

	ULONG ObjectBytes = 0;
	ULONG HashBytes = 0;
	ULONG ReturnedBytes = 0;
	if (BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes), &ReturnedBytes, 0) <
			0 ||
		BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashBytes), sizeof(HashBytes), &ReturnedBytes, 0) < 0 ||
		HashBytes != 32)
	{
		throw std::runtime_error("Could not query SHA-256 provider requirements");
	}
	std::vector<uint8> HashObject(ObjectBytes);
	std::vector<uint8> Digest(HashBytes);
	BCRYPT_HASH_HANDLE Hash = nullptr;
	if (BCryptCreateHash(Algorithm, &Hash, HashObject.data(), ObjectBytes, nullptr, 0, 0) < 0)
		throw std::runtime_error("Could not create SHA-256 state");
	struct HashScope final
	{
		BCRYPT_HASH_HANDLE Handle;
		~HashScope()
		{
			if (this->Handle != nullptr)
				(void)BCryptDestroyHash(this->Handle);
		}
	} HashGuard{Hash};

	try
	{
		(void)core::io::SecurePath::ReadFileChunksWithin(
			AssetPath.parent_path(), AssetPath.filename(), MaximumSourceHashBytes,
			[Hash](const std::span<const uint8> Bytes)
			{
				if (Bytes.size() > static_cast<usize>(std::numeric_limits<ULONG>::max()) ||
					BCryptHashData(Hash, const_cast<PUCHAR>(Bytes.data()), static_cast<ULONG>(Bytes.size()), 0) < 0)
				{
					throw std::runtime_error("Could not update SHA-256 source hash");
				}
			},
			"Asset source hash");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw std::runtime_error("Could not securely hash asset source: " + string(Exception.what()));
	}
	if (BCryptFinishHash(Hash, Digest.data(), HashBytes, 0) < 0)
		throw std::runtime_error("Could not finalize SHA-256 source hash");
	std::ostringstream Text;
	Text << std::hex << std::setfill('0');
	for (const uint8 Byte : Digest)
		Text << std::setw(2) << static_cast<uint32>(Byte);
	return Text.str();
}

std::optional<string> AssetMetadataStore::InferAssetTypeName(const std::filesystem::path &AssetPath)
{
	static const std::unordered_map<string, string> Types{{".png", "Texture2D"},
														  {".jpg", "Texture2D"},
														  {".jpeg", "Texture2D"},
														  {".tga", "Texture2D"},
														  {".bmp", "Texture2D"},
														  {".hdr", "Texture2D"},
														  {".gltf", "Model"},
														  {".glb", "Model"},
														  {".obj", "Model"},
														  {".fbx", "Model"},
														  {".dae", "Model"},
														  {".material", "Material"},
														  {".materialinstance", "MaterialInstance"},
														  {".mesh", "StaticMesh"},
														  {".skinnedmesh", "SkeletalMesh"},
														  {".skeleton", "Skeleton"},
														  {".animation", "AnimationClip"},
														  {".animationgraph", "AnimationGraph"},
														  {".retarget", "RetargetProfile"},
														  {".glsl", "ShaderSource"},
														  {".vert", "ShaderSource"},
														  {".frag", "ShaderSource"},
														  {".comp", "ShaderSource"},
														  {".scene", "Scene"},
														  {".enginelevel", "Scene"},
														  {".h", "SourceFile"},
														  {".cpp", "SourceFile"},
														  {".inl", "SourceFile"}};
	const auto Found = Types.find(Lowercase(AssetPath.extension().string()));
	return Found == Types.end() ? std::nullopt : std::optional<string>(Found->second);
}

AssetMetadata AssetMetadataStore::Create(const std::filesystem::path &AssetPath, string VirtualSource, string AssetType)
{
	return {.ID = util::UUID::GenerateRandomUUID().ToString(),
			.AssetType = std::move(AssetType),
			.VirtualSource = std::move(VirtualSource),
			.PhysicalSourceIdentity = CalculatePhysicalSourceIdentity(AssetPath),
			.SourceHash = CalculateSourceHash(AssetPath)};
}

std::optional<AssetMetadata> AssetMetadataStore::TryLoad(const std::filesystem::path &SidecarPath, string &Diagnostic)
{
	try
	{
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(SidecarPath.parent_path(), SidecarPath.filename(), MaximumMetadataBytes, "Asset metadata");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object())
			throw std::runtime_error("metadata root is not an object");

		AssetMetadata Result{.FormatVersion = Root.at("FormatVersion").get<uint32>(),
							 .ID = Root.at("ID").get<string>(),
							 .AssetType = Root.at("AssetType").get<string>(),
							 .ImporterVersion = Root.at("ImporterVersion").get<uint32>(),
							 .SchemaVersion = Root.at("SchemaVersion").get<uint32>(),
							 .VirtualSource = Root.at("VirtualSource").get<string>(),
							 .PhysicalSourceIdentity = Root.at("PhysicalSourceIdentity").get<string>(),
							 .SourceHash = Root.at("SourceHash").get<string>(),
							 .Dependencies = Root.value("Dependencies", std::vector<resource::AssetID>{})};
		if (Result.FormatVersion != AssetMetadata::CurrentFormatVersion)
			throw std::runtime_error("metadata format version is unsupported");
		if (!util::UUID::TryParse(Result.ID).has_value())
			throw std::runtime_error("asset ID is not a canonical UUID");
		if (Result.AssetType.empty() || Result.VirtualSource.empty() || Result.PhysicalSourceIdentity.empty() || Result.SourceHash.empty())
			throw std::runtime_error("required metadata field is empty");
		for (const resource::AssetID &Dependency : Result.Dependencies)
		{
			if (!util::UUID::TryParse(Dependency).has_value())
				throw std::runtime_error("dependency ID is not a canonical UUID");
		}
		for (const Json &Derived : Root.value("DerivedSubassets", Json::array()))
		{
			DerivedAssetMetadata Entry{.Key = Derived.at("Key").get<string>(),
									   .ID = Derived.at("ID").get<string>(),
									   .Type = static_cast<resource::AssetType>(Derived.at("Type").get<uint32>())};
			if (Entry.Key.empty() || !util::UUID::TryParse(Entry.ID).has_value() ||
				static_cast<uint32>(Entry.Type) >= static_cast<uint32>(resource::AssetType::Count))
			{
				throw std::runtime_error("derived-subasset metadata is invalid");
			}
			Result.DerivedAssets.push_back(std::move(Entry));
		}
		return Result;
	}
	catch (const std::exception &Exception)
	{
		Diagnostic = "Could not parse asset metadata '" + SidecarPath.string() + "': " + Exception.what();
		return std::nullopt;
	}
}

void AssetMetadataStore::Save(const AssetMetadata &Metadata, const std::filesystem::path &SidecarPath)
{
	if (!util::UUID::TryParse(Metadata.ID).has_value())
		throw std::invalid_argument("Asset metadata requires a canonical UUID");
	Json Root = Json::object();
	if (std::filesystem::is_regular_file(SidecarPath))
	{
		try
		{
			const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(SidecarPath.parent_path(), SidecarPath.filename(),
																				  MaximumMetadataBytes, "Existing asset metadata");
			Json Existing = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
			if (Existing.is_object())
				Root = std::move(Existing);
		}
		catch (const std::exception &)
		{
			// A malformed sidecar is replaced by the validated authoritative record below.
		}
	}
	Json Derived = Json::array();
	for (const DerivedAssetMetadata &Entry : Metadata.DerivedAssets)
	{
		Derived.push_back({{"Key", Entry.Key}, {"ID", Entry.ID}, {"Type", static_cast<uint32>(Entry.Type)}});
	}
	Root["FormatVersion"] = AssetMetadata::CurrentFormatVersion;
	Root["ID"] = Metadata.ID;
	Root["AssetType"] = Metadata.AssetType;
	Root["ImporterVersion"] = Metadata.ImporterVersion;
	Root["SchemaVersion"] = Metadata.SchemaVersion;
	Root["VirtualSource"] = Metadata.VirtualSource;
	Root["PhysicalSourceIdentity"] = Metadata.PhysicalSourceIdentity;
	if (!Root.contains("ImportSettings"))
		Root["ImportSettings"] = Json::object();
	Root["DerivedSubassets"] = std::move(Derived);
	Root["SourceHash"] = Metadata.SourceHash;
	Root["Dependencies"] = Metadata.Dependencies;
	if (!Root.contains("EditorMetadata"))
		Root["EditorMetadata"] = Json::object();

	if (std::filesystem::is_regular_file(SidecarPath))
	{
		try
		{
			const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(SidecarPath.parent_path(), SidecarPath.filename(),
																				  MaximumMetadataBytes, "Existing asset metadata");
			const Json Existing = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
			if (Existing == Root)
				return;
		}
		catch (const std::exception &)
		{
		}
	}
	core::io::SecurePath::CreateTrustedRoot(SidecarPath.parent_path(), "Asset metadata root");
	const std::filesystem::path Temporary = SidecarPath.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump(2) + '\n';
	try
	{
		core::io::SecurePath::WriteFileWithin(SidecarPath.parent_path(), Temporary,
											  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
											  "Asset metadata temporary file");
		core::io::SecurePath::ReplaceWithin(SidecarPath.parent_path(), Temporary, SidecarPath.filename(), "Asset metadata publication");
	}
	catch (...)
	{
		try
		{
			if (std::filesystem::exists(SidecarPath.parent_path() / Temporary))
				core::io::SecurePath::RemoveWithin(SidecarPath.parent_path(), Temporary, false, "Asset metadata cleanup");
		}
		catch (...)
		{
		}
		throw;
	}
}
} // namespace editor::asset
