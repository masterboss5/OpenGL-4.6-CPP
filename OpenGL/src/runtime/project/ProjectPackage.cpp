#include "ProjectPackage.h"

#include "src/core/io/CompressedArchive.h"
#include "src/core/io/SecurePath.h"
#include "src/core/threading/TaskScheduler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <concepts>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace runtime::project
{
namespace
{
using Json = nlohmann::json;

struct MountContentEntry final
{
	std::filesystem::path LogicalPath;
	std::filesystem::path ArchivePath;
	uint64 ArchiveOffset = 0;
	uint64 SourceBytes = 0;
	uint64 ArchiveBytes = 0;
	uint64 ContentChecksum = 0;
	string ContentSHA256;
	string Encoding;
};

constexpr std::array<uint8, 8> ChunkMagic{'O', 'G', 'L', 'C', 'H', 'N', 'K', 0};
constexpr uint32 ChunkFormatVersion = 1;
constexpr uint64 ChunkHeaderBytes = ChunkMagic.size() + sizeof(uint32) + sizeof(uint64) * 5U;
constexpr uint64 MaximumManifestBytes = 64ULL * 1'024ULL * 1'024ULL;
constexpr usize MaximumContentEntries = 1'000'000U;
constexpr usize MaximumRuntimeFiles = 16'384U;
constexpr uint64 MaximumRuntimeFileBytes = 4ULL * 1'024ULL * 1'024ULL * 1'024ULL;
constexpr uint64 MaximumPackageDecodedBytes = 256ULL * 1'024ULL * 1'024ULL * 1'024ULL;
constexpr uint32 MaximumMountWorkers = 8U;

class SHA256State final
{
  public:
	SHA256State()
	{
		if (BCryptOpenAlgorithmProvider(&this->Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			throw ProjectPackageException("Could not initialize package SHA-256 provider");
		ULONG ObjectBytes = 0;
		ULONG Returned = 0;
		if (BCryptGetProperty(this->Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes), &Returned,
							  0) < 0)
		{
			BCryptCloseAlgorithmProvider(this->Algorithm, 0);
			this->Algorithm = nullptr;
			throw ProjectPackageException("Could not query package SHA-256 provider");
		}
		this->Object.resize(ObjectBytes);
		if (BCryptCreateHash(this->Algorithm, &this->Hash, this->Object.data(), ObjectBytes, nullptr, 0, 0) < 0)
		{
			BCryptCloseAlgorithmProvider(this->Algorithm, 0);
			this->Algorithm = nullptr;
			throw ProjectPackageException("Could not create package SHA-256 state");
		}
	}

	~SHA256State()
	{
		if (this->Hash != nullptr)
			BCryptDestroyHash(this->Hash);
		if (this->Algorithm != nullptr)
			BCryptCloseAlgorithmProvider(this->Algorithm, 0);
	}

	SHA256State(const SHA256State &) = delete;
	SHA256State &operator=(const SHA256State &) = delete;
	SHA256State(SHA256State &&) = delete;
	SHA256State &operator=(SHA256State &&) = delete;

	void Update(const std::span<const uint8> Bytes)
	{
		if (this->Finished)
			throw ProjectPackageException("Package SHA-256 state was already finalized");
		for (usize Offset = 0; Offset < Bytes.size();)
		{
			const uint32 Chunk = static_cast<uint32>(std::min<usize>(Bytes.size() - Offset, std::numeric_limits<uint32>::max()));
			if (BCryptHashData(this->Hash, const_cast<PUCHAR>(Bytes.data() + Offset), Chunk, 0) < 0)
				throw ProjectPackageException("Could not update package SHA-256 digest");
			Offset += Chunk;
		}
	}

	[[nodiscard]] std::vector<uint8> Finish()
	{
		if (this->Finished)
			throw ProjectPackageException("Package SHA-256 state was already finalized");
		std::vector<uint8> Digest(32U);
		if (BCryptFinishHash(this->Hash, Digest.data(), static_cast<uint32>(Digest.size()), 0) < 0)
			throw ProjectPackageException("Could not finish package SHA-256 digest");
		this->Finished = true;
		return Digest;
	}

  private:
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_HASH_HANDLE Hash = nullptr;
	std::vector<uint8> Object;
	bool Finished = false;
};

[[nodiscard]] std::vector<uint8> SHA256(const std::span<const uint8> Bytes)
{
	SHA256State State;
	State.Update(Bytes);
	return State.Finish();
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

[[nodiscard]] string Hex(const std::span<const uint8> Bytes)
{
	static constexpr char Digits[] = "0123456789abcdef";
	string Result(Bytes.size() * 2U, '0');
	for (usize Index = 0; Index < Bytes.size(); ++Index)
	{
		Result[Index * 2U] = Digits[Bytes[Index] >> 4U];
		Result[Index * 2U + 1U] = Digits[Bytes[Index] & 0x0fU];
	}
	return Result;
}

[[nodiscard]] std::vector<uint8> DecodeHex(const string_view Text)
{
	if (Text.empty() || Text.size() % 2U != 0)
		throw ProjectPackageException("Package signature encoding is invalid");
	auto Digit = [](const char Character) -> uint8
	{
		if (Character >= '0' && Character <= '9')
			return static_cast<uint8>(Character - '0');
		if (Character >= 'a' && Character <= 'f')
			return static_cast<uint8>(Character - 'a' + 10);
		throw ProjectPackageException("Package signature encoding is invalid");
	};
	std::vector<uint8> Result(Text.size() / 2U);
	for (usize Index = 0; Index < Result.size(); ++Index)
		Result[Index] = static_cast<uint8>((Digit(Text[Index * 2U]) << 4U) | Digit(Text[Index * 2U + 1U]));
	return Result;
}

void VerifyManifestSignature(const Json &Manifest, const ProjectPackageTrustPolicy &Policy)
{
	if (!Manifest.contains("Signature"))
	{
		if (Policy.RequireSignature)
			throw ProjectPackageException("Package signature is required by trust policy");
		return;
	}
	const Json &SignatureNode = Manifest.at("Signature");
	if (!SignatureNode.is_object() || SignatureNode.value("Algorithm", string{}) != "RSA-PKCS1-SHA256")
		throw ProjectPackageException("Package signature algorithm is unsupported");
	const string KeyID = SignatureNode.at("KeyID").get<string>();
	const uint32 KeyVersion = SignatureNode.at("KeyVersion").get<uint32>();
	const auto Key = std::ranges::find_if(Policy.TrustedKeys, [&KeyID, KeyVersion](const TrustedPackageSigningKey &Candidate)
										  { return Candidate.ID == KeyID && Candidate.Version == KeyVersion; });
	if (Key == Policy.TrustedKeys.end() || Key->PublicKeyBlob.empty())
		throw ProjectPackageException("Package signature key is not trusted by the active key-rotation policy");
	Json CanonicalManifest = Manifest;
	CanonicalManifest.erase("Signature");
	const string Canonical = CanonicalManifest.dump();
	const std::vector<uint8> Digest = SHA256(std::span(reinterpret_cast<const uint8 *>(Canonical.data()), Canonical.size()));
	const std::vector<uint8> Signature = DecodeHex(SignatureNode.at("Value").get<string>());
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_KEY_HANDLE PublicKey = nullptr;
	if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
		BCryptImportKeyPair(Algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &PublicKey, const_cast<PUCHAR>(Key->PublicKeyBlob.data()),
							static_cast<uint32>(Key->PublicKeyBlob.size()), 0) < 0)
		throw ProjectPackageException("Could not import trusted package signing key");
	struct Scope final
	{
		BCRYPT_ALG_HANDLE Algorithm;
		BCRYPT_KEY_HANDLE Key;
		~Scope()
		{
			if (this->Key != nullptr)
				BCryptDestroyKey(this->Key);
			if (this->Algorithm != nullptr)
				BCryptCloseAlgorithmProvider(this->Algorithm, 0);
		}
	} Guard{Algorithm, PublicKey};
	BCRYPT_PKCS1_PADDING_INFO Padding{BCRYPT_SHA256_ALGORITHM};
	if (BCryptVerifySignature(PublicKey, &Padding, const_cast<PUCHAR>(Digest.data()), static_cast<uint32>(Digest.size()),
							  const_cast<PUCHAR>(Signature.data()), static_cast<uint32>(Signature.size()), BCRYPT_PAD_PKCS1) < 0)
		throw ProjectPackageException("Package manifest signature validation failed");
}

template <std::unsigned_integral ValueType> [[nodiscard]] ValueType ReadLittleEndian(const std::span<const uint8> Bytes, usize &Offset)
{
	if (Bytes.size() - std::min(Bytes.size(), Offset) < sizeof(ValueType))
		throw ProjectPackageException("Package archive chunk header is truncated");
	ValueType Value = 0;
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Value |= static_cast<ValueType>(Bytes[Offset++]) << (ByteIndex * 8U);
	return Value;
}

class PackageMountLock final
{
  public:
	PackageMountLock(const util::UUID &ProjectID, const util::UUID &OperationID)
	{
		const string NarrowName = "Local\\OpenGLPackage-" + ProjectID.ToString() + "-" + OperationID.ToString();
		const std::wstring Name(NarrowName.begin(), NarrowName.end());
		this->Handle = CreateMutexW(nullptr, FALSE, Name.c_str());
		if (this->Handle == nullptr)
			throw ProjectPackageException("Could not create the package cache coordination lock");
		const DWORD Result = WaitForSingleObject(this->Handle, 30'000);
		if (Result != WAIT_OBJECT_0 && Result != WAIT_ABANDONED)
		{
			CloseHandle(this->Handle);
			this->Handle = nullptr;
			throw ProjectPackageException("Timed out waiting for another process to finish mounting the package");
		}
	}

	~PackageMountLock()
	{
		if (this->Handle == nullptr)
			return;
		ReleaseMutex(this->Handle);
		CloseHandle(this->Handle);
	}

	PackageMountLock(const PackageMountLock &) = delete;
	PackageMountLock &operator=(const PackageMountLock &) = delete;
	PackageMountLock(PackageMountLock &&) = delete;
	PackageMountLock &operator=(PackageMountLock &&) = delete;

  private:
	HANDLE Handle = nullptr;
};

[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path &Path)
{
	if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory())
		return false;
	for (const std::filesystem::path &Part : Path.lexically_normal())
	{
		if (Part == ".." || Part == ".")
			return false;
	}
	return true;
}

[[nodiscard]] string NormalizePathKey(const std::filesystem::path &Path)
{
	string Key = Path.lexically_normal().generic_string();
	std::ranges::transform(Key, Key.begin(),
						   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
	return Key;
}

[[nodiscard]] std::filesystem::path ResolveWithin(const std::filesystem::path &Root, const std::filesystem::path &Relative,
												  const string_view Field)
{
	if (!IsSafeRelativePath(Relative))
		throw ProjectPackageException(string(Field) + " must be a safe relative path");
	const std::filesystem::path NormalRoot = std::filesystem::absolute(Root).lexically_normal();
	const std::filesystem::path Result = std::filesystem::absolute(NormalRoot / Relative).lexically_normal();
	auto RootPart = NormalRoot.begin();
	auto ResultPart = Result.begin();
	for (; RootPart != NormalRoot.end(); ++RootPart, ++ResultPart)
	{
		if (ResultPart == Result.end() || *RootPart != *ResultPart)
			throw ProjectPackageException(string(Field) + " escapes its package root");
	}
	std::filesystem::path Current = NormalRoot;
	for (const std::filesystem::path &Part : Relative)
	{
		Current /= Part;
		const DWORD Attributes = GetFileAttributesW(Current.c_str());
		if (Attributes == INVALID_FILE_ATTRIBUTES)
			break;
		if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			throw ProjectPackageException(string(Field) + " traverses a reparse point");
	}
	return Result;
}

[[nodiscard]] Json ReadManifest(const std::filesystem::path &Path)
{
	try
	{
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(Path.parent_path(), Path.filename(), MaximumManifestBytes, "Package manifest");
		return Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
	}
	catch (const Json::exception &Exception)
	{
		throw ProjectPackageException("Could not parse package manifest: " + string(Exception.what()));
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ProjectPackageException("Could not securely read package manifest: " + string(Exception.what()));
	}
}

[[nodiscard]] std::vector<uint8> ReadRange(const std::filesystem::path &Root, const std::filesystem::path &Relative, const uint64 Offset,
										   const uint64 ByteCount, const uint64 MaximumFileBytes)
{
	try
	{
		return core::io::SecurePath::ReadFileRangeWithin(Root, Relative, Offset, ByteCount, MaximumFileBytes, "Package file range");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ProjectPackageException("Could not securely read package file range: " + string(Exception.what()));
	}
}

[[nodiscard]] std::filesystem::path GetCacheRoot()
{
	PWSTR RawPath = nullptr;
	const HRESULT Result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &RawPath);
	if (FAILED(Result) || RawPath == nullptr)
		throw ProjectPackageException("Could not resolve the per-user runtime cache directory");
	const std::filesystem::path LocalRoot = RawPath;
	CoTaskMemFree(RawPath);
	core::io::SecurePath::CreateDirectoriesWithin(LocalRoot, std::filesystem::path("OpenGL46Engine") / "RuntimeCache",
												  "Runtime package cache root");
	const std::filesystem::path Root = LocalRoot / "OpenGL46Engine" / "RuntimeCache";
	return Root;
}

void VerifyFileIntegrity(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath, const uint64 MaximumBytes,
						 const uint64 ExpectedBytes, const uint64 ExpectedChecksum, const string_view ExpectedSHA256,
						 const string_view Role)
{
	uint64 Checksum = 14'695'981'039'346'656'037ULL;
	SHA256State Digest;
	uint64 ActualBytes = 0;
	try
	{
		ActualBytes = core::io::SecurePath::ReadFileChunksWithin(
			TrustedRoot, RelativePath, MaximumBytes,
			[&Checksum, &Digest](const std::span<const uint8> Chunk)
			{
				Checksum = UpdateChecksum(Checksum, Chunk);
				Digest.Update(Chunk);
			},
			Role);
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw ProjectPackageException("Could not securely verify " + string(Role) + ": " + Exception.what());
	}
	if (ActualBytes != ExpectedBytes || Checksum != ExpectedChecksum || Hex(Digest.Finish()) != ExpectedSHA256)
		throw ProjectPackageException(string(Role) + " size or checksum validation failed: '" + RelativePath.string() + "'");
}

void WriteReadyMarker(const std::filesystem::path &TrustedRoot, const util::UUID &OperationID)
{
	const string Contents = OperationID.ToString() + '\n';
	core::io::SecurePath::WriteFileWithin(TrustedRoot, ".ready",
										  std::span(reinterpret_cast<const uint8 *>(Contents.data()), Contents.size()), false, true,
										  "Package cache completion marker");
}

[[nodiscard]] bool CacheIsReady(const std::filesystem::path &Directory, const util::UUID &OperationID)
{
	try
	{
		const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(Directory, ".ready", 128U, "Package cache ready marker");
		const string Value(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
		return Value == OperationID.ToString() + '\n';
	}
	catch (const std::exception &)
	{
		return false;
	}
}
} // namespace

bool ProjectPackage::IsPackage(const std::filesystem::path &Root)
{
	std::error_code Error;
	return std::filesystem::is_regular_file(std::filesystem::absolute(Root).lexically_normal() / "PackageManifest.json", Error) && !Error;
}

ProjectPackageMount ProjectPackage::Mount(const std::filesystem::path &Root, const std::filesystem::path &CacheRoot,
										  const ProjectPackageTrustPolicy &TrustPolicy)
{
	const std::filesystem::path PackageRoot = std::filesystem::absolute(Root).lexically_normal();
	const std::filesystem::path ManifestPath = PackageRoot / "PackageManifest.json";
	const Json Manifest = ReadManifest(ManifestPath);
	VerifyManifestSignature(Manifest, TrustPolicy);
	if (!Manifest.is_object() || Manifest.value("FormatVersion", uint32{0}) != ProjectPackageFormatVersion ||
		Manifest.value("Compression", string{}) != "Zstandard" || !Manifest.contains("Content") || !Manifest["Content"].is_array() ||
		!Manifest.contains("GameModule"))
	{
		throw ProjectPackageException("Package manifest root, version, compression, or content table is invalid");
	}

	ProjectPackageMount Mount;
	try
	{
		Mount.OperationID = util::UUID::Parse(Manifest.at("OperationID").get<string>());
		Mount.BuildID = util::UUID::Parse(Manifest.at("BuildID").get<string>());
		Mount.ProjectID = util::UUID::Parse(Manifest.at("ProjectID").get<string>());
		Mount.ProjectName = Manifest.at("ProjectName").get<string>();
		Mount.StartupScene = Manifest.at("StartupScene").get<string>();
	}
	catch (const std::exception &Exception)
	{
		throw ProjectPackageException("Package identity or startup-scene metadata is invalid: " + string(Exception.what()));
	}
	if (Mount.ProjectName.empty() || !IsSafeRelativePath(Mount.StartupScene))
		throw ProjectPackageException("Package project name or startup scene is invalid");
	const string ContentIndexText = Manifest.at("Content").dump();
	const uint64 ActualContentIndexChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(ContentIndexText.data()), ContentIndexText.size()));
	if (Manifest.value("ContentIndexChecksum", uint64{0}) != ActualContentIndexChecksum)
		throw ProjectPackageException("Package content index checksum validation failed");

	Mount.PackageRoot = PackageRoot;
	if (!Manifest.contains("RuntimeFiles") || !Manifest["RuntimeFiles"].is_array())
		throw ProjectPackageException("Package runtime-file table is missing or invalid");
	if (Manifest["RuntimeFiles"].size() > MaximumRuntimeFiles || Manifest["Content"].size() > MaximumContentEntries)
		throw ProjectPackageException("Package manifest exceeds the configured entry-count budget");
	const string RuntimeIndexText = Manifest.at("RuntimeFiles").dump();
	const uint64 ActualRuntimeIndexChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(RuntimeIndexText.data()), RuntimeIndexText.size()));
	if (Manifest.value("RuntimeIndexChecksum", uint64{0}) != ActualRuntimeIndexChecksum)
		throw ProjectPackageException("Package runtime-file index checksum validation failed");
	const util::UUID ExpectedBuildID((Mount.ProjectID.GetLeft() ^ ActualContentIndexChecksum ^ ActualRuntimeIndexChecksum) | 1ULL,
									 (Mount.ProjectID.GetRight() ^ ActualContentIndexChecksum ^ (ActualRuntimeIndexChecksum << 1U)) | 1ULL);
	if (Mount.BuildID != ExpectedBuildID)
		throw ProjectPackageException("Package build identity does not match its hashed indexes");
	std::unordered_set<string> RuntimePaths;
	uint64 TotalRuntimeBytes = 0;
	usize ExecutableCount = 0;
	usize GameModuleCount = 0;
	string GameModulePath;
	for (const Json &RuntimeFile : Manifest["RuntimeFiles"])
	{
		const std::filesystem::path Relative = RuntimeFile.at("Path").get<string>();
		const uint32 RawKind = RuntimeFile.at("Kind").get<uint32>();
		if (RawKind > static_cast<uint32>(PackageFileKind::EngineContent))
			throw ProjectPackageException("Package runtime-file kind is invalid");
		const auto Kind = static_cast<PackageFileKind>(RawKind);
		const string Key = NormalizePathKey(Relative);
		if (!RuntimePaths.emplace(Key).second)
			throw ProjectPackageException("Package runtime-file table contains a duplicate path");
		const std::filesystem::path RuntimePath = ResolveWithin(PackageRoot, Relative, "RuntimeFile");
		std::error_code RuntimeError;
		const uint64 RuntimeBytes = std::filesystem::file_size(RuntimePath, RuntimeError);
		if (RuntimeError || RuntimeBytes > MaximumRuntimeFileBytes ||
			RuntimeBytes > MaximumPackageDecodedBytes - std::min(MaximumPackageDecodedBytes, TotalRuntimeBytes) ||
			RuntimeBytes != RuntimeFile.at("Bytes").get<uint64>())
			throw ProjectPackageException("Package runtime file is missing: '" + Relative.string() + "'");
		TotalRuntimeBytes += RuntimeBytes;
		VerifyFileIntegrity(PackageRoot, Relative, MaximumRuntimeFileBytes, RuntimeBytes, RuntimeFile.at("Checksum").get<uint64>(),
							RuntimeFile.at("SHA256").get<string>(), "Package runtime file");
		switch (Kind)
		{
		case PackageFileKind::Executable:
			++ExecutableCount;
			break;
		case PackageFileKind::GameModule:
			++GameModuleCount;
			GameModulePath = Key;
			break;
		case PackageFileKind::DynamicLibrary:
		case PackageFileKind::EngineContent:
			break;
		default:
			throw ProjectPackageException("Package runtime-file kind is invalid");
		}
	}
	if (ExecutableCount != 1 || GameModuleCount > 1)
		throw ProjectPackageException("Package must contain exactly one executable and at most one game module");
	Mount.EngineContentRoot = PackageRoot / "Engine";
	if (!std::filesystem::is_directory(Mount.EngineContentRoot / "shader"))
		throw ProjectPackageException("Package does not contain the required engine shader content");
	if (!Manifest.at("GameModule").is_null())
	{
		Mount.GameModule = ResolveWithin(PackageRoot, Manifest.at("GameModule").get<string>(), "GameModule");
		if (GameModuleCount != 1 || GameModulePath != NormalizePathKey(Mount.GameModule.lexically_relative(PackageRoot)))
			throw ProjectPackageException("Package game-module metadata does not match its runtime-file table");
	}
	else if (GameModuleCount != 0)
	{
		throw ProjectPackageException("Package runtime-file table contains an unreferenced game module");
	}

	std::unordered_set<string> LogicalPaths;
	std::unordered_set<string> AssetIDs;
	std::unordered_map<string, std::vector<string>> AssetDependencies;
	std::vector<MountContentEntry> Entries;
	std::unordered_map<string, std::vector<std::pair<uint64, uint64>>> ArchiveRanges;
	bool StartupSceneCompiled = false;
	Entries.reserve(Manifest["Content"].size());
	uint64 TotalDecodedBytes = 0;
	for (const Json &Entry : Manifest["Content"])
	{
		const std::filesystem::path Logical = Entry.at("LogicalPath").get<string>();
		const std::filesystem::path Archive = Entry.at("ArchivePath").get<string>();
		const uint64 ArchiveOffset = Entry.at("ArchiveOffset").get<uint64>();
		const uint64 SourceBytes = Entry.at("SourceBytes").get<uint64>();
		const uint64 ArchiveBytes = Entry.at("ArchiveBytes").get<uint64>();
		const uint64 ContentChecksum = Entry.at("ContentChecksum").get<uint64>();
		const string ContentSHA256 = Entry.at("ContentSHA256").get<string>();
		if (ContentSHA256.size() != 64U)
			throw ProjectPackageException("Package content SHA-256 metadata is invalid");
		if (!IsSafeRelativePath(Logical))
			throw ProjectPackageException("Package logical content path is not safe");
		if (!LogicalPaths.emplace(NormalizePathKey(Logical)).second)
			throw ProjectPackageException("Package content table contains a duplicate logical path");
		const string AssetID = Entry.at("AssetID").get<string>();
		if (!util::UUID::TryParse(AssetID).has_value() || !AssetIDs.emplace(AssetID).second ||
			Entry.at("AssetType").get<string>().empty() || Entry.at("SourceHash").get<string>().empty() ||
			!Entry.at("Dependencies").is_array())
			throw ProjectPackageException("Package content AssetID, type, hash, or dependency metadata is invalid");
		std::vector<string> Dependencies;
		for (const Json &Dependency : Entry.at("Dependencies"))
		{
			const string DependencyID = Dependency.get<string>();
			if (!util::UUID::TryParse(DependencyID).has_value())
				throw ProjectPackageException("Package content dependency is not a canonical AssetID");
			Dependencies.push_back(DependencyID);
		}
		AssetDependencies.emplace(AssetID, std::move(Dependencies));
		const string Encoding = Entry.at("Encoding").get<string>();
		if (Encoding != "Raw" && Encoding != "SceneCBOR")
			throw ProjectPackageException("Package content encoding is unsupported");
		if (NormalizePathKey(Logical) == NormalizePathKey(Mount.StartupScene))
			StartupSceneCompiled = Encoding == "SceneCBOR";
		if (SourceBytes > std::numeric_limits<uint64>::max() - TotalDecodedBytes)
			throw ProjectPackageException("Package decoded-size total overflows its supported range");
		TotalDecodedBytes += SourceBytes;
		if (TotalDecodedBytes > MaximumPackageDecodedBytes)
			throw ProjectPackageException("Package decoded content exceeds the configured aggregate budget");
		if (!IsSafeRelativePath(Archive) || !NormalizePathKey(Archive).starts_with("content/chunks/"))
			throw ProjectPackageException("Package archive path must remain under Content/Chunks");
		const std::filesystem::path ArchivePath = ResolveWithin(PackageRoot, Archive, "ArchivePath");
		std::error_code ArchiveError;
		const uint64 ArchiveFileBytes = std::filesystem::file_size(ArchivePath, ArchiveError);
		if (ArchiveError || ArchiveOffset < ChunkHeaderBytes || ArchiveBytes > ArchiveFileBytes ||
			ArchiveOffset > ArchiveFileBytes - ArchiveBytes || ArchiveOffset > std::numeric_limits<uint64>::max() - ArchiveBytes)
			throw ProjectPackageException("Package archive range exceeds its chunk file");
		auto &Ranges = ArchiveRanges[NormalizePathKey(Archive)];
		for (const auto &[OtherOffset, OtherBytes] : Ranges)
		{
			if (ArchiveOffset < OtherOffset + OtherBytes && OtherOffset < ArchiveOffset + ArchiveBytes)
				throw ProjectPackageException("Package archive ranges overlap");
		}
		Ranges.emplace_back(ArchiveOffset, ArchiveBytes);
		Entries.push_back({.LogicalPath = Logical,
						   .ArchivePath = ArchivePath,
						   .ArchiveOffset = ArchiveOffset,
						   .SourceBytes = SourceBytes,
						   .ArchiveBytes = ArchiveBytes,
						   .ContentChecksum = ContentChecksum,
						   .ContentSHA256 = ContentSHA256,
						   .Encoding = Encoding});
	}
	for (const auto &[AssetID, Dependencies] : AssetDependencies)
	{
		(void)AssetID;
		for (const string &Dependency : Dependencies)
		{
			if (!AssetIDs.contains(Dependency))
				throw ProjectPackageException("Package content dependency closure is incomplete");
		}
	}
	if (!StartupSceneCompiled)
		throw ProjectPackageException("Package startup scene is missing or is not compiled runtime scene data");
	for (const auto &[ArchiveKey, Ranges] : ArchiveRanges)
	{
		const auto Found =
			std::ranges::find_if(Entries, [&ArchiveKey, &PackageRoot](const MountContentEntry &Entry)
								 { return NormalizePathKey(Entry.ArchivePath.lexically_relative(PackageRoot)) == ArchiveKey; });
		if (Found == Entries.end())
			throw ProjectPackageException("Package archive index references an unknown chunk");
		const std::vector<uint8> Header =
			ReadRange(PackageRoot, Found->ArchivePath.lexically_relative(PackageRoot), 0, ChunkHeaderBytes, MaximumRuntimeFileBytes);
		if (!std::equal(ChunkMagic.begin(), ChunkMagic.end(), Header.begin()))
			throw ProjectPackageException("Package archive chunk magic is invalid");
		usize HeaderOffset = ChunkMagic.size();
		if (ReadLittleEndian<uint32>(Header, HeaderOffset) != ChunkFormatVersion ||
			ReadLittleEndian<uint64>(Header, HeaderOffset) != Ranges.size() ||
			ReadLittleEndian<uint64>(Header, HeaderOffset) != Mount.ProjectID.GetLeft() ||
			ReadLittleEndian<uint64>(Header, HeaderOffset) != Mount.ProjectID.GetRight() ||
			ReadLittleEndian<uint64>(Header, HeaderOffset) != Mount.BuildID.GetLeft() ||
			ReadLittleEndian<uint64>(Header, HeaderOffset) != Mount.BuildID.GetRight())
			throw ProjectPackageException("Package archive chunk version or entry count is invalid");
	}

	const std::filesystem::path TrustedCacheRoot =
		CacheRoot.empty() ? GetCacheRoot() : std::filesystem::absolute(CacheRoot).lexically_normal();
	core::io::SecurePath::CreateTrustedRoot(TrustedCacheRoot, "Configured package cache root");
	core::io::SecurePath::CreateDirectoriesWithin(TrustedCacheRoot, Mount.ProjectID.ToString(), "Package cache project");
	const std::filesystem::path CacheRelative = std::filesystem::path(Mount.ProjectID.ToString()) / Mount.BuildID.ToString();
	const std::filesystem::path CacheDirectory = TrustedCacheRoot / CacheRelative;
	const PackageMountLock Lock(Mount.ProjectID, Mount.BuildID);
	if (!CacheIsReady(CacheDirectory, Mount.BuildID))
	{
		std::error_code Error;
		if (std::filesystem::exists(CacheDirectory, Error))
			core::io::SecurePath::RemoveWithin(TrustedCacheRoot, CacheRelative, true, "Incomplete package cache");
		const std::filesystem::path Staging =
			CacheDirectory.parent_path() / (CacheDirectory.filename().string() + ".staging-" + util::UUID::GenerateRandomUUID().ToString());
		try
		{
			core::io::SecurePath::CreateDirectoriesWithin(TrustedCacheRoot, (Staging / "Content").lexically_relative(TrustedCacheRoot),
														  "Package cache staging");
			std::error_code SpaceError;
			const std::filesystem::space_info Space = std::filesystem::space(Staging, SpaceError);
			if (SpaceError)
				throw ProjectPackageException("Could not query available package-cache storage: " + SpaceError.message());
			if (Space.available < TotalDecodedBytes)
				throw ProjectPackageException("Package cache does not have enough free storage for decoded content");
			if (!Entries.empty())
			{
				const uint32 WorkerCount = std::min<uint32>(MaximumMountWorkers, static_cast<uint32>(Entries.size()));
				core::threading::TaskScheduler Scheduler({.WorkerCount = WorkerCount, .Capacity = WorkerCount});
				core::threading::TaskGroup Tasks;
				std::atomic<usize> NextEntry = 0;
				for (uint32 WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
				{
					(void)WorkerIndex;
					Tasks.Run(
						Scheduler,
						[&Entries, &NextEntry, Staging, PackageRoot]()
						{
							for (;;)
							{
								const usize EntryIndex = NextEntry.fetch_add(1, std::memory_order_relaxed);
								if (EntryIndex >= Entries.size())
									return;
								const MountContentEntry &Entry = Entries[EntryIndex];
								const std::filesystem::path ContentRoot = Staging / "Content";
								(void)ResolveWithin(ContentRoot, Entry.LogicalPath, "LogicalPath");
								core::io::SecurePath::WriteFileWithin(ContentRoot, Entry.LogicalPath, std::span<const uint8>{}, false,
																	  false, "Mounted package content staging file");
								SHA256State Digest;
								uint64 OutputOffset = 0;
								core::io::CompressedArchiveDecodeResult Result;
								try
								{
									Result = core::io::CompressedArchive::DecodeStream(
										Entry.ArchiveBytes,
										[PackageRoot, Entry](const uint64 Offset, const std::span<uint8> Destination)
										{
											if (Offset > Entry.ArchiveBytes || Destination.size() > Entry.ArchiveBytes - Offset ||
												Entry.ArchiveOffset > std::numeric_limits<uint64>::max() - Offset)
											{
												throw ProjectPackageException("Package content stream exceeded its declared archive range");
											}
											const std::vector<uint8> Bytes =
												ReadRange(PackageRoot, Entry.ArchivePath.lexically_relative(PackageRoot),
														  Entry.ArchiveOffset + Offset, static_cast<uint64>(Destination.size()),
														  MaximumRuntimeFileBytes);
											std::ranges::copy(Bytes, Destination.begin());
										},
										[ContentRoot, &Entry, &Digest, &OutputOffset](const std::span<const uint8> Chunk)
										{
											Digest.Update(Chunk);
											core::io::SecurePath::WriteFileAtWithin(ContentRoot, Entry.LogicalPath, OutputOffset, Chunk,
																					false, "Mounted package content stream");
											OutputOffset += Chunk.size();
										},
										Entry.SourceBytes);
								}
								catch (const core::io::CompressedArchiveException &Exception)
								{
									throw ProjectPackageException("Package content decompression failed for '" +
																  Entry.LogicalPath.generic_string() + "': " + Exception.what());
								}
								core::io::SecurePath::WriteFileAtWithin(ContentRoot, Entry.LogicalPath, OutputOffset,
																		std::span<const uint8>{}, true,
																		"Mounted package content finalization");
								if (Result.DecodedBytes != Entry.SourceBytes || OutputOffset != Entry.SourceBytes ||
									Result.Checksum != Entry.ContentChecksum || Hex(Digest.Finish()) != Entry.ContentSHA256)
								{
									throw ProjectPackageException("Package content size or checksum does not match its manifest entry");
								}
							}
						},
						core::threading::TaskPriority::Critical);
				}
				Tasks.Wait();
			}
			WriteReadyMarker(Staging, Mount.BuildID);
			core::io::SecurePath::MoveWithin(TrustedCacheRoot, Staging.lexically_relative(TrustedCacheRoot), TrustedCacheRoot,
											 CacheRelative, false, "Package cache publication");
		}
		catch (...)
		{
			try
			{
				if (std::filesystem::exists(Staging))
					core::io::SecurePath::RemoveWithin(TrustedCacheRoot, Staging.lexically_relative(TrustedCacheRoot), true,
													   "Package cache rollback");
			}
			catch (...)
			{
			}
			throw;
		}
	}

	Mount.ContentRoot = CacheDirectory / "Content";
	const std::filesystem::path Startup = ResolveWithin(Mount.ContentRoot, Mount.StartupScene, "StartupScene");
	if (!std::filesystem::is_regular_file(Startup))
		throw ProjectPackageException("Package startup scene was not materialized");
	return Mount;
}
} // namespace runtime::project
