#include "CookPackageService.h"

#include "Source/core/io/CompressedArchive.h"
#include "Source/core/io/SecurePath.h"
#include "Source/editor/asset/AssetMetadata.h"
#include "Source/runtime/project/RuntimeSceneBinary.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <concepts>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <unordered_set>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace editor::cook
{
namespace
{
using Json = nlohmann::json;
constexpr std::array<uint8, 8> ChunkMagic{'O', 'G', 'L', 'C', 'H', 'N', 'K', 0};
constexpr uint32 ChunkFormatVersion = 1;
constexpr uint64 ChunkHeaderBytes = ChunkMagic.size() + sizeof(uint32) + sizeof(uint64) * 5U;
constexpr uint64 ChunkBuildIDOffset = ChunkMagic.size() + sizeof(uint32) + sizeof(uint64) * 3U;
constexpr usize MaximumCookedAssets = 1'000'000U;
// Zstandard's one-shot encode is not interruptible; this cap bounds one cancellation interval.
constexpr uint64 MaximumSourceFileBytes = 512ULL * 1'024ULL * 1'024ULL;
constexpr uint64 MaximumCookSourceBytes = 256ULL * 1'024ULL * 1'024ULL * 1'024ULL;
constexpr uint64 MaximumArchiveChunkBytes = 1ULL * 1'024ULL * 1'024ULL * 1'024ULL;
std::mutex SceneCookMutex;

class SHA256State final
{
  public:
	SHA256State()
	{
		if (BCryptOpenAlgorithmProvider(&this->Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			throw CookPackageException("Could not initialize package SHA-256 provider");
		ULONG ObjectBytes = 0;
		ULONG Returned = 0;
		if (BCryptGetProperty(this->Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes), &Returned,
							  0) < 0)
		{
			BCryptCloseAlgorithmProvider(this->Algorithm, 0);
			this->Algorithm = nullptr;
			throw CookPackageException("Could not query package SHA-256 provider");
		}
		this->Object.resize(ObjectBytes);
		if (BCryptCreateHash(this->Algorithm, &this->Hash, this->Object.data(), ObjectBytes, nullptr, 0, 0) < 0)
		{
			BCryptCloseAlgorithmProvider(this->Algorithm, 0);
			this->Algorithm = nullptr;
			throw CookPackageException("Could not create package SHA-256 state");
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

	void Update(const std::span<const uint8> Bytes)
	{
		for (usize Offset = 0; Offset < Bytes.size();)
		{
			const uint32 Chunk = static_cast<uint32>(std::min<usize>(Bytes.size() - Offset, std::numeric_limits<uint32>::max()));
			if (BCryptHashData(this->Hash, const_cast<PUCHAR>(Bytes.data() + Offset), Chunk, 0) < 0)
				throw CookPackageException("Could not update package SHA-256 digest");
			Offset += Chunk;
		}
	}

	[[nodiscard]] std::vector<uint8> Finish()
	{
		std::vector<uint8> Digest(32U);
		if (BCryptFinishHash(this->Hash, Digest.data(), static_cast<uint32>(Digest.size()), 0) < 0)
			throw CookPackageException("Could not calculate package SHA-256 digest");
		return Digest;
	}

  private:
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_HASH_HANDLE Hash = nullptr;
	std::vector<uint8> Object;
};

struct SecureFileIntegrity final
{
	uint64 Bytes = 0;
	uint64 Checksum = 14'695'981'039'346'656'037ULL;
	string SHA256;
};

[[nodiscard]] uint64 UpdateChecksum(uint64 Hash, const std::span<const uint8> Bytes) noexcept
{
	for (const uint8 Byte : Bytes)
	{
		Hash ^= Byte;
		Hash *= 1'099'511'628'211ULL;
	}
	return Hash;
}

[[nodiscard]] std::vector<uint8> ReadSecureFile(const std::filesystem::path &Root, const std::filesystem::path &Relative,
												const uint64 MaximumBytes, const string_view Role)
{
	try
	{
		return core::io::SecurePath::ReadFileWithin(Root, Relative, MaximumBytes, Role);
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw CookPackageException(string(Role) + " could not be read securely: " + Exception.what());
	}
}

[[nodiscard]] std::vector<uint8> SHA256(const std::span<const uint8> Bytes)
{
	SHA256State State;
	State.Update(Bytes);
	return State.Finish();
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

[[nodiscard]] SecureFileIntegrity InspectSecureFile(const std::filesystem::path &Root, const std::filesystem::path &Relative,
													const uint64 MaximumBytes, const string_view Role)
{
	SecureFileIntegrity Result;
	SHA256State Digest;
	try
	{
		Result.Bytes = core::io::SecurePath::ReadFileChunksWithin(
			Root, Relative, MaximumBytes,
			[&Result, &Digest](const std::span<const uint8> Chunk)
			{
				Result.Checksum = UpdateChecksum(Result.Checksum, Chunk);
				Digest.Update(Chunk);
			},
			Role);
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw CookPackageException(string(Role) + " could not be inspected securely: " + Exception.what());
	}
	Result.SHA256 = Hex(Digest.Finish());
	return Result;
}

[[nodiscard]] core::io::CompressedArchiveEncodeResult EncodeSecureFile(const std::filesystem::path &SourceRoot,
																	   const std::filesystem::path &SourceRelative,
																	   const uint64 SourceBytes,
																	   const std::filesystem::path &DestinationRoot,
																	   const std::filesystem::path &DestinationRelative,
																	   const int32 CompressionLevel, const string_view Role)
{
	core::io::SecurePath::WriteFileWithin(DestinationRoot, DestinationRelative, std::span<const uint8>{}, true, false, Role);
	const core::io::CompressedArchiveEncodeResult Result = core::io::CompressedArchive::EncodeStream(
		SourceBytes,
		[&SourceRoot, &SourceRelative, SourceBytes, Role](const uint64 Offset, const std::span<uint8> Destination)
		{
			const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileRangeWithin(
				SourceRoot, SourceRelative, Offset, static_cast<uint64>(Destination.size()), SourceBytes, Role);
			std::ranges::copy(Bytes, Destination.begin());
		},
		[&DestinationRoot, &DestinationRelative, Role](const uint64 Offset, const std::span<const uint8> Bytes)
		{ core::io::SecurePath::WriteFileAtWithin(DestinationRoot, DestinationRelative, Offset, Bytes, false, Role); }, CompressionLevel);
	core::io::SecurePath::WriteFileAtWithin(DestinationRoot, DestinationRelative, Result.ArchiveBytes, std::span<const uint8>{}, true,
											Role);
	return Result;
}

[[nodiscard]] core::io::CompressedArchiveEncodeResult EncodeBytes(const std::span<const uint8> Source,
																  const std::filesystem::path &DestinationRoot,
																  const std::filesystem::path &DestinationRelative,
																  const int32 CompressionLevel, const string_view Role)
{
	core::io::SecurePath::WriteFileWithin(DestinationRoot, DestinationRelative, std::span<const uint8>{}, true, false, Role);
	const core::io::CompressedArchiveEncodeResult Result = core::io::CompressedArchive::EncodeStream(
		Source.size(),
		[Source](const uint64 Offset, const std::span<uint8> Destination)
		{
			if (Offset > Source.size() || Destination.size() > Source.size() - static_cast<usize>(Offset))
				throw CookPackageException("Cooked-memory archive reader exceeded its source range");
			std::ranges::copy_n(Source.begin() + static_cast<usize>(Offset), Destination.size(), Destination.begin());
		},
		[&DestinationRoot, &DestinationRelative, Role](const uint64 Offset, const std::span<const uint8> Bytes)
		{ core::io::SecurePath::WriteFileAtWithin(DestinationRoot, DestinationRelative, Offset, Bytes, false, Role); }, CompressionLevel);
	core::io::SecurePath::WriteFileAtWithin(DestinationRoot, DestinationRelative, Result.ArchiveBytes, std::span<const uint8>{}, true,
											Role);
	return Result;
}

[[nodiscard]] bool ValidateCachedArchive(const std::filesystem::path &Root, const std::filesystem::path &Relative,
										 const uint64 ArchiveBytes, const SecureFileIntegrity &Expected)
{
	SHA256State Digest;
	try
	{
		const core::io::CompressedArchiveDecodeResult Result = core::io::CompressedArchive::DecodeStream(
			ArchiveBytes,
			[&Root, &Relative, ArchiveBytes](const uint64 Offset, const std::span<uint8> Destination)
			{
				const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileRangeWithin(
					Root, Relative, Offset, static_cast<uint64>(Destination.size()), ArchiveBytes, "Cook cache validation");
				std::ranges::copy(Bytes, Destination.begin());
			},
			[&Digest](const std::span<const uint8> Chunk) { Digest.Update(Chunk); }, Expected.Bytes);
		return Result.DecodedBytes == Expected.Bytes && Result.Checksum == Expected.Checksum && Hex(Digest.Finish()) == Expected.SHA256;
	}
	catch (const std::exception &)
	{
		return false;
	}
}

[[nodiscard]] string SignManifest(const Json &Manifest, const std::filesystem::path &PrivateKeyPath)
{
	const std::vector<uint8> KeyBlob =
		ReadSecureFile(PrivateKeyPath.parent_path(), PrivateKeyPath.filename(), 64U * 1'024U, "Package signing private key");
	if (KeyBlob.empty() || KeyBlob.size() > 64U * 1'024U)
		throw CookPackageException("Package signing private-key blob is invalid");
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_KEY_HANDLE Key = nullptr;
	if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
		BCryptImportKeyPair(Algorithm, nullptr, BCRYPT_RSAPRIVATE_BLOB, &Key, const_cast<PUCHAR>(KeyBlob.data()),
							static_cast<uint32>(KeyBlob.size()), 0) < 0)
		throw CookPackageException("Could not import package signing private key");
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
	} Guard{Algorithm, Key};
	const string Canonical = Manifest.dump();
	const std::vector<uint8> Digest = SHA256(std::span(reinterpret_cast<const uint8 *>(Canonical.data()), Canonical.size()));
	BCRYPT_PKCS1_PADDING_INFO Padding{BCRYPT_SHA256_ALGORITHM};
	ULONG SignatureBytes = 0;
	if (BCryptSignHash(Key, &Padding, const_cast<PUCHAR>(Digest.data()), static_cast<uint32>(Digest.size()), nullptr, 0, &SignatureBytes,
					   BCRYPT_PAD_PKCS1) < 0)
		throw CookPackageException("Could not size package manifest signature");
	std::vector<uint8> Signature(SignatureBytes);
	if (BCryptSignHash(Key, &Padding, const_cast<PUCHAR>(Digest.data()), static_cast<uint32>(Digest.size()), Signature.data(),
					   SignatureBytes, &SignatureBytes, BCRYPT_PAD_PKCS1) < 0)
		throw CookPackageException("Could not sign package manifest");
	Signature.resize(SignatureBytes);
	return Hex(Signature);
}

template <std::unsigned_integral ValueType> void AppendLittleEndian(std::vector<uint8> &Bytes, const ValueType Value)
{
	for (usize ByteIndex = 0; ByteIndex < sizeof(ValueType); ++ByteIndex)
		Bytes.push_back(static_cast<uint8>(Value >> (ByteIndex * 8U)));
}

[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path &Path)
{
	if (Path.empty() || Path.is_absolute())
		return false;
	for (const std::filesystem::path &Part : Path.lexically_normal())
	{
		if (Part == "..")
			return false;
	}
	return true;
}

[[nodiscard]] bool IsPathWithin(const std::filesystem::path &Root, const std::filesystem::path &Candidate)
{
	const std::filesystem::path NormalRoot = std::filesystem::absolute(Root).lexically_normal();
	const std::filesystem::path NormalCandidate = std::filesystem::absolute(Candidate).lexically_normal();
	auto RootPart = NormalRoot.begin();
	auto CandidatePart = NormalCandidate.begin();
	for (; RootPart != NormalRoot.end(); ++RootPart, ++CandidatePart)
	{
		if (CandidatePart == NormalCandidate.end() || *RootPart != *CandidatePart)
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

[[nodiscard]] resource::AssetID MakeStableAssetID(const util::UUID &ProjectID, const std::filesystem::path &LogicalPath)
{
	const string Key = NormalizePathKey(LogicalPath);
	uint64 Left = ProjectID.GetLeft() ^ 14'695'981'039'346'656'037ULL;
	uint64 Right = ProjectID.GetRight() ^ 1'099'511'628'211ULL;
	for (const uint8 Byte : std::span(reinterpret_cast<const uint8 *>(Key.data()), Key.size()))
	{
		Left = (Left ^ Byte) * 1'099'511'628'211ULL;
		Right ^= static_cast<uint64>(Byte) + 0x9e3779b97f4a7c15ULL + (Right << 6U) + (Right >> 2U);
	}
	return util::UUID(Left, Right).ToString();
}

[[nodiscard]] string SanitizeChunkName(const string_view Name)
{
	string Result;
	Result.reserve(Name.size());
	for (const char Character : Name)
		Result.push_back(std::isalnum(static_cast<unsigned char>(Character)) != 0 || Character == '-' || Character == '_' ? Character
																														  : '_');
	if (Result.empty())
		throw CookPackageException("Archive chunk name cannot be empty after path sanitization");
	uint64 Hash = 14'695'981'039'346'656'037ULL;
	for (const uint8 Byte : std::span(reinterpret_cast<const uint8 *>(Name.data()), Name.size()))
	{
		Hash ^= Byte;
		Hash *= 1'099'511'628'211ULL;
	}
	return std::format("{}-{:016x}", Result, Hash);
}

[[nodiscard]] bool IsReparsePoint(const std::filesystem::path &Path)
{
	const DWORD Attributes = GetFileAttributesW(Path.c_str());
	if (Attributes == INVALID_FILE_ATTRIBUTES)
		throw CookPackageException("Could not inspect package source attributes: '" + Path.string() + "'");
	return (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

class CookCacheLock final
{
  public:
	explicit CookCacheLock(const std::filesystem::path &Path)
	{
		uint64 Hash = 14'695'981'039'346'656'037ULL;
		for (const wchar_t Character : std::filesystem::absolute(Path).lexically_normal().native())
		{
			Hash ^= static_cast<uint64>(Character);
			Hash *= 1'099'511'628'211ULL;
		}
		const std::wstring Name = std::format(L"Local\\OpenFrameCookCache-{:016x}", Hash);
		this->Handle = CreateMutexW(nullptr, FALSE, Name.c_str());
		if (this->Handle == nullptr)
			throw CookPackageException("Could not create incremental cook-cache lock");
		const DWORD Wait = WaitForSingleObject(this->Handle, 30'000U);
		if (Wait != WAIT_OBJECT_0 && Wait != WAIT_ABANDONED)
		{
			CloseHandle(this->Handle);
			this->Handle = nullptr;
			throw CookPackageException("Timed out waiting for the incremental cook-cache lock");
		}
	}

	~CookCacheLock()
	{
		if (this->Handle != nullptr)
		{
			ReleaseMutex(this->Handle);
			CloseHandle(this->Handle);
		}
	}

	CookCacheLock(const CookCacheLock &) = delete;
	CookCacheLock &operator=(const CookCacheLock &) = delete;

  private:
	HANDLE Handle = nullptr;
};

void CopyRuntimeFile(const RuntimePackageFile &File, const std::filesystem::path &StagingDirectory)
{
	if (!std::filesystem::is_regular_file(File.Source))
		throw CookPackageException("Runtime package file does not exist: '" + File.Source.string() + "'");
	if (!IsSafeRelativePath(File.Destination))
		throw CookPackageException("Runtime package destination must be a safe relative path");
	core::io::SecurePath::CopyWithin(File.Source.parent_path(), File.Source.filename(), StagingDirectory, File.Destination, false, true,
									 "Runtime package staging");
}

void PublishJournal(const std::filesystem::path &Path, const std::filesystem::path &Staging, const std::filesystem::path &Backup,
					const string_view Phase)
{
	const Json Root{{"FormatVersion", 1U},
					{"Staging", Staging.filename().generic_string()},
					{"Backup", Backup.filename().generic_string()},
					{"Phase", Phase}};
	const std::filesystem::path Temporary = Path.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump() + '\n';
	core::io::SecurePath::WriteFileWithin(Path.parent_path(), Temporary,
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
										  "Package publication journal");
	core::io::SecurePath::ReplaceWithin(Path.parent_path(), Temporary, Path.filename(), "Package publication journal");
}

void RecoverPublication(const std::filesystem::path &Destination)
{
	const std::filesystem::path Journal = Destination.parent_path() / (Destination.filename().string() + ".publish.json");
	const std::filesystem::path PublicationRoot = Destination.parent_path();
	if (!std::filesystem::is_regular_file(Journal))
		return;
	const std::vector<uint8> JournalBytes =
		ReadSecureFile(PublicationRoot, Journal.filename(), 1024U * 1024U, "Package publication journal");
	const Json Root = Json::parse(JournalBytes.begin(), JournalBytes.end(), nullptr, true, true);
	const std::filesystem::path StagingName = Root.at("Staging").get<string>();
	const std::filesystem::path BackupName = Root.at("Backup").get<string>();
	if (Root.value("FormatVersion", 0U) != 1U || StagingName.has_parent_path() || BackupName.has_parent_path())
		throw CookPackageException("Package publication journal is invalid");
	const std::filesystem::path Staging = Destination.parent_path() / StagingName;
	const std::filesystem::path Backup = Destination.parent_path() / BackupName;
	std::error_code Error;
	if (!std::filesystem::exists(Destination, Error))
	{
		if (std::filesystem::exists(Backup, Error))
			core::io::SecurePath::MoveWithin(PublicationRoot, Backup.filename(), PublicationRoot, Destination.filename(), false,
											 "Package publication recovery");
		else if (std::filesystem::is_regular_file(Staging / "PackageManifest.json", Error))
			core::io::SecurePath::MoveWithin(PublicationRoot, Staging.filename(), PublicationRoot, Destination.filename(), false,
											 "Package publication recovery");
	}
	if (std::filesystem::exists(Staging))
		core::io::SecurePath::RemoveWithin(PublicationRoot, Staging.filename(), true, "Interrupted package staging");
	if (std::filesystem::exists(Backup))
		core::io::SecurePath::RemoveWithin(PublicationRoot, Backup.filename(), true, "Interrupted package backup");
	core::io::SecurePath::RemoveWithin(PublicationRoot, Journal.filename(), false, "Package publication recovery journal");
}
} // namespace

CookPackageService::CookPackageService() : OwnerThread(std::this_thread::get_id())
{
}

CookPackageService::~CookPackageService()
{
	this->Cancel();
	this->Wait();
	this->CleanupStaging();
}

void CookPackageService::Begin(const project::Project &Project, CookPackageSpecification Specification,
							   core::threading::TaskScheduler &Scheduler)
{
	this->VerifyOwnerThread();
	if (this->State == CookPackageState::Cooking || this->State == CookPackageState::Publishing)
		throw CookPackageException("A cook/package operation is already active");
	this->Reset();
	if (Specification.CompressionLevel < -5 || Specification.CompressionLevel > 22)
		throw CookPackageException("Zstandard compression level must be between -5 and 22");
	if (Specification.RequireSignedPackage && (Specification.SigningKeyID.empty() || Specification.SigningKeyVersion == 0 ||
											   !std::filesystem::is_regular_file(Specification.SigningPrivateKey)))
		throw CookPackageException("Signed packaging requires a key ID, non-zero key version, and CNG RSA private-key blob");
	if (Project.GetDescriptor().Cook.ArchiveChunkSizeBytes <= ChunkHeaderBytes ||
		Project.GetDescriptor().Cook.ArchiveChunkSizeBytes > MaximumArchiveChunkBytes)
		throw CookPackageException("Archive chunk size is outside the supported package budget");
	if (Specification.OutputDirectory.empty())
		Specification.OutputDirectory = Project.GetPaths().DevelopmentBuild / Project.GetDescriptor().Name;
	Specification.OutputDirectory = std::filesystem::absolute(Specification.OutputDirectory).lexically_normal();
	RecoverPublication(Specification.OutputDirectory);
	if (Specification.OutputDirectory == Project.GetPaths().Root ||
		IsPathWithin(Project.GetPaths().Content, Specification.OutputDirectory) ||
		IsPathWithin(Project.GetPaths().Source, Specification.OutputDirectory) ||
		IsPathWithin(Project.GetPaths().Intermediate, Specification.OutputDirectory))
		throw CookPackageException("Package output cannot replace a project source or working directory");
	if (!Specification.RuntimeFiles.empty())
	{
		if (Project.GetDescriptor().StartupScene.empty() ||
			!std::filesystem::is_regular_file(Project.ResolveContentPath(Project.GetDescriptor().StartupScene)))
		{
			throw CookPackageException("Runnable packaging requires an existing startup scene");
		}
		usize ExecutableCount = 0;
		usize GameModuleCount = 0;
		usize EngineContentCount = 0;
		std::unordered_set<string> Destinations;
		for (const RuntimePackageFile &File : Specification.RuntimeFiles)
		{
			if (!std::filesystem::is_regular_file(File.Source))
				throw CookPackageException("Runtime package file does not exist: '" + File.Source.string() + "'");
			if (!IsSafeRelativePath(File.Destination))
				throw CookPackageException("Runtime package destination must be a safe relative path");
			const string DestinationKey = NormalizePathKey(File.Destination);
			if (DestinationKey == "packagemanifest.json" || DestinationKey.starts_with("content/"))
				throw CookPackageException("Runtime package destination conflicts with package-owned files");
			if (!Destinations.emplace(DestinationKey).second)
				throw CookPackageException("Runtime package contains a duplicate destination path");
			switch (File.Kind)
			{
			case runtime::project::PackageFileKind::Executable:
				++ExecutableCount;
				break;
			case runtime::project::PackageFileKind::GameModule:
				++GameModuleCount;
				break;
			case runtime::project::PackageFileKind::EngineContent:
				++EngineContentCount;
				break;
			case runtime::project::PackageFileKind::DynamicLibrary:
				break;
			default:
				throw CookPackageException("Runtime package file kind is invalid");
			}
		}
		if (ExecutableCount != 1)
			throw CookPackageException("Runnable packaging requires exactly one executable");
		if (GameModuleCount > 1)
			throw CookPackageException("Runnable packaging cannot contain more than one game module");
		if (EngineContentCount == 0)
			throw CookPackageException("Runnable packaging requires engine shader content");
		std::ranges::sort(Specification.RuntimeFiles, [](const RuntimePackageFile &Left, const RuntimePackageFile &Right)
						  { return NormalizePathKey(Left.Destination) < NormalizePathKey(Right.Destination); });
	}

	this->ProjectDescriptor = Project.GetDescriptor();
	this->ProjectPaths = Project.GetPaths();
	this->Specification = std::move(Specification);
	this->OperationID = util::UUID::GenerateRandomUUID();
	this->StagingDirectory = this->Specification.OutputDirectory.parent_path() /
							 (this->Specification.OutputDirectory.filename().string() + ".staging-" + this->OperationID.ToString());
	if (std::filesystem::exists(this->StagingDirectory))
		throw CookPackageException("Unique package staging directory unexpectedly already exists");
	core::io::SecurePath::CreateTrustedRoot(this->Specification.OutputDirectory.parent_path(), "Package publication root");
	core::io::SecurePath::CreateDirectoriesWithin(
		this->Specification.OutputDirectory.parent_path(),
		(this->StagingDirectory / "Content").lexically_relative(this->Specification.OutputDirectory.parent_path()), "Package staging root");

	std::vector<std::filesystem::path> Sources;
	std::error_code Error;
	for (std::filesystem::recursive_directory_iterator
			 Iterator(this->ProjectPaths.Content, std::filesystem::directory_options::skip_permission_denied, Error),
		 End;
		 Iterator != End; Iterator.increment(Error))
	{
		if (Error)
			throw CookPackageException("Could not enumerate project content: " + Error.message());
		if (IsReparsePoint(Iterator->path()))
		{
			if (Iterator->is_directory())
				Iterator.disable_recursion_pending();
			throw CookPackageException("Project content contains a reparse point, which cannot be cooked securely: '" +
									   Iterator->path().string() + "'");
		}
		if (Iterator->is_regular_file() && NormalizePathKey(Iterator->path().extension()) != ".assetmeta")
			Sources.push_back(Iterator->path());
	}
	std::ranges::sort(Sources);
	if (Sources.size() > MaximumCookedAssets)
		throw CookPackageException("Project content exceeds the configured asset-count budget");
	uint64 TotalSourceBytes = 0;
	std::vector<uint64> SourceSizes(Sources.size());
	std::unordered_set<resource::AssetID> PreflightAssetIDs;
	std::vector<std::vector<resource::AssetID>> PreflightDependencies(Sources.size());
	for (usize Index = 0; Index < Sources.size(); ++Index)
	{
		std::error_code SizeError;
		const uint64 Bytes = std::filesystem::file_size(Sources[Index], SizeError);
		if (SizeError || Bytes > MaximumSourceFileBytes ||
			Bytes > MaximumCookSourceBytes - std::min(MaximumCookSourceBytes, TotalSourceBytes))
			throw CookPackageException("Project content exceeds the configured source-byte budget");
		TotalSourceBytes += Bytes;
		SourceSizes[Index] = Bytes;
		resource::AssetID AssetID =
			MakeStableAssetID(this->ProjectDescriptor.ID, Sources[Index].lexically_relative(this->ProjectPaths.Content));
		const std::filesystem::path Sidecar = asset::AssetMetadataStore::GetSidecarPath(Sources[Index]);
		if (std::filesystem::is_regular_file(Sidecar))
		{
			string Diagnostic;
			const std::optional<asset::AssetMetadata> Metadata = asset::AssetMetadataStore::TryLoad(Sidecar, Diagnostic);
			if (!Metadata.has_value())
				throw CookPackageException(Diagnostic);
			AssetID = Metadata->ID;
			PreflightDependencies[Index] = Metadata->Dependencies;
		}
		if (!util::UUID::TryParse(AssetID).has_value() || !PreflightAssetIDs.emplace(AssetID).second)
			throw CookPackageException("Project content contains an invalid or duplicate AssetID");
	}
	for (const std::vector<resource::AssetID> &Dependencies : PreflightDependencies)
	{
		for (const resource::AssetID &Dependency : Dependencies)
		{
			if (!PreflightAssetIDs.contains(Dependency))
				throw CookPackageException("Project content dependency closure is incomplete; dependency '" + Dependency + "' is missing");
		}
	}
	this->Entries.resize(Sources.size());
	this->TotalTasks = Sources.size() + this->Specification.RuntimeFiles.size();
	this->TotalWorkBytes = TotalSourceBytes;
	for (const RuntimePackageFile &RuntimeFile : this->Specification.RuntimeFiles)
	{
		std::error_code RuntimeSizeError;
		const uint64 RuntimeBytes = std::filesystem::file_size(RuntimeFile.Source, RuntimeSizeError);
		if (RuntimeSizeError || RuntimeBytes > MaximumSourceFileBytes ||
			RuntimeBytes > std::numeric_limits<uint64>::max() - this->TotalWorkBytes)
			throw CookPackageException("Runtime package input exceeds the configured byte budget");
		this->TotalWorkBytes += RuntimeBytes;
	}
	this->State = CookPackageState::Cooking;

	try
	{
		for (usize Index = 0; Index < Sources.size(); ++Index)
		{
			const std::filesystem::path Source = Sources[Index];
			const std::filesystem::path Relative = Source.lexically_relative(this->ProjectPaths.Content);
			if (!IsSafeRelativePath(Relative))
				throw CookPackageException("Enumerated content path escaped the project Content directory");
			const string VirtualPath = "/Game/" + Relative.generic_string();
			const runtime::project::ProjectArchiveChunk *AssignedChunk = nullptr;
			for (const runtime::project::ProjectArchiveChunk &Chunk : this->ProjectDescriptor.ArchiveChunks)
			{
				if (std::ranges::any_of(Chunk.VirtualRoots, [&VirtualPath](const string &Root)
										{ return VirtualPath == Root || VirtualPath.starts_with(Root + "/"); }))
				{
					AssignedChunk = &Chunk;
					break;
				}
			}
			if (AssignedChunk == nullptr)
				throw CookPackageException("No archive chunk owns cooked virtual path '" + VirtualPath + "'");

			resource::AssetID AssetID = MakeStableAssetID(this->ProjectDescriptor.ID, Relative);
			string AssetType = asset::AssetMetadataStore::InferAssetTypeName(Source).value_or("Binary");
			string SourceHash;
			std::vector<resource::AssetID> Dependencies;
			const std::filesystem::path Sidecar = asset::AssetMetadataStore::GetSidecarPath(Source);
			if (std::filesystem::is_regular_file(Sidecar))
			{
				string MetadataDiagnostic;
				const std::optional<asset::AssetMetadata> Metadata = asset::AssetMetadataStore::TryLoad(Sidecar, MetadataDiagnostic);
				if (!Metadata.has_value())
					throw CookPackageException(MetadataDiagnostic);
				AssetID = Metadata->ID;
				AssetType = Metadata->AssetType;
				SourceHash = Metadata->SourceHash;
				Dependencies = Metadata->Dependencies;
			}
			const std::filesystem::path TemporaryArchive =
				std::filesystem::path("Content") / ".CookEntries" / (std::to_string(Index) + ".oglarchive");
			this->Entries[Index] = {.LogicalPath = Relative,
									.ArchivePath = TemporaryArchive,
									.AssetID = std::move(AssetID),
									.AssetType = std::move(AssetType),
									.SourceHash = std::move(SourceHash),
									.Chunk = AssignedChunk->Name,
									.Dependencies = std::move(Dependencies)};
			this->Tasks.Run(
				Scheduler,
				[this, Index, Relative, WorkBytes = SourceSizes[Index]]()
				{
					if (this->CancelRequested.load(std::memory_order_acquire))
						throw CookPackageCancelledException("Cook/package operation was cancelled");
					const bool IsScene = NormalizePathKey(this->Entries[Index].LogicalPath.extension()) == ".enginelevel";
					const SecureFileIntegrity SourceIntegrity =
						InspectSecureFile(this->ProjectPaths.Content, Relative, MaximumSourceFileBytes, "Cook source asset");
					if (this->CancelRequested.load(std::memory_order_acquire))
						throw CookPackageCancelledException("Cook/package operation was cancelled");
					const std::filesystem::path Destination = this->StagingDirectory / this->Entries[Index].ArchivePath;
					const std::filesystem::path DestinationRelative = Destination.lexically_relative(this->StagingDirectory);
					const string GenerationHash = std::format("{:016x}", SourceIntegrity.Checksum);
					std::vector<uint8> CookedScene;
					std::unique_lock<std::mutex> SceneLock;
					SecureFileIntegrity CookedIntegrity = SourceIntegrity;
					if (IsScene)
					{
						SceneLock = std::unique_lock(SceneCookMutex);
						const std::vector<uint8> SourceBytes =
							ReadSecureFile(this->ProjectPaths.Content, Relative, MaximumSourceFileBytes, "Cook scene source");
						if (core::io::CompressedArchive::CalculateChecksum(SourceBytes) != SourceIntegrity.Checksum ||
							Hex(SHA256(SourceBytes)) != SourceIntegrity.SHA256)
						{
							throw CookPackageException("Cook scene source changed while it was being prepared");
						}
						CookedScene = runtime::project::RuntimeSceneBinary::Compile(SourceBytes);
						CookedIntegrity.Bytes = CookedScene.size();
						CookedIntegrity.Checksum = core::io::CompressedArchive::CalculateChecksum(CookedScene);
						CookedIntegrity.SHA256 = Hex(SHA256(CookedScene));
					}
					bool CacheHit = false;
					if (this->Specification.UseIncrementalCache)
					{
						const std::filesystem::path CachePath =
							this->ProjectPaths.Intermediate / "CookCache" /
							(std::format("{}-{}-L{}-S{}.oglarchive", this->Entries[Index].AssetID, GenerationHash,
										 this->Specification.CompressionLevel, runtime::project::RuntimeSceneBinary::FormatVersion));
						const CookCacheLock CacheLock(CachePath);
						if (std::filesystem::is_regular_file(CachePath))
						{
							std::error_code SizeError;
							const uint64 CacheBytes = std::filesystem::file_size(CachePath, SizeError);
							CacheHit = !SizeError && CacheBytes <= MaximumArchiveChunkBytes &&
									   ValidateCachedArchive(CachePath.parent_path(), CachePath.filename(), CacheBytes, CookedIntegrity);
						}
						if (!CacheHit)
						{
							core::io::SecurePath::CreateTrustedRoot(CachePath.parent_path(), "Cook cache root");
							const std::filesystem::path Temporary =
								CachePath.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
							try
							{
								const core::io::CompressedArchiveEncodeResult Encoded =
									IsScene ? EncodeBytes(CookedScene, CachePath.parent_path(), Temporary,
														  this->Specification.CompressionLevel, "Cook cache temporary archive")
											: EncodeSecureFile(this->ProjectPaths.Content, Relative, SourceIntegrity.Bytes,
															   CachePath.parent_path(), Temporary, this->Specification.CompressionLevel,
															   "Cook cache temporary archive");
								if (Encoded.SourceBytes != CookedIntegrity.Bytes || Encoded.Checksum != CookedIntegrity.Checksum)
									throw CookPackageException("Cook source changed during streaming compression");
								if (!ValidateCachedArchive(CachePath.parent_path(), Temporary, Encoded.ArchiveBytes, CookedIntegrity))
									throw CookPackageException("Cook cache archive did not verify before publication");
								core::io::SecurePath::ReplaceWithin(CachePath.parent_path(), Temporary, CachePath.filename(),
																	"Cook cache publication");
							}
							catch (...)
							{
								try
								{
									core::io::SecurePath::RemoveWithin(CachePath.parent_path(), Temporary, false,
																	   "Cook cache temporary cleanup");
								}
								catch (...)
								{
								}
								throw;
							}
						}
						core::io::SecurePath::CopyWithin(CachePath.parent_path(), CachePath.filename(), this->StagingDirectory,
														 Destination.lexically_relative(this->StagingDirectory), false, true,
														 "Incremental cooked archive staging");
					}
					else
					{
						const core::io::CompressedArchiveEncodeResult Encoded =
							IsScene ? EncodeBytes(CookedScene, this->StagingDirectory, DestinationRelative,
												  this->Specification.CompressionLevel, "Cooked scene archive staging")
									: EncodeSecureFile(this->ProjectPaths.Content, Relative, SourceIntegrity.Bytes, this->StagingDirectory,
													   DestinationRelative, this->Specification.CompressionLevel, "Cooked archive staging");
						if (Encoded.SourceBytes != CookedIntegrity.Bytes || Encoded.Checksum != CookedIntegrity.Checksum)
							throw CookPackageException("Cook source changed during streaming compression");
						if (!ValidateCachedArchive(this->StagingDirectory, DestinationRelative, Encoded.ArchiveBytes, CookedIntegrity))
							throw CookPackageException("Cooked archive did not verify before staging");
					}
					const uint64 ArchiveBytes = std::filesystem::file_size(Destination);
					CookedContentEntry &Entry = this->Entries[Index];
					Entry.SourceBytes = CookedIntegrity.Bytes;
					Entry.OriginalSourceBytes = SourceIntegrity.Bytes;
					Entry.ArchiveBytes = ArchiveBytes;
					Entry.ContentChecksum = CookedIntegrity.Checksum;
					Entry.ContentSHA256 = CookedIntegrity.SHA256;
					Entry.Encoding = IsScene ? "SceneCBOR" : "Raw";
					if (Entry.SourceHash.empty())
						Entry.SourceHash = GenerationHash;
					this->CompletedTasks.fetch_add(1, std::memory_order_release);
					this->CompletedWorkBytes.fetch_add(WorkBytes, std::memory_order_release);
				},
				core::threading::TaskPriority::Background);
		}
		for (const RuntimePackageFile &RuntimeFile : this->Specification.RuntimeFiles)
		{
			this->Tasks.Run(
				Scheduler,
				[this, RuntimeFile]()
				{
					if (this->CancelRequested.load(std::memory_order_acquire))
						throw CookPackageCancelledException("Cook/package operation was cancelled");
					CopyRuntimeFile(RuntimeFile, this->StagingDirectory);
					if (this->CancelRequested.load(std::memory_order_acquire))
						throw CookPackageCancelledException("Cook/package operation was cancelled");
					this->CompletedWorkBytes.fetch_add(std::filesystem::file_size(RuntimeFile.Source), std::memory_order_release);
					this->CompletedTasks.fetch_add(1, std::memory_order_release);
				},
				core::threading::TaskPriority::Background);
		}
	}
	catch (...)
	{
		const std::exception_ptr SchedulingFailure = std::current_exception();
		this->CancelRequested.store(true, std::memory_order_release);
		try
		{
			this->Tasks.Wait();
		}
		catch (...)
		{
		}
		try
		{
			std::rethrow_exception(SchedulingFailure);
		}
		catch (const std::exception &Exception)
		{
			this->Diagnostic = Exception.what();
		}
		catch (...)
		{
			this->Diagnostic = "Cook/package task scheduling failed with a non-standard exception";
		}
		this->State = CookPackageState::Failed;
		this->CleanupStaging();
		std::rethrow_exception(SchedulingFailure);
	}
	if (this->TotalTasks == 0)
		this->Finalize();
}

bool CookPackageService::Poll()
{
	this->VerifyOwnerThread();
	if (this->State != CookPackageState::Cooking)
		return false;
	if (!this->Tasks.IsComplete())
		return false;
	try
	{
		this->Tasks.Wait();
		if (this->CancelRequested.load(std::memory_order_acquire))
			throw CookPackageCancelledException("Cook/package operation was cancelled");
		this->Finalize();
	}
	catch (const CookPackageCancelledException &Exception)
	{
		this->Diagnostic = Exception.what();
		this->State = CookPackageState::Cancelled;
		this->CleanupStaging();
	}
	catch (...)
	{
		this->FailFromCurrentException();
	}
	return true;
}

void CookPackageService::Cancel() noexcept
{
	this->CancelRequested.store(true, std::memory_order_release);
}

void CookPackageService::Wait() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	if (this->State != CookPackageState::Cooking)
		return;
	try
	{
		this->Tasks.Wait();
		if (this->CancelRequested.load(std::memory_order_acquire))
		{
			this->Diagnostic = "Cook/package operation was cancelled";
			this->State = CookPackageState::Cancelled;
			this->CleanupStaging();
		}
		else
			this->Finalize();
	}
	catch (...)
	{
		this->FailFromCurrentException();
	}
}

void CookPackageService::Reset()
{
	this->VerifyOwnerThread();
	if (this->State == CookPackageState::Cooking || this->State == CookPackageState::Publishing)
		throw CookPackageException("Cannot reset an active cook/package operation");
	this->CleanupStaging();
	this->ProjectDescriptor = {};
	this->ProjectPaths = {};
	this->Specification = {};
	this->State = CookPackageState::Idle;
	this->OperationID = {};
	this->Entries.clear();
	this->Tasks = core::threading::TaskGroup{};
	this->CompletedTasks.store(0, std::memory_order_release);
	this->CompletedWorkBytes.store(0, std::memory_order_release);
	this->CancelRequested.store(false, std::memory_order_release);
	this->TotalTasks = 0;
	this->TotalWorkBytes = 0;
	this->Diagnostic.clear();
	this->Result.reset();
}

CookPackageState CookPackageService::GetState() const noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	return this->State;
}

float32 CookPackageService::GetProgress() const noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	if (this->State == CookPackageState::Completed)
		return 1.0f;
	if (this->TotalWorkBytes == 0)
		return 0.0f;
	const float32 WorkProgress =
		static_cast<float32>(this->CompletedWorkBytes.load(std::memory_order_acquire)) / static_cast<float32>(this->TotalWorkBytes);
	return this->State == CookPackageState::Publishing ? 0.95f : std::min(WorkProgress * 0.95f, 0.95f);
}

string CookPackageService::GetDiagnostic() const
{
	this->VerifyOwnerThread();
	return this->Diagnostic;
}

std::optional<CookPackageResult> CookPackageService::GetResult() const
{
	this->VerifyOwnerThread();
	return this->Result;
}

void CookPackageService::VerifyOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw CookPackageException("Cook/package service must be accessed from its owner thread");
}

void CookPackageService::Finalize()
{
	this->State = CookPackageState::Publishing;
	if (this->CancelRequested.load(std::memory_order_acquire))
		throw CookPackageCancelledException("Cook/package operation was cancelled before publication");
	uint64 SourceBytes = 0;
	uint64 PackageBytes = 0;
	std::unordered_set<resource::AssetID> AssetIDs;
	std::map<string, std::vector<usize>> ChunkEntries;
	for (usize Index = 0; Index < this->Entries.size(); ++Index)
	{
		CookedContentEntry &Entry = this->Entries[Index];
		if (!util::UUID::TryParse(Entry.AssetID).has_value() || !AssetIDs.emplace(Entry.AssetID).second)
			throw CookPackageException("Cooked content contains an invalid or duplicate AssetID");
		for (const resource::AssetID &Dependency : Entry.Dependencies)
		{
			if (!util::UUID::TryParse(Dependency).has_value())
				throw CookPackageException("Cooked content dependency is not a canonical AssetID");
		}
		ChunkEntries[Entry.Chunk].push_back(Index);
	}

	const std::filesystem::path ChunkDirectory = this->StagingDirectory / "Content" / "Chunks";
	core::io::SecurePath::CreateDirectoriesWithin(this->StagingDirectory, ChunkDirectory.lexically_relative(this->StagingDirectory),
												  "Cooked chunk staging");
	for (const auto &[ChunkName, Indices] : ChunkEntries)
	{
		if (this->CancelRequested.load(std::memory_order_acquire))
			throw CookPackageCancelledException("Cook/package operation was cancelled while assembling chunks");
		usize First = 0;
		uint32 Segment = 0;
		while (First < Indices.size())
		{
			usize End = First;
			uint64 SegmentBytes = ChunkHeaderBytes;
			while (End < Indices.size())
			{
				const uint64 CandidateBytes = this->Entries[Indices[End]].ArchiveBytes;
				const uint64 ChunkLimit = this->ProjectDescriptor.Cook.ArchiveChunkSizeBytes;
				if (CandidateBytes > ChunkLimit - ChunkHeaderBytes)
					throw CookPackageException("Cooked asset exceeds the configured archive-chunk payload limit");
				if (End != First && (SegmentBytes >= ChunkLimit || CandidateBytes > ChunkLimit - SegmentBytes))
					break;
				if (CandidateBytes > std::numeric_limits<uint64>::max() - SegmentBytes)
					throw CookPackageException("Cooked archive chunk size overflowed");
				SegmentBytes += CandidateBytes;
				++End;
				if (SegmentBytes >= this->ProjectDescriptor.Cook.ArchiveChunkSizeBytes)
					break;
			}
			const std::filesystem::path RelativeChunk =
				std::filesystem::path("Content") / "Chunks" / (std::format("{}-{:03}.oglpkg", SanitizeChunkName(ChunkName), Segment));
			std::vector<uint8> Header(ChunkMagic.begin(), ChunkMagic.end());
			Header.reserve(static_cast<usize>(ChunkHeaderBytes));
			AppendLittleEndian(Header, ChunkFormatVersion);
			AppendLittleEndian(Header, static_cast<uint64>(End - First));
			AppendLittleEndian(Header, this->ProjectDescriptor.ID.GetLeft());
			AppendLittleEndian(Header, this->ProjectDescriptor.ID.GetRight());
			AppendLittleEndian(Header, uint64{0});
			AppendLittleEndian(Header, uint64{0});
			core::io::SecurePath::WriteFileWithin(this->StagingDirectory, RelativeChunk, Header, false, false,
												  "Cooked archive chunk header");
			uint64 Offset = ChunkHeaderBytes;
			for (usize Position = First; Position < End; ++Position)
			{
				CookedContentEntry &Entry = this->Entries[Indices[Position]];
				const std::filesystem::path TemporaryArchive = Entry.ArchivePath;
				uint64 CopiedBytes = 0;
				const uint64 ReadBytes = core::io::SecurePath::ReadFileChunksWithin(
					this->StagingDirectory, TemporaryArchive, Entry.ArchiveBytes,
					[this, &RelativeChunk, &Offset, &CopiedBytes](const std::span<const uint8> Chunk)
					{
						if (this->CancelRequested.load(std::memory_order_acquire))
							throw CookPackageCancelledException("Cook/package operation was cancelled while streaming an archive chunk");
						core::io::SecurePath::WriteFileAtWithin(this->StagingDirectory, RelativeChunk, Offset + CopiedBytes, Chunk, false,
																"Cooked archive chunk payload");
						CopiedBytes += Chunk.size();
					},
					"Temporary cooked archive");
				if (ReadBytes != Entry.ArchiveBytes || CopiedBytes != Entry.ArchiveBytes)
					throw CookPackageException("Temporary cooked archive size changed before publication");
				Entry.ArchivePath = RelativeChunk;
				Entry.ArchiveOffset = Offset;
				Offset += Entry.ArchiveBytes;
			}
			core::io::SecurePath::WriteFileAtWithin(this->StagingDirectory, RelativeChunk, Offset, std::span<const uint8>{}, true,
													"Cooked archive chunk durability");
			PackageBytes += Offset;
			First = End;
			++Segment;
		}
	}
	core::io::SecurePath::RemoveWithin(this->StagingDirectory, std::filesystem::path("Content") / ".CookEntries", true,
									   "Temporary cooked archives");

	Json Content = Json::array();
	for (const CookedContentEntry &Entry : this->Entries)
	{
		SourceBytes += Entry.SourceBytes;
		Content.push_back({{"LogicalPath", Entry.LogicalPath.generic_string()},
						   {"ArchivePath", Entry.ArchivePath.generic_string()},
						   {"ArchiveOffset", Entry.ArchiveOffset},
						   {"Chunk", Entry.Chunk},
						   {"AssetID", Entry.AssetID},
						   {"AssetType", Entry.AssetType},
						   {"Dependencies", Entry.Dependencies},
						   {"SourceHash", Entry.SourceHash},
						   {"Encoding", Entry.Encoding},
						   {"SourceBytes", Entry.SourceBytes},
						   {"OriginalSourceBytes", Entry.OriginalSourceBytes},
						   {"ArchiveBytes", Entry.ArchiveBytes},
						   {"ContentChecksum", Entry.ContentChecksum}});
		Content.back()["ContentSHA256"] = Entry.ContentSHA256;
	}
	const string ContentIndexText = Content.dump();
	const uint64 ContentIndexChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(ContentIndexText.data()), ContentIndexText.size()));
	const auto StartupEntry = std::ranges::find(this->Entries, this->ProjectDescriptor.StartupScene, &CookedContentEntry::LogicalPath);
	if (!this->Specification.RuntimeFiles.empty() && (StartupEntry == this->Entries.end() || StartupEntry->Encoding != "SceneCBOR"))
		throw CookPackageException("Packaged startup scene is missing or was not compiled to the runtime scene format");
	Json Runtime = Json::array();
	for (const RuntimePackageFile &File : this->Specification.RuntimeFiles)
	{
		if (this->CancelRequested.load(std::memory_order_acquire))
			throw CookPackageCancelledException("Cook/package operation was cancelled while hashing runtime files");
		const SecureFileIntegrity Integrity =
			InspectSecureFile(this->StagingDirectory, File.Destination, MaximumSourceFileBytes, "Staged runtime package file");
		Runtime.push_back({{"Path", File.Destination.generic_string()},
						   {"Kind", static_cast<uint32>(File.Kind)},
						   {"Bytes", Integrity.Bytes},
						   {"Checksum", Integrity.Checksum},
						   {"SHA256", Integrity.SHA256}});
	}
	const string RuntimeIndexText = Runtime.dump();
	const uint64 RuntimeIndexChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(RuntimeIndexText.data()), RuntimeIndexText.size()));
	const util::UUID BuildID((this->ProjectDescriptor.ID.GetLeft() ^ ContentIndexChecksum ^ RuntimeIndexChecksum) | 1ULL,
							 (this->ProjectDescriptor.ID.GetRight() ^ ContentIndexChecksum ^ (RuntimeIndexChecksum << 1U)) | 1ULL);
	std::unordered_set<string> PatchedChunks;
	for (const CookedContentEntry &Entry : this->Entries)
	{
		if (!PatchedChunks.emplace(NormalizePathKey(Entry.ArchivePath)).second)
			continue;
		std::vector<uint8> Identity;
		Identity.reserve(sizeof(uint64) * 2U);
		AppendLittleEndian(Identity, BuildID.GetLeft());
		AppendLittleEndian(Identity, BuildID.GetRight());
		core::io::SecurePath::WriteFileAtWithin(this->StagingDirectory, Entry.ArchivePath, ChunkBuildIDOffset, Identity, true,
												"Cooked archive build identity");
	}
	const auto GameModule =
		std::ranges::find(this->Specification.RuntimeFiles, runtime::project::PackageFileKind::GameModule, &RuntimePackageFile::Kind);
	Json Manifest{{"FormatVersion", runtime::project::ProjectPackageFormatVersion},
				  {"OperationID", BuildID.ToString()},
				  {"BuildID", BuildID.ToString()},
				  {"ProjectID", this->ProjectDescriptor.ID.ToString()},
				  {"ProjectName", this->ProjectDescriptor.Name},
				  {"StartupScene", this->ProjectDescriptor.StartupScene.generic_string()},
				  {"GameModule",
				   GameModule == this->Specification.RuntimeFiles.end() ? Json(nullptr) : Json(GameModule->Destination.generic_string())},
				  {"Compression", "Zstandard"},
				  {"ContentIndexChecksum", ContentIndexChecksum},
				  {"RuntimeIndexChecksum", RuntimeIndexChecksum},
				  {"EngineSchemaVersion", this->ProjectDescriptor.EngineSchemaVersion},
				  {"Cook",
				   {{"CompressionLevel", this->Specification.CompressionLevel},
					{"ArchiveChunkSizeBytes", this->ProjectDescriptor.Cook.ArchiveChunkSizeBytes},
					{"Deterministic", this->ProjectDescriptor.Cook.Deterministic}}},
				  {"Content", std::move(Content)},
				  {"RuntimeFiles", std::move(Runtime)}};
	if (this->Specification.RequireSignedPackage)
	{
		const string Signature = SignManifest(Manifest, this->Specification.SigningPrivateKey);
		Manifest["Signature"] = {{"Algorithm", "RSA-PKCS1-SHA256"},
								 {"KeyID", this->Specification.SigningKeyID},
								 {"KeyVersion", this->Specification.SigningKeyVersion},
								 {"Value", Signature}};
	}
	const string ManifestText = Manifest.dump(2) + '\n';
	core::io::SecurePath::WriteFileWithin(this->StagingDirectory, "PackageManifest.json",
										  std::span(reinterpret_cast<const uint8 *>(ManifestText.data()), ManifestText.size()), false, true,
										  "Package manifest");

	const std::filesystem::path Destination = this->Specification.OutputDirectory;
	const std::filesystem::path Backup =
		Destination.parent_path() / (Destination.filename().string() + ".backup-" + this->OperationID.ToString());
	const std::filesystem::path Journal = Destination.parent_path() / (Destination.filename().string() + ".publish.json");
	const std::filesystem::path PublicationRoot = Destination.parent_path();
	PublishJournal(Journal, this->StagingDirectory, Backup, "Ready");
	if (std::filesystem::exists(Destination))
	{
		if (!this->Specification.ReplaceExisting)
			throw CookPackageException("Package output already exists and replacement is disabled");
		core::io::SecurePath::MoveWithin(PublicationRoot, Destination.filename(), PublicationRoot, Backup.filename(), false,
										 "Package backup publication");
		PublishJournal(Journal, this->StagingDirectory, Backup, "PreviousPreserved");
	}
	try
	{
		core::io::SecurePath::MoveWithin(PublicationRoot, this->StagingDirectory.filename(), PublicationRoot, Destination.filename(), false,
										 "Package publication");
	}
	catch (...)
	{
		if (std::filesystem::exists(Backup))
			core::io::SecurePath::MoveWithin(PublicationRoot, Backup.filename(), PublicationRoot, Destination.filename(), false,
											 "Package publication rollback");
		throw;
	}
	if (std::filesystem::exists(Backup))
		core::io::SecurePath::RemoveWithin(PublicationRoot, Backup.filename(), true, "Retired package backup");
	if (std::filesystem::exists(Journal))
		core::io::SecurePath::RemoveWithin(PublicationRoot, Journal.filename(), false, "Package publication journal");
	this->StagingDirectory.clear();
	this->Result = CookPackageResult{.OperationID = this->OperationID,
									 .BuildID = BuildID,
									 .PackageDirectory = Destination,
									 .Content = this->Entries,
									 .SourceBytes = SourceBytes,
									 .PackageBytes = PackageBytes};
	this->State = CookPackageState::Completed;
}

void CookPackageService::FailFromCurrentException() noexcept
{
	try
	{
		throw;
	}
	catch (const std::exception &Exception)
	{
		this->Diagnostic = Exception.what();
	}
	catch (...)
	{
		this->Diagnostic = "Cook/package operation failed with a non-standard exception";
	}
	this->State = CookPackageState::Failed;
	this->CleanupStaging();
}

void CookPackageService::CleanupStaging() noexcept
{
	if (this->StagingDirectory.empty())
		return;
	try
	{
		const std::filesystem::path Root = this->StagingDirectory.parent_path();
		if (std::filesystem::exists(this->StagingDirectory))
			core::io::SecurePath::RemoveWithin(Root, this->StagingDirectory.filename(), true, "Cook staging cleanup");
	}
	catch (...)
	{
	}
	this->StagingDirectory.clear();
}
} // namespace editor::cook
