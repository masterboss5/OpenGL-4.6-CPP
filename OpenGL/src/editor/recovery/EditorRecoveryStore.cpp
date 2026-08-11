#include "EditorRecoveryStore.h"

#include "src/editor/document/SceneDocument.h"
#include "src/editor/reflection/ReflectionRegistry.h"
#include "src/editor/serialization/SceneDocumentSerializer.h"
#include "src/core/io/CompressedArchive.h"
#include "src/core/io/SecurePath.h"
#include "src/resource/asset/AssetManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::recovery
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] bool IsWithin(const std::filesystem::path &Root, const std::filesystem::path &Candidate)
{
	const std::filesystem::path NormalRoot = std::filesystem::absolute(Root).lexically_normal();
	const std::filesystem::path NormalCandidate = std::filesystem::absolute(Candidate).lexically_normal();
	auto CandidatePart = NormalCandidate.begin();
	for (auto RootPart = NormalRoot.begin(); RootPart != NormalRoot.end(); ++RootPart, ++CandidatePart)
	{
		if (CandidatePart == NormalCandidate.end() || *RootPart != *CandidatePart)
			return false;
	}
	return true;
}

[[nodiscard]] uint64 CalculateFileChecksum(const std::filesystem::path &Path)
{
	constexpr uint64 MaximumRecoverySnapshotBytes = 256U * 1024U * 1024U;
	uint64 Hash = 14'695'981'039'346'656'037ULL;
	try
	{
		(void)core::io::SecurePath::ReadFileChunksWithin(
			Path.parent_path(), Path.filename(), MaximumRecoverySnapshotBytes,
			[&Hash](const std::span<const uint8> Bytes)
			{
				for (const uint8 Byte : Bytes)
				{
					Hash ^= Byte;
					Hash *= 1'099'511'628'211ULL;
				}
			},
			"Recovery snapshot checksum");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw EditorRecoveryException("Could not securely verify recovery snapshot: " + string(Exception.what()));
	}
	return Hash;
}

void ValidateSnapshotMetadata(const EditorRecoveryCandidate &Candidate)
{
	constexpr uint64 MaximumRecoverySnapshotBytes = 256U * 1024U * 1024U;
	const std::vector<uint8> Bytes =
		core::io::SecurePath::ReadFileWithin(Candidate.SnapshotPath.parent_path(), Candidate.SnapshotPath.filename(),
											 MaximumRecoverySnapshotBytes, "Recovery snapshot metadata");
	Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
	if (!Root.is_object() || !Root.contains("Recovery") || !Root.at("Recovery").is_object())
		throw EditorRecoveryException("Recovery snapshot metadata is missing");
	const Json Recovery = Root.at("Recovery");
	if (Recovery.value("DocumentID", string{}) != Candidate.DocumentID.ToString() ||
		Recovery.value("Revision", uint64{0}) != Candidate.Revision ||
		Recovery.value("TimestampMilliseconds", int64{0}) != Candidate.TimestampMilliseconds)
		throw EditorRecoveryException("Recovery snapshot metadata does not match its manifest");
	const uint64 ExpectedChecksum = Recovery.value("ContentChecksum", uint64{0});
	Root.erase("Recovery");
	const string CanonicalContent = Root.dump();
	const uint64 ActualChecksum = core::io::CompressedArchive::CalculateChecksum(
		std::span(reinterpret_cast<const uint8 *>(CanonicalContent.data()), CanonicalContent.size()));
	if (ExpectedChecksum == 0 || ActualChecksum != ExpectedChecksum)
		throw EditorRecoveryException("Recovery snapshot semantic checksum validation failed");
}

void WriteManifest(const EditorRecoveryCandidate &Candidate, const std::filesystem::path &RecoveryRoot, const std::filesystem::path &Path)
{
	const Json Root{{"FormatVersion", 2U},
					{"DocumentID", Candidate.DocumentID.ToString()},
					{"DocumentName", Candidate.DocumentName},
					{"SnapshotFile", Candidate.SnapshotPath.filename().generic_string()},
					{"OriginalPath", Candidate.OriginalPath.generic_string()},
					{"Revision", Candidate.Revision},
					{"TimestampMilliseconds", Candidate.TimestampMilliseconds},
					{"SnapshotChecksum", Candidate.SnapshotChecksum}};
	core::io::SecurePath::VerifyContained(RecoveryRoot, Path, "Recovery manifest");
	const std::filesystem::path TemporaryRelative = Path.lexically_relative(RecoveryRoot).parent_path() /
													(Path.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString());
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::WriteFileWithin(RecoveryRoot, TemporaryRelative,
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), false, true,
										  "Recovery manifest temporary file");
	core::io::SecurePath::ReplaceWithin(RecoveryRoot, TemporaryRelative, Path.lexically_relative(RecoveryRoot),
										"Recovery manifest publication");
}

[[nodiscard]] EditorRecoveryCandidate ReadManifest(const std::filesystem::path &Path, const std::filesystem::path &AutosaveRoot)
{
	constexpr uint64 MaximumRecoveryManifestBytes = 1024U * 1024U;
	const std::vector<uint8> Bytes =
		core::io::SecurePath::ReadFileWithin(Path.parent_path(), Path.filename(), MaximumRecoveryManifestBytes, "Recovery manifest");
	const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
	if (!Root.is_object() || Root.value("FormatVersion", 0U) != 2U)
		throw EditorRecoveryException("Recovery manifest has an unsupported format");
	const util::UUID DocumentID = util::UUID::Parse(Root.at("DocumentID").get<string>());
	const std::filesystem::path ExpectedFile = DocumentID.ToString() + ".autosave.scene";
	const std::filesystem::path StoredFile = Root.at("SnapshotFile").get<string>();
	if (StoredFile != ExpectedFile || StoredFile.has_parent_path() || StoredFile.is_absolute())
		throw EditorRecoveryException("Recovery manifest snapshot identity is invalid");
	EditorRecoveryCandidate Candidate{.DocumentID = DocumentID,
									  .DocumentName = Root.at("DocumentName").get<string>(),
									  .SnapshotPath = AutosaveRoot / ExpectedFile,
									  .OriginalPath = Root.value("OriginalPath", string{}),
									  .Revision = Root.at("Revision").get<uint64>(),
									  .TimestampMilliseconds = Root.at("TimestampMilliseconds").get<int64>(),
									  .SnapshotChecksum = Root.at("SnapshotChecksum").get<uint64>()};
	if (Candidate.DocumentName.empty() || Candidate.Revision == 0 || Candidate.TimestampMilliseconds <= 0 ||
		Candidate.SnapshotChecksum == 0)
		throw EditorRecoveryException("Recovery manifest contains an invalid candidate");
	return Candidate;
}
} // namespace

EditorRecoveryStore::EditorRecoveryStore(std::filesystem::path AutosaveRoot, std::filesystem::path RecoveryRoot,
										 EditorRecoverySpecification Specification)
	: AutosaveRoot(std::filesystem::absolute(std::move(AutosaveRoot)).lexically_normal()),
	  RecoveryRoot(std::filesystem::absolute(std::move(RecoveryRoot)).lexically_normal()), Specification(Specification)
{
	if (this->AutosaveRoot.empty() || this->RecoveryRoot.empty())
		throw std::invalid_argument("Editor recovery storage requires autosave and recovery roots");
	if (this->Specification.AutosaveInterval <= std::chrono::seconds::zero() ||
		this->Specification.QuietPeriod < std::chrono::seconds::zero())
	{
		throw std::invalid_argument("Editor recovery timing must be positive");
	}
	core::io::SecurePath::CreateTrustedRoot(this->AutosaveRoot, "Editor autosave root");
	core::io::SecurePath::CreateTrustedRoot(this->RecoveryRoot, "Editor recovery root");
}

EditorRecoveryStore::~EditorRecoveryStore()
{
	this->Wait();
}

void EditorRecoveryStore::Tick(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection,
							   resource::AssetManager &Assets, core::threading::TaskScheduler &Scheduler)
{
	const auto Now = std::chrono::steady_clock::now();
	const uint64 Revision = Document.GetRevision();
	if (Revision != this->ObservedRevision)
	{
		this->ObservedRevision = Revision;
		this->LastMutation = Now;
	}
	if (!Document.IsDirty() || this->Pending.valid() || Revision == this->PublishedRevision ||
		Now - this->LastMutation < this->Specification.QuietPeriod ||
		(this->LastPublication != std::chrono::steady_clock::time_point::min() &&
		 Now - this->LastPublication < this->Specification.AutosaveInterval))
	{
		return;
	}
	this->Begin(Document, Reflection, Assets, Scheduler);
}

bool EditorRecoveryStore::Poll()
{
	if (!this->Pending.valid() || this->Pending.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
		return false;
	this->CompletePending();
	return true;
}

void EditorRecoveryStore::Wait() noexcept
{
	if (!this->Pending.valid())
		return;
	this->Pending.wait();
	this->CompletePending();
}

void EditorRecoveryStore::CompletePending() noexcept
{
	try
	{
		this->LastResult = this->Pending.get();
		if (this->LastResult->Candidate.DocumentID == this->SuppressedDocument &&
			this->LastResult->Candidate.Revision <= this->SuppressedThroughRevision)
		{
			this->Discard(this->LastResult->Candidate);
			this->LastResult.reset();
		}
		else
		{
			this->PublishedRevision = this->LastResult->Candidate.Revision;
		}
	}
	catch (const std::exception &Exception)
	{
		this->LastResult = EditorRecoveryResult{.Diagnostic = Exception.what()};
	}
	catch (...)
	{
		this->LastResult = EditorRecoveryResult{.Diagnostic = "Unknown non-standard recovery failure"};
	}
	this->LastPublication = std::chrono::steady_clock::now();
}

void EditorRecoveryStore::Force(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection,
								resource::AssetManager &Assets, core::threading::TaskScheduler &Scheduler)
{
	if (this->Pending.valid())
		throw EditorRecoveryException("A recovery snapshot is already being written");
	this->ObservedRevision = Document.GetRevision();
	this->Begin(Document, Reflection, Assets, Scheduler);
}

void EditorRecoveryStore::SetSpecification(const EditorRecoverySpecification Specification)
{
	if (Specification.AutosaveInterval <= std::chrono::seconds::zero() || Specification.QuietPeriod < std::chrono::seconds::zero())
		throw std::invalid_argument("Editor recovery timing must be positive");
	this->Specification = Specification;
}

std::vector<EditorRecoveryCandidate> EditorRecoveryStore::Scan() const
{
	std::vector<EditorRecoveryCandidate> Result;
	std::error_code Error;
	for (std::filesystem::directory_iterator
			 Iterator(this->RecoveryRoot, std::filesystem::directory_options::skip_permission_denied, Error),
		 End;
		 !Error && Iterator != End; Iterator.increment(Error))
	{
		if (!Iterator->is_regular_file() || Iterator->path().extension() != ".json")
			continue;
		try
		{
			EditorRecoveryCandidate Candidate = ReadManifest(Iterator->path(), this->AutosaveRoot);
			if (std::filesystem::is_regular_file(Candidate.SnapshotPath) &&
				CalculateFileChecksum(Candidate.SnapshotPath) == Candidate.SnapshotChecksum)
			{
				ValidateSnapshotMetadata(Candidate);
				Result.push_back(std::move(Candidate));
			}
		}
		catch (const std::exception &)
		{
		}
	}
	if (Error)
		throw EditorRecoveryException("Could not enumerate recovery manifests: " + Error.message());
	std::ranges::sort(Result, std::greater{}, &EditorRecoveryCandidate::TimestampMilliseconds);
	return Result;
}

void EditorRecoveryStore::Discard(const EditorRecoveryCandidate &Candidate)
{
	if (!Candidate.DocumentID.IsValid())
		throw std::invalid_argument("Cannot discard an invalid recovery candidate");
	const std::filesystem::path ExpectedSnapshot = this->AutosaveRoot / (Candidate.DocumentID.ToString() + ".autosave.scene");
	if (std::filesystem::exists(ExpectedSnapshot))
		core::io::SecurePath::RemoveWithin(this->AutosaveRoot, ExpectedSnapshot.filename(), false, "Recovery snapshot discard");
	const std::filesystem::path Manifest = this->ManifestPath(Candidate.DocumentID);
	if (std::filesystem::exists(Manifest))
		core::io::SecurePath::RemoveWithin(this->RecoveryRoot, Manifest.filename(), false, "Recovery manifest discard");
}

void EditorRecoveryStore::Discard(const util::UUID &DocumentID)
{
	if (!DocumentID.IsValid())
		throw std::invalid_argument("Cannot discard recovery for an invalid document identity");
	for (const EditorRecoveryCandidate &Candidate : this->Scan())
	{
		if (Candidate.DocumentID == DocumentID)
			this->Discard(Candidate);
	}
}

void EditorRecoveryStore::AcknowledgeSaved(const util::UUID &DocumentID, const uint64 Revision)
{
	if (!DocumentID.IsValid() || Revision == 0)
		throw std::invalid_argument("Saved recovery acknowledgement requires a valid document and revision");
	this->SuppressedDocument = DocumentID;
	this->SuppressedThroughRevision = Revision;
	this->Discard(DocumentID);
}

void EditorRecoveryStore::ResetTracking() noexcept
{
	this->ObservedRevision = 0;
	this->PublishedRevision = 0;
	this->LastMutation = std::chrono::steady_clock::now();
	this->LastPublication = std::chrono::steady_clock::time_point::min();
	this->LastResult.reset();
	this->SuppressedDocument = {};
	this->SuppressedThroughRevision = 0;
}

bool EditorRecoveryStore::IsBusy() const noexcept
{
	return this->Pending.valid();
}

const std::optional<EditorRecoveryResult> &EditorRecoveryStore::GetLastResult() const noexcept
{
	return this->LastResult;
}

void EditorRecoveryStore::Begin(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection,
								resource::AssetManager &Assets, core::threading::TaskScheduler &Scheduler)
{
	const util::UUID DocumentID = Document.GetID();
	const string DocumentName = Document.GetName();
	const std::filesystem::path ProjectRoot = this->AutosaveRoot.parent_path().parent_path();
	const std::filesystem::path DocumentPath = Document.GetPath();
	const std::filesystem::path OriginalPath =
		DocumentPath.empty() || !IsWithin(ProjectRoot, DocumentPath)
			? std::filesystem::path{}
			: std::filesystem::absolute(DocumentPath).lexically_normal().lexically_relative(ProjectRoot);
	const uint64 Revision = Document.GetRevision();
	const std::filesystem::path SnapshotPath = this->AutosaveRoot / (DocumentID.ToString() + ".autosave.scene");
	const std::filesystem::path ManifestPath = this->ManifestPath(DocumentID);
	const std::filesystem::path RecoveryRoot = this->RecoveryRoot;
	const world::Scene *Scene = &Document.GetScene();
	this->Pending = Scheduler.Submit(
		[DocumentID, DocumentName, OriginalPath, Revision, SnapshotPath, ManifestPath, RecoveryRoot, Scene, &Reflection, &Assets]()
		{
			const int64 TimestampMilliseconds =
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			serialization::SceneDocumentSerializer::SaveSnapshot(DocumentID, DocumentName, *Scene, Reflection, Assets, SnapshotPath,
																 Revision, TimestampMilliseconds);
			EditorRecoveryCandidate Candidate{.DocumentID = DocumentID,
											  .DocumentName = DocumentName,
											  .SnapshotPath = SnapshotPath,
											  .OriginalPath = OriginalPath,
											  .Revision = Revision,
											  .TimestampMilliseconds = TimestampMilliseconds,
											  .SnapshotChecksum = CalculateFileChecksum(SnapshotPath)};
			WriteManifest(Candidate, RecoveryRoot, ManifestPath);
			return EditorRecoveryResult{.Candidate = std::move(Candidate), .Diagnostic = "Recovery snapshot published"};
		},
		core::threading::TaskPriority::Background);
}

std::filesystem::path EditorRecoveryStore::ManifestPath(const util::UUID &DocumentID) const
{
	return this->RecoveryRoot / (DocumentID.ToString() + ".recovery.json");
}
} // namespace editor::recovery
