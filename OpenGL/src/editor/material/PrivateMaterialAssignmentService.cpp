#include "PrivateMaterialAssignmentService.h"

#include "src/component/object/CObjectIdentityComponent.h"
#include "src/component/object/CObjectMeshComponent.h"
#include "src/editor/asset/AssetMetadata.h"
#include "src/editor/asset/AssetContentService.h"
#include "src/editor/asset/AssetRegistry.h"
#include "src/editor/commands/EditorCommand.h"
#include "src/editor/document/SceneDocument.h"
#include "src/editor/material/MaterialDocument.h"
#include "src/core/io/SecurePath.h"
#include "src/resource/asset/AssetManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::material
{
namespace
{
enum class AssignmentPublicationState : uint8
{
	Pending,
	Committed,
	Failed,
	Superseded
};

struct AssignmentTargetState final
{
	world::ObjectHandle Object;
	resource::ModelMeshInstanceID MeshInstance = 0;
	resource::MaterialSlotID MaterialSlot = 0;
	std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Before;
	resource::AssetHandle<resource::MaterialInterfaceAsset> After;
};

[[nodiscard]] std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> FindOverride(
	const components::CObjectMeshComponent &Component, const resource::ModelMeshInstanceID MeshInstance,
	const resource::MaterialSlotID MaterialSlot)
{
	const auto Found = std::ranges::find_if(Component.GetMaterialOverrides(), [&](const components::MeshMaterialOverride &Override)
											{ return Override.MeshInstance == MeshInstance && Override.MaterialSlot == MaterialSlot; });
	return Found == Component.GetMaterialOverrides().end()
			   ? std::nullopt
			   : std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>>(Found->Material);
}

void SetOverride(components::CObjectMeshComponent &Component, const resource::ModelMeshInstanceID MeshInstance,
				 const resource::MaterialSlotID MaterialSlot, const resource::AssetHandle<resource::MaterialInterfaceAsset> &Material)
{
	if (Material)
		Component.SetMaterialOverride(MeshInstance, MaterialSlot, Material);
	else
		Component.ClearMaterialOverride(MeshInstance, MaterialSlot);
}

class AssignmentCommandState final
{
  public:
	explicit AssignmentCommandState(world::Scene &Scene) : Scene(&Scene)
	{
	}

	void ApplyAfter()
	{
		if (this->PublicationState == AssignmentPublicationState::Failed ||
			this->PublicationState == AssignmentPublicationState::Superseded)
		{
			this->RestoreBefore(false);
			return;
		}
		this->Apply(false);
	}

	void RestoreBefore(const bool OnlyIfStillAssigned)
	{
		this->Apply(true, OnlyIfStillAssigned);
	}

	void Commit() noexcept
	{
		this->PublicationState = AssignmentPublicationState::Committed;
	}

	void Fail()
	{
		this->PublicationState = AssignmentPublicationState::Failed;
		this->RestoreBefore(true);
	}

	void Supersede()
	{
		this->PublicationState = AssignmentPublicationState::Superseded;
		this->RestoreBefore(true);
	}

	[[nodiscard]] std::vector<resource::AssetID> ReleaseAfterHandles()
	{
		std::vector<resource::AssetID> IDs;
		IDs.reserve(this->Targets.size());
		for (AssignmentTargetState &Target : this->Targets)
		{
			if (Target.After)
				IDs.push_back(Target.After.GetID());
			Target.After = {};
		}
		return IDs;
	}

	world::Scene *Scene = nullptr;
	std::vector<AssignmentTargetState> Targets;
	AssignmentPublicationState PublicationState = AssignmentPublicationState::Pending;

  private:
	void Apply(const bool Restore, const bool OnlyIfStillAssigned = false)
	{
		if (this->Scene == nullptr)
			throw std::logic_error("Private material assignment lost its scene");
		std::vector<const AssignmentTargetState *> LiveTargets;
		LiveTargets.reserve(this->Targets.size());
		for (const AssignmentTargetState &Target : this->Targets)
		{
			if (this->Scene->Contains(Target.Object))
				LiveTargets.push_back(&Target);
		}
		auto Access = this->Scene->Write();
		for (const AssignmentTargetState *Target : LiveTargets)
		{
			const auto Mesh = Access.GetComponent<components::CObjectMeshComponent>(Target->Object);
			if (!Mesh.IsValid())
				continue;
			components::CObjectMeshComponent &Component = Access.Resolve(Mesh);
			if (OnlyIfStillAssigned)
			{
				const auto Current = FindOverride(Component, Target->MeshInstance, Target->MaterialSlot);
				if (!Current.has_value() || !*Current || Current->GetID() != Target->After.GetID())
					continue;
			}
			SetOverride(Component, Target->MeshInstance, Target->MaterialSlot, Restore ? Target->Before.value_or(nullptr) : Target->After);
		}
	}
};

class PrivateMaterialAssignmentCommand final : public commands::EditorCommand
{
  public:
	explicit PrivateMaterialAssignmentCommand(std::shared_ptr<AssignmentCommandState> State) : State(std::move(State))
	{
		if (this->State == nullptr || this->State->Targets.empty())
			throw std::invalid_argument("Private material assignment command requires targets");
	}

	[[nodiscard]] string_view GetName() const noexcept override
	{
		return "Set Private Material Color";
	}

	void Execute() override
	{
		this->State->ApplyAfter();
	}

	void Undo() override
	{
		this->State->RestoreBefore(false);
	}

  private:
	std::shared_ptr<AssignmentCommandState> State;
};

struct PersistencePayload final
{
	util::UUID OperationID;
	std::filesystem::path ContentRoot;
	std::filesystem::path IntermediateRoot;
	std::vector<MaterialDocument> Documents;
	std::vector<asset::AssetMetadata> Metadata;
};

struct PersistenceResult final
{
	util::UUID OperationID;
	bool Committed = false;
	string Diagnostic;
	std::filesystem::path TransactionRoot;
};

struct PendingAssignment final
{
	util::UUID OperationID;
	document::SceneDocument *Document = nullptr;
	std::shared_ptr<AssignmentCommandState> CommandState;
	std::vector<string> TargetKeys;
	std::vector<resource::GeneratedAssetStage> AssetStages;
	bool Superseded = false;
	std::future<PersistenceResult> Future;
};

struct BaseColorPreviewSource final
{
	PrivateMaterialTarget Target;
	std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Before;
	resource::AssetHandle<resource::MaterialInterfaceAsset> Parent;
	resource::MaterialPipelineContract Pipeline;
	resource::PBRMaterialFactors Factors;
	std::vector<resource::MaterialTextureBinding> Textures;
};

struct BaseColorPreview final
{
	util::UUID ID;
	document::SceneDocument *Document = nullptr;
	glm::vec4 Value{1.0f};
	std::vector<BaseColorPreviewSource> Sources;
	std::vector<resource::GeneratedAssetStage> AssetStages;
	std::vector<string> TargetKeys;
};

[[nodiscard]] resource::AssetHandle<resource::MaterialInterfaceAsset> ResolveEffectiveMaterial(
	const components::CObjectMeshComponent &Component, const resource::ModelMeshInstanceID MeshInstance,
	const resource::MaterialSlotID MaterialSlot)
{
	if (const auto Override = FindOverride(Component, MeshInstance, MaterialSlot); Override.has_value())
		return *Override;
	const auto Model = Component.GetModel().Pin();
	if (Model == nullptr)
		throw std::runtime_error("Private material assignment references an unavailable model asset");
	const auto Instance = std::ranges::find(Model->GetMeshInstances(), MeshInstance, &resource::ModelMeshInstance::ID);
	if (Instance == Model->GetMeshInstances().end())
		throw std::out_of_range("Private material assignment references an unknown mesh instance");
	const auto Mesh = Instance->Mesh.Pin();
	if (Mesh == nullptr)
		throw std::runtime_error("Private material assignment references an unavailable mesh asset");
	const auto Slot = std::ranges::find(Mesh->GetMaterialSlots(), MaterialSlot, &resource::MeshMaterialSlot::ID);
	if (Slot == Mesh->GetMaterialSlots().end() || !Slot->DefaultMaterial)
		throw std::out_of_range("Private material assignment references an unavailable material slot");
	return Slot->DefaultMaterial;
}

[[nodiscard]] string MakePrivateMaterialName(const util::UUID &ObjectID, const resource::ModelMeshInstanceID MeshInstance,
											 const resource::MaterialSlotID MaterialSlot)
{
	return ObjectID.ToString() + "_" + std::to_string(MeshInstance) + "_" + std::to_string(MaterialSlot);
}

[[nodiscard]] resource::AssetID ResolvePrivateAssetID(const std::filesystem::path &Path)
{
	string Diagnostic;
	const auto Metadata = asset::AssetMetadataStore::TryLoad(asset::AssetMetadataStore::GetSidecarPath(Path), Diagnostic);
	return Metadata.has_value() ? Metadata->ID : util::UUID::GenerateRandomUUID().ToString();
}

[[nodiscard]] std::filesystem::path GetJournalPath(const std::filesystem::path &TransactionRoot)
{
	return TransactionRoot / "journal.json";
}

[[nodiscard]] nlohmann::json ReadJournal(const std::filesystem::path &TransactionRoot)
{
	constexpr uint64 MaximumJournalBytes = 4U * 1024U * 1024U;
	const std::vector<uint8> Bytes =
		core::io::SecurePath::ReadFileWithin(TransactionRoot, "journal.json", MaximumJournalBytes, "Private-material transaction journal");
	return nlohmann::json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
}

void WriteJournal(const std::filesystem::path &TransactionRoot, const nlohmann::json &Journal)
{
	const string Serialized = Journal.dump(2) + '\n';
	core::io::SecurePath::WriteFileWithin(TransactionRoot, "journal.json",
										  std::span(reinterpret_cast<const uint8 *>(Serialized.data()), Serialized.size()), true, true,
										  "Private-material transaction journal");
}

void RollbackPersistenceTransaction(const std::filesystem::path &TransactionRoot, const std::filesystem::path &IntermediateRoot,
									const std::filesystem::path &ContentRoot)
{
	const nlohmann::json Journal = ReadJournal(TransactionRoot);
	if (Journal.value("State", string()) == "Committed")
	{
		core::io::SecurePath::RemoveWithin(IntermediateRoot, TransactionRoot.lexically_relative(IntermediateRoot), true,
										   "Committed private-material transaction");
		return;
	}
	const auto &Entries = Journal.at("Entries");
	for (auto Iterator = Entries.rbegin(); Iterator != Entries.rend(); ++Iterator)
	{
		const std::filesystem::path Destination = Iterator->at("Destination").get<string>();
		const std::filesystem::path Backup = Iterator->at("Backup").get<string>();
		const bool Existed = Iterator->at("Existed").get<bool>();
		if (Existed)
			core::io::SecurePath::CopyWithin(IntermediateRoot, Backup.lexically_relative(IntermediateRoot), ContentRoot,
											 Destination.lexically_relative(ContentRoot), false, true, "Private-material rollback");
		else if (std::filesystem::exists(Destination))
			core::io::SecurePath::RemoveWithin(ContentRoot, Destination.lexically_relative(ContentRoot), false,
											   "Private-material rollback");
	}
	core::io::SecurePath::RemoveWithin(IntermediateRoot, TransactionRoot.lexically_relative(IntermediateRoot), true,
									   "Completed private-material transaction");
}

void FinalizePersistenceTransaction(const std::filesystem::path &TransactionRoot, const std::filesystem::path &IntermediateRoot)
{
	nlohmann::json Journal = ReadJournal(TransactionRoot);
	Journal["State"] = "Committed";
	WriteJournal(TransactionRoot, Journal);
	core::io::SecurePath::RemoveWithin(IntermediateRoot, TransactionRoot.lexically_relative(IntermediateRoot), true,
									   "Private-material transaction finalization");
}

[[nodiscard]] PersistenceResult Persist(PersistencePayload Payload)
{
	const std::filesystem::path BackupRoot = Payload.IntermediateRoot / "PrivateMaterialTransactions" / Payload.OperationID.ToString();
	struct BackupEntry final
	{
		std::filesystem::path Destination;
		std::filesystem::path Backup;
		bool Existed = false;
	};
	std::vector<BackupEntry> Backups;
	try
	{
		core::io::SecurePath::CreateDirectoriesWithin(Payload.IntermediateRoot, BackupRoot.lexically_relative(Payload.IntermediateRoot),
													  "Private-material transaction staging");
		nlohmann::json Journal{{"Version", 1U}, {"State", "Prepared"}, {"Entries", nlohmann::json::array()}};
		for (usize Index = 0; Index < Payload.Documents.size(); ++Index)
		{
			const std::filesystem::path Sidecar = asset::AssetMetadataStore::GetSidecarPath(Payload.Documents[Index].Path);
			for (const std::filesystem::path &Destination : {Payload.Documents[Index].Path, Sidecar})
			{
				BackupEntry Entry{.Destination = Destination,
								  .Backup = BackupRoot / (std::to_string(Backups.size()) + ".backup"),
								  .Existed = std::filesystem::is_regular_file(Destination)};
				if (Entry.Existed)
					core::io::SecurePath::CopyWithin(Payload.ContentRoot, Entry.Destination.lexically_relative(Payload.ContentRoot),
													 Payload.IntermediateRoot, Entry.Backup.lexically_relative(Payload.IntermediateRoot),
													 false, true, "Private-material backup");
				Journal["Entries"].push_back(
					{{"Destination", Entry.Destination.string()}, {"Backup", Entry.Backup.string()}, {"Existed", Entry.Existed}});
				Backups.push_back(std::move(Entry));
			}
			WriteJournal(BackupRoot, Journal);
			MaterialDocumentStore::Save(Payload.Documents[Index]);
			Payload.Metadata[Index].PhysicalSourceIdentity =
				asset::AssetMetadataStore::CalculatePhysicalSourceIdentity(Payload.Documents[Index].Path);
			Payload.Metadata[Index].SourceHash = asset::AssetMetadataStore::CalculateSourceHash(Payload.Documents[Index].Path);
			asset::AssetMetadataStore::Save(Payload.Metadata[Index], Sidecar);
		}
		return {.OperationID = Payload.OperationID,
				.Committed = true,
				.Diagnostic = "Persisted " + std::to_string(Payload.Documents.size()) + " private material instance(s)",
				.TransactionRoot = BackupRoot};
	}
	catch (const std::exception &Exception)
	{
		string Diagnostic = Exception.what();
		try
		{
			if (std::filesystem::is_regular_file(GetJournalPath(BackupRoot)))
				RollbackPersistenceTransaction(BackupRoot, Payload.IntermediateRoot, Payload.ContentRoot);
		}
		catch (const std::exception &RollbackException)
		{
			Diagnostic += "; rollback requires startup recovery: " + string(RollbackException.what());
		}
		return {.OperationID = Payload.OperationID, .Committed = false, .Diagnostic = std::move(Diagnostic), .TransactionRoot = BackupRoot};
	}
}
} // namespace

class PrivateMaterialAssignmentService::Implementation final
{
  public:
	Implementation(resource::AssetManager &Assets, std::filesystem::path ContentRoot, std::filesystem::path IntermediateRoot,
				   std::filesystem::path TrashRoot)
		: Assets(&Assets), ContentRoot(std::filesystem::absolute(std::move(ContentRoot)).lexically_normal()),
		  IntermediateRoot(std::filesystem::absolute(std::move(IntermediateRoot)).lexically_normal()),
		  TrashRoot(std::filesystem::absolute(std::move(TrashRoot)).lexically_normal())
	{
		const std::filesystem::path TransactionsRoot = this->IntermediateRoot / "PrivateMaterialTransactions";
		std::error_code EnumerationError;
		for (std::filesystem::directory_iterator Iterator(TransactionsRoot, EnumerationError), End; !EnumerationError && Iterator != End;
			 ++Iterator)
		{
			if (!Iterator->is_directory())
				continue;
			try
			{
				RollbackPersistenceTransaction(Iterator->path(), this->IntermediateRoot, this->ContentRoot);
			}
			catch (const std::exception &Exception)
			{
				this->Results.push_back({.OperationID = util::UUID::GenerateRandomUUID(),
										 .Committed = false,
										 .Diagnostic = "Private-material startup recovery failed: " + string(Exception.what())});
			}
		}
		if (EnumerationError && EnumerationError != std::errc::no_such_file_or_directory)
		{
			this->Results.push_back(
				{.OperationID = util::UUID::GenerateRandomUUID(),
				 .Committed = false,
				 .Diagnostic = "Could not enumerate private-material recovery transactions: " + EnumerationError.message()});
		}
	}

	void RequireOwnerThread() const
	{
		if (std::this_thread::get_id() != this->OwnerThread)
			throw std::logic_error("Private material assignments must be controlled by their owner thread");
	}

	void RequireActive() const
	{
		this->RequireOwnerThread();
		if (this->IsShutdown)
			throw std::logic_error("Private material assignment service is shut down");
	}

	[[nodiscard]] BaseColorPreview &FindPreview(const util::UUID &PreviewID)
	{
		const auto Preview = std::ranges::find(this->Previews, PreviewID, &BaseColorPreview::ID);
		if (Preview == this->Previews.end())
			throw std::out_of_range("Private material color preview is no longer active");
		return *Preview;
	}

	void ApplyPreview(BaseColorPreview &Preview, const glm::vec4 &BaseColor)
	{
		std::vector<resource::GeneratedAssetStage> ReplacementStages;
		std::vector<resource::AssetHandle<resource::MaterialInterfaceAsset>> ReplacementHandles;
		ReplacementStages.reserve(Preview.Sources.size());
		ReplacementHandles.reserve(Preview.Sources.size());
		for (const BaseColorPreviewSource &Source : Preview.Sources)
		{
			resource::PBRMaterialFactors Factors = Source.Factors;
			Factors.BaseColor = BaseColor;
			std::vector<resource::AssetID> Dependencies{Source.Parent.GetID()};
			Dependencies.reserve(Source.Textures.size() + 1U);
			for (const resource::MaterialTextureBinding &Texture : Source.Textures)
				Dependencies.push_back(Texture.Texture.GetID());
			std::ranges::sort(Dependencies);
			Dependencies.erase(std::unique(Dependencies.begin(), Dependencies.end()), Dependencies.end());
			const string PreviewToken = util::UUID::GenerateRandomUUID().ToString();
			const resource::AssetID PreviewAssetID = "EditorMaterialPreview:" + PreviewToken;
			resource::GeneratedAssetStage Stage = this->Assets->StageGeneratedAsset<resource::MaterialInstanceAsset>(
				PreviewAssetID, this->ContentRoot / "Generated" / "MaterialPreviews" / PreviewToken,
				resource::AssetPtr<resource::MaterialInstanceAsset>::Make("Editor Material Preview", Source.Parent, Source.Pipeline,
																		  Factors, Source.Textures),
				std::move(Dependencies));
			ReplacementHandles.emplace_back(Stage.GetHandle<resource::MaterialInstanceAsset>());
			ReplacementStages.push_back(std::move(Stage));
		}

		world::Scene &Scene = Preview.Document->GetScene();
		for (const BaseColorPreviewSource &Source : Preview.Sources)
		{
			if (!Scene.Contains(Source.Target.Object))
				throw std::out_of_range("Private material color preview target no longer exists");
		}
		auto Access = Scene.WriteTransient();
		for (usize Index = 0; Index < Preview.Sources.size(); ++Index)
		{
			const BaseColorPreviewSource &Source = Preview.Sources[Index];
			auto &Mesh = Access.Resolve(Access.GetComponent<components::CObjectMeshComponent>(Source.Target.Object));
			SetOverride(Mesh, Source.Target.MeshInstance, Source.Target.MaterialSlot, ReplacementHandles[Index]);
		}
		Preview.Value = BaseColor;
		Preview.AssetStages = std::move(ReplacementStages);
	}

	void RestorePreview(BaseColorPreview &Preview)
	{
		world::Scene &Scene = Preview.Document->GetScene();
		std::vector<const BaseColorPreviewSource *> LiveSources;
		LiveSources.reserve(Preview.Sources.size());
		for (const BaseColorPreviewSource &Source : Preview.Sources)
		{
			if (Scene.Contains(Source.Target.Object))
				LiveSources.push_back(&Source);
		}
		auto Access = Scene.WriteTransient();
		for (const BaseColorPreviewSource *Source : LiveSources)
		{
			const auto MeshHandle = Access.GetComponent<components::CObjectMeshComponent>(Source->Target.Object);
			if (!MeshHandle.IsValid())
				continue;
			SetOverride(Access.Resolve(MeshHandle), Source->Target.MeshInstance, Source->Target.MaterialSlot,
						Source->Before.value_or(nullptr));
		}
		Preview.AssetStages.clear();
	}

	void ReleasePreviewTargets(const BaseColorPreview &Preview) noexcept
	{
		for (const string &TargetKey : Preview.TargetKeys)
			this->ActiveTargetKeys.erase(TargetKey);
	}

	struct ActiveRetirement final
	{
		resource::GeneratedAssetRetirement Retirement;
	};

	[[nodiscard]] bool StartNextRetirement(core::threading::TaskScheduler &Scheduler)
	{
		std::vector<resource::AssetID> Candidates;
		{
			std::scoped_lock Lock(this->RetirementMutex);
			Candidates.swap(this->RetirementCandidates);
		}
		for (usize Index = 0; Index < Candidates.size(); ++Index)
		{
			const resource::AssetID &ID = Candidates[Index];
			const std::optional<resource::AssetRecordSnapshot> Snapshot = this->Assets->SnapshotRecord(ID);
			if (!Snapshot.has_value() || Snapshot->Type != resource::AssetType::MaterialInstance)
				continue;
			const std::filesystem::path Relative = Snapshot->CanonicalPath.lexically_relative(this->ContentRoot);
			if (Relative.empty() || *Relative.begin() == ".." || Relative.parent_path() != std::filesystem::path("Generated/Materials"))
			{
				continue;
			}

			resource::GeneratedAssetRetirementAttempt Attempt = this->Assets->TryRetireGeneratedAsset(ID);
			if (Attempt.Status == resource::GeneratedAssetRetirementStatus::Referenced ||
				Attempt.Status == resource::GeneratedAssetRetirementStatus::Busy)
			{
				std::scoped_lock Lock(this->RetirementMutex);
				this->RetirementCandidates.push_back(ID);
				continue;
			}
			if (Attempt.Status != resource::GeneratedAssetRetirementStatus::Retired || !Attempt.Retirement.has_value())
				continue;

			this->Active = ActiveRetirement{.Retirement = std::move(*Attempt.Retirement)};
			try
			{
				if (this->RetirementContent == nullptr)
				{
					this->RetirementContent =
						std::make_unique<asset::AssetContentService>(this->ContentRoot, this->IntermediateRoot, this->TrashRoot);
				}
				this->RetirementContent->Queue({.Operation = asset::AssetContentOperation::Trash, .Source = Relative}, Scheduler);
			}
			catch (const std::exception &Exception)
			{
				this->Assets->RestoreRetiredGeneratedAsset(std::move(this->Active->Retirement));
				this->Active.reset();
				this->Results.push_back({.OperationID = util::UUID::GenerateRandomUUID(),
										 .Committed = false,
										 .Diagnostic = "Could not queue private-material retirement: " + string(Exception.what())});
				continue;
			}

			if (Index + 1 < Candidates.size())
			{
				std::scoped_lock Lock(this->RetirementMutex);
				this->RetirementCandidates.insert(this->RetirementCandidates.end(), Candidates.begin() + static_cast<isize>(Index + 1),
												  Candidates.end());
			}
			return true;
		}
		return false;
	}

	void DiscardFailedPublications(AssignmentCommandState &CommandState) noexcept
	{
		try
		{
			for (const resource::AssetID &ID : CommandState.ReleaseAfterHandles())
				(void)this->Assets->TryRetireGeneratedAsset(ID);
		}
		catch (...)
		{
		}
	}

	void CompleteRetirement()
	{
		if (this->RetirementContent == nullptr)
			return;
		const std::optional<asset::AssetContentResult> &Result = this->RetirementContent->GetResult();
		if (!this->Active.has_value() || !Result.has_value())
			return;
		if (!Result->Committed)
			this->Assets->RestoreRetiredGeneratedAsset(std::move(this->Active->Retirement));
		this->Results.push_back({.OperationID = Result->OperationID,
								 .Committed = Result->Committed,
								 .Diagnostic = Result->Committed ? "Retired unused private material to project trash"
																 : "Private-material retirement rolled back: " + Result->Diagnostic});
		this->Active.reset();
		this->RetirementContent->Reset();
	}

	resource::AssetManager *Assets = nullptr;
	std::filesystem::path ContentRoot;
	std::filesystem::path IntermediateRoot;
	std::filesystem::path TrashRoot;
	std::vector<PendingAssignment> Pending;
	std::vector<BaseColorPreview> Previews;
	std::vector<PrivateMaterialAssignmentResult> Results;
	std::unique_ptr<asset::AssetContentService> RetirementContent;
	std::mutex RetirementMutex;
	std::vector<resource::AssetID> RetirementCandidates;
	std::optional<ActiveRetirement> Active;
	core::threading::TaskScheduler *LastScheduler = nullptr;
	std::unordered_set<string> ActiveTargetKeys;
	std::thread::id OwnerThread = std::this_thread::get_id();
	bool IsShutdown = false;
};

PrivateMaterialAssignmentService::PrivateMaterialAssignmentService(resource::AssetManager &Assets, std::filesystem::path ContentRoot,
																   std::filesystem::path IntermediateRoot, std::filesystem::path TrashRoot)
	: State(std::make_unique<Implementation>(Assets, std::move(ContentRoot), std::move(IntermediateRoot), std::move(TrashRoot)))
{
}

PrivateMaterialAssignmentService::~PrivateMaterialAssignmentService()
{
	this->Shutdown();
}

util::UUID PrivateMaterialAssignmentService::BeginBaseColorPreview(document::SceneDocument &Document,
																   const std::span<const PrivateMaterialTarget> Targets,
																   const glm::vec4 &BaseColor)
{
	this->State->RequireActive();
	if (Targets.empty())
		throw std::invalid_argument("Private material color preview requires at least one target");
	if (!std::isfinite(BaseColor.x) || !std::isfinite(BaseColor.y) || !std::isfinite(BaseColor.z) || !std::isfinite(BaseColor.w))
		throw std::invalid_argument("Private material preview base color must be finite");

	BaseColorPreview Preview{.ID = util::UUID::GenerateRandomUUID(), .Document = &Document, .Value = BaseColor};
	Preview.Sources.reserve(Targets.size());
	Preview.TargetKeys.reserve(Targets.size());
	std::unordered_set<string> UniqueTargets;
	UniqueTargets.reserve(Targets.size());
	for (const PrivateMaterialTarget &Target : Targets)
	{
		if (!Target.Object.IsValid() || Target.MeshInstance == 0 || Target.MaterialSlot == 0 ||
			!Document.GetScene().Contains(Target.Object))
			throw std::invalid_argument("Private material color preview target is invalid");

		resource::AssetHandle<resource::MaterialInterfaceAsset> Effective;
		std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Before;
		util::UUID ObjectID;
		{
			auto Access = Document.GetScene().Read();
			const auto MeshHandle = Access.GetComponent<components::CObjectMeshComponent>(Target.Object);
			const auto IdentityHandle = Access.GetComponent<components::CObjectIdentityComponent>(Target.Object);
			if (!MeshHandle.IsValid() || !IdentityHandle.IsValid())
				throw std::invalid_argument("Private material color preview requires mesh and identity components");
			const components::CObjectMeshComponent &Mesh = Access.Resolve(MeshHandle);
			Before = FindOverride(Mesh, Target.MeshInstance, Target.MaterialSlot);
			Effective = ResolveEffectiveMaterial(Mesh, Target.MeshInstance, Target.MaterialSlot);
			ObjectID = Access.Resolve(IdentityHandle).GetPersistentID();
		}

		const string TargetKey = MakePrivateMaterialName(ObjectID, Target.MeshInstance, Target.MaterialSlot);
		if (!UniqueTargets.insert(TargetKey).second)
			continue;
		if (this->State->ActiveTargetKeys.contains(TargetKey))
			throw std::logic_error("A private material edit for this object slot is already active");

		const auto PinnedEffective = Effective.Pin();
		if (PinnedEffective == nullptr)
			throw std::runtime_error("Private material color preview references an unavailable effective material");
		resource::AssetHandle<resource::MaterialInterfaceAsset> Parent = Effective;
		if (const auto *Instance = dynamic_cast<const resource::MaterialInstanceAsset *>(PinnedEffective.Get()); Instance != nullptr)
		{
			if (!Instance->GetParent())
				throw std::logic_error("Material instance has no valid parent");
			Parent = Instance->GetParent();
		}
		Preview.TargetKeys.push_back(TargetKey);
		Preview.Sources.push_back({.Target = Target,
								   .Before = std::move(Before),
								   .Parent = std::move(Parent),
								   .Pipeline = PinnedEffective->GetPipelineContract(),
								   .Factors = PinnedEffective->GetFactors(),
								   .Textures = std::vector<resource::MaterialTextureBinding>(PinnedEffective->GetTextures().begin(),
																							 PinnedEffective->GetTextures().end())});
	}
	if (Preview.Sources.empty())
		throw std::invalid_argument("Private material color preview did not contain any unique targets");

	this->State->Previews.reserve(this->State->Previews.size() + 1U);
	try
	{
		for (const string &TargetKey : Preview.TargetKeys)
			this->State->ActiveTargetKeys.insert(TargetKey);
		this->State->ApplyPreview(Preview, BaseColor);
		const util::UUID PreviewID = Preview.ID;
		this->State->Previews.push_back(std::move(Preview));
		return PreviewID;
	}
	catch (...)
	{
		try
		{
			this->State->RestorePreview(Preview);
		}
		catch (...)
		{
		}
		this->State->ReleasePreviewTargets(Preview);
		throw;
	}
}

void PrivateMaterialAssignmentService::UpdateBaseColorPreview(const util::UUID &PreviewID, const glm::vec4 &BaseColor)
{
	this->State->RequireActive();
	if (!std::isfinite(BaseColor.x) || !std::isfinite(BaseColor.y) || !std::isfinite(BaseColor.z) || !std::isfinite(BaseColor.w))
		throw std::invalid_argument("Private material preview base color must be finite");
	this->State->ApplyPreview(this->State->FindPreview(PreviewID), BaseColor);
}

util::UUID PrivateMaterialAssignmentService::CommitBaseColorPreview(const util::UUID &PreviewID, core::threading::TaskScheduler &Scheduler)
{
	this->State->RequireActive();
	const auto Match = std::ranges::find(this->State->Previews, PreviewID, &BaseColorPreview::ID);
	if (Match == this->State->Previews.end())
		throw std::out_of_range("Private material color preview is no longer active");
	document::SceneDocument *const Document = Match->Document;
	const glm::vec4 BaseColor = Match->Value;
	std::vector<PrivateMaterialTarget> Targets;
	Targets.reserve(Match->Sources.size());
	for (const BaseColorPreviewSource &Source : Match->Sources)
		Targets.push_back(Source.Target);
	this->State->RestorePreview(*Match);
	this->State->ReleasePreviewTargets(*Match);
	this->State->Previews.erase(Match);
	return this->BeginBaseColorAssignment(*Document, Targets, BaseColor, Scheduler);
}

void PrivateMaterialAssignmentService::CancelBaseColorPreview(const util::UUID &PreviewID)
{
	this->State->RequireActive();
	const auto Match = std::ranges::find(this->State->Previews, PreviewID, &BaseColorPreview::ID);
	if (Match == this->State->Previews.end())
		return;
	this->State->RestorePreview(*Match);
	this->State->ReleasePreviewTargets(*Match);
	this->State->Previews.erase(Match);
}

util::UUID PrivateMaterialAssignmentService::BeginBaseColorAssignment(document::SceneDocument &Document,
																	  const std::span<const PrivateMaterialTarget> Targets,
																	  const glm::vec4 &BaseColor, core::threading::TaskScheduler &Scheduler)
{
	this->State->RequireActive();
	this->State->LastScheduler = &Scheduler;
	if (Targets.empty())
		throw std::invalid_argument("Private material assignment requires at least one target");
	if (!std::isfinite(BaseColor.x) || !std::isfinite(BaseColor.y) || !std::isfinite(BaseColor.z) || !std::isfinite(BaseColor.w))
		throw std::invalid_argument("Private material base color must be finite");

	const util::UUID OperationID = util::UUID::GenerateRandomUUID();
	auto CommandState = std::make_shared<AssignmentCommandState>(Document.GetScene());
	CommandState->Targets.reserve(Targets.size());
	PersistencePayload Payload{
		.OperationID = OperationID, .ContentRoot = this->State->ContentRoot, .IntermediateRoot = this->State->IntermediateRoot};
	std::vector<resource::GeneratedAssetStage> AssetStages;
	Payload.Documents.reserve(Targets.size());
	Payload.Metadata.reserve(Targets.size());
	std::unordered_set<string> UniqueTargets;
	UniqueTargets.reserve(Targets.size());

	for (const PrivateMaterialTarget &Target : Targets)
	{
		if (!Target.Object.IsValid() || Target.MeshInstance == 0 || Target.MaterialSlot == 0 ||
			!Document.GetScene().Contains(Target.Object))
		{
			throw std::invalid_argument("Private material assignment target is invalid");
		}

		resource::AssetHandle<resource::MaterialInterfaceAsset> Effective;
		std::optional<resource::AssetHandle<resource::MaterialInterfaceAsset>> Before;
		util::UUID ObjectID;
		{
			auto Access = Document.GetScene().Read();
			const auto MeshHandle = Access.GetComponent<components::CObjectMeshComponent>(Target.Object);
			const auto IdentityHandle = Access.GetComponent<components::CObjectIdentityComponent>(Target.Object);
			if (!MeshHandle.IsValid() || !IdentityHandle.IsValid())
				throw std::invalid_argument("Private material assignment target requires mesh and identity components");
			const components::CObjectMeshComponent &Mesh = Access.Resolve(MeshHandle);
			Before = FindOverride(Mesh, Target.MeshInstance, Target.MaterialSlot);
			Effective = ResolveEffectiveMaterial(Mesh, Target.MeshInstance, Target.MaterialSlot);
			ObjectID = Access.Resolve(IdentityHandle).GetPersistentID();
		}

		const string TargetKey = MakePrivateMaterialName(ObjectID, Target.MeshInstance, Target.MaterialSlot);
		if (!UniqueTargets.insert(TargetKey).second)
			continue;
		if (this->State->ActiveTargetKeys.contains(TargetKey))
			throw std::logic_error("A private material assignment for this object slot is already pending");
		const std::filesystem::path RelativePath = std::filesystem::path("Generated/Materials") / (TargetKey + ".materialinstance");
		const std::filesystem::path AbsolutePath = this->State->ContentRoot / RelativePath;
		const resource::AssetID PrivateID = ResolvePrivateAssetID(AbsolutePath);

		const auto PinnedEffective = Effective.Pin();
		if (PinnedEffective == nullptr)
			throw std::runtime_error("Private material assignment references an unavailable effective material");
		resource::AssetHandle<resource::MaterialInterfaceAsset> Parent = Effective;
		if (const auto *TypedInstance = dynamic_cast<const resource::MaterialInstanceAsset *>(PinnedEffective.Get());
			TypedInstance != nullptr)
		{
			if (!TypedInstance->GetParent())
				throw std::logic_error("Material instance has no valid parent");
			Parent = TypedInstance->GetParent();
		}

		const auto PinnedParent = Parent.Pin();
		if (PinnedParent == nullptr)
			throw std::runtime_error("Private material assignment references an unavailable parent material");
		resource::PBRMaterialFactors Factors = PinnedEffective->GetFactors();
		Factors.BaseColor = BaseColor;
		std::vector<resource::MaterialTextureBinding> Textures(PinnedEffective->GetTextures().begin(),
															   PinnedEffective->GetTextures().end());
		std::vector<resource::AssetID> Dependencies{Parent.GetID()};
		Dependencies.reserve(Textures.size() + 1U);
		for (const resource::MaterialTextureBinding &Texture : Textures)
			Dependencies.push_back(Texture.Texture.GetID());
		std::ranges::sort(Dependencies);
		Dependencies.erase(std::unique(Dependencies.begin(), Dependencies.end()), Dependencies.end());

		auto Private = this->State->Assets->StageGeneratedAsset<resource::MaterialInstanceAsset>(
			PrivateID, AbsolutePath,
			resource::AssetPtr<resource::MaterialInstanceAsset>::Make(TargetKey, Parent, PinnedEffective->GetPipelineContract(), Factors,
																	  Textures),
			Dependencies);
		CommandState->Targets.push_back(
			{.Object = Target.Object,
			 .MeshInstance = Target.MeshInstance,
			 .MaterialSlot = Target.MaterialSlot,
			 .Before = std::move(Before),
			 .After = resource::AssetHandle<resource::MaterialInterfaceAsset>(Private.GetHandle<resource::MaterialInstanceAsset>())});
		AssetStages.push_back(std::move(Private));

		MaterialDocument Material{
			.DocumentID = PrivateID,
			.Type = MaterialDocumentType::MaterialInstance,
			.Name = TargetKey,
			.Path = AbsolutePath,
			.Pipeline = PinnedEffective->GetPipelineContract(),
			.Factors = Factors,
			.FactorOverrides = {.BaseColor = BaseColor},
			.Parent = MaterialParentReference{.ID = Parent.GetID(),
											  .Type = dynamic_cast<const resource::MaterialInstanceAsset *>(PinnedParent.Get()) != nullptr
														  ? resource::AssetType::MaterialInstance
														  : resource::AssetType::Material}};
		Material.Textures.reserve(Textures.size());
		for (const resource::MaterialTextureBinding &Texture : Textures)
		{
			Material.Textures.push_back({.Semantic = Texture.Semantic,
										 .ID = Texture.Texture.GetID(),
										 .TextureCoordinateChannel = Texture.TextureCoordinateChannel});
		}
		Payload.Documents.push_back(std::move(Material));
		asset::AssetMetadata Metadata{.ID = PrivateID,
									  .AssetType = "MaterialInstance",
									  .VirtualSource = "/Game/" + RelativePath.generic_string(),
									  .Dependencies = std::move(Dependencies)};
		Payload.Metadata.push_back(std::move(Metadata));
	}

	if (CommandState->Targets.empty())
		throw std::invalid_argument("Private material assignment did not contain any unique targets");
	Document.Execute(std::make_unique<PrivateMaterialAssignmentCommand>(CommandState));
	for (const string &TargetKey : UniqueTargets)
		this->State->ActiveTargetKeys.insert(TargetKey);
	try
	{
		this->State->Pending.push_back(
			{.OperationID = OperationID,
			 .Document = &Document,
			 .CommandState = CommandState,
			 .TargetKeys = std::vector<string>(UniqueTargets.begin(), UniqueTargets.end()),
			 .AssetStages = std::move(AssetStages),
			 .Future = Scheduler.Submit([Payload = std::move(Payload)]() mutable { return Persist(std::move(Payload)); },
										core::threading::TaskPriority::Normal)});
	}
	catch (...)
	{
		for (const string &TargetKey : UniqueTargets)
			this->State->ActiveTargetKeys.erase(TargetKey);
		CommandState->Fail();
		this->State->DiscardFailedPublications(*CommandState);
		Document.MarkModified();
		throw;
	}
	return OperationID;
}

std::vector<util::UUID> PrivateMaterialAssignmentService::ClonePrivateAssignments(document::SceneDocument &Document,
																				  const std::span<const world::ObjectHandle> Objects,
																				  core::threading::TaskScheduler &Scheduler)
{
	this->State->RequireActive();
	struct Candidate final
	{
		PrivateMaterialTarget Target;
		glm::vec4 BaseColor{1.0f};
	};
	std::vector<Candidate> Candidates;
	for (const world::ObjectHandle Object : Objects)
	{
		if (!Object.IsValid() || !Document.GetScene().Contains(Object))
			continue;
		auto Access = Document.GetScene().Read();
		const auto MeshHandle = Access.GetComponent<components::CObjectMeshComponent>(Object);
		if (!MeshHandle.IsValid())
			continue;
		const components::CObjectMeshComponent &Mesh = Access.Resolve(MeshHandle);
		for (const components::MeshMaterialOverride &Override : Mesh.GetMaterialOverrides())
		{
			const std::optional<resource::AssetRecordSnapshot> Record = this->State->Assets->SnapshotRecord(Override.Material.GetID());
			if (!Record.has_value())
				continue;
			const std::filesystem::path Relative = Record->CanonicalPath.lexically_relative(this->State->ContentRoot);
			if (Relative.empty() || *Relative.begin() == ".." || Relative.parent_path() != std::filesystem::path("Generated/Materials"))
				continue;
			const resource::AssetPtr<resource::MaterialInterfaceAsset> Material = Override.Material.Pin();
			if (Material == nullptr)
				throw std::runtime_error("Private material clone encountered an unavailable material override");
			Candidates.push_back(
				{.Target = {.Object = Object, .MeshInstance = Override.MeshInstance, .MaterialSlot = Override.MaterialSlot},
				 .BaseColor = Material->GetFactors().BaseColor});
		}
	}

	std::vector<util::UUID> Operations;
	Operations.reserve(Candidates.size());
	for (const Candidate &Candidate : Candidates)
		Operations.push_back(this->BeginBaseColorAssignment(Document, std::span(&Candidate.Target, 1), Candidate.BaseColor, Scheduler));
	return Operations;
}

void PrivateMaterialAssignmentService::QueueRetirementCandidates(std::vector<resource::AssetID> Assets,
																 core::threading::TaskScheduler &Scheduler) noexcept
{
	try
	{
		std::scoped_lock Lock(this->State->RetirementMutex);
		if (this->State->IsShutdown)
			return;
		this->State->LastScheduler = &Scheduler;
		for (resource::AssetID &ID : Assets)
		{
			if (ID.empty() || (this->State->Active.has_value() && this->State->Active->Retirement.Publication.ID == ID) ||
				std::ranges::find(this->State->RetirementCandidates, ID) != this->State->RetirementCandidates.end())
			{
				continue;
			}
			this->State->RetirementCandidates.push_back(std::move(ID));
		}
	}
	catch (...)
	{
	}
}

bool PrivateMaterialAssignmentService::Poll(core::threading::TaskScheduler &Scheduler, asset::AssetRegistry &Registry)
{
	this->State->RequireActive();
	this->State->LastScheduler = &Scheduler;
	bool CompletedAny = false;
	if (this->State->Active.has_value() && this->State->RetirementContent != nullptr &&
		this->State->RetirementContent->Poll(Scheduler, Registry))
	{
		this->State->CompleteRetirement();
		CompletedAny = true;
	}
	for (usize Index = 0; Index < this->State->Pending.size();)
	{
		PendingAssignment &Pending = this->State->Pending[Index];
		if (Pending.Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++Index;
			continue;
		}
		CompletedAny = true;
		PersistenceResult Result;
		try
		{
			Result = Pending.Future.get();
		}
		catch (const std::exception &Exception)
		{
			Result = {.OperationID = Pending.OperationID, .Committed = false, .Diagnostic = Exception.what()};
		}
		if (Pending.Superseded)
		{
			Pending.CommandState->Supersede();
			std::vector<resource::AssetID> Retirements = Pending.CommandState->ReleaseAfterHandles();
			this->QueueRetirementCandidates(std::move(Retirements), Scheduler);
			Result.Committed = false;
			Result.Diagnostic = "Private material assignment was superseded";
			if (!Result.TransactionRoot.empty() && std::filesystem::exists(Result.TransactionRoot))
			{
				try
				{
					RollbackPersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot, this->State->ContentRoot);
				}
				catch (const std::exception &Exception)
				{
					Result.Diagnostic += "; persistence rollback requires recovery: " + string(Exception.what());
				}
			}
		}
		else if (Result.Committed)
		{
			try
			{
				std::vector<resource::GeneratedAssetStage *> Stages;
				Stages.reserve(Pending.AssetStages.size());
				for (resource::GeneratedAssetStage &Stage : Pending.AssetStages)
					Stages.push_back(&Stage);
				this->State->Assets->CommitGeneratedAssets(Stages);
				Pending.CommandState->Commit();
				FinalizePersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot);
			}
			catch (const std::exception &Exception)
			{
				Pending.CommandState->Fail();
				this->State->DiscardFailedPublications(*Pending.CommandState);
				if (Pending.Document != nullptr)
					Pending.Document->MarkModified();
				Result.Committed = false;
				Result.Diagnostic = "Private material publication failed after persistence: " + string(Exception.what());
				try
				{
					if (!Result.TransactionRoot.empty() && std::filesystem::exists(Result.TransactionRoot))
						RollbackPersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot, this->State->ContentRoot);
				}
				catch (const std::exception &RollbackException)
				{
					Result.Diagnostic += "; persistence rollback requires recovery: " + string(RollbackException.what());
				}
			}
		}
		else
		{
			Pending.CommandState->Fail();
			this->State->DiscardFailedPublications(*Pending.CommandState);
			if (Pending.Document != nullptr)
				Pending.Document->MarkModified();
		}
		this->State->Results.push_back(
			{.OperationID = Result.OperationID, .Committed = Result.Committed, .Diagnostic = std::move(Result.Diagnostic)});
		for (const string &TargetKey : Pending.TargetKeys)
			this->State->ActiveTargetKeys.erase(TargetKey);
		this->State->Pending.erase(this->State->Pending.begin() + static_cast<isize>(Index));
	}
	if (CompletedAny)
		Registry.RequestRefresh(Scheduler, true);
	if (!this->State->Active.has_value())
		(void)this->State->StartNextRetirement(Scheduler);
	return CompletedAny;
}

bool PrivateMaterialAssignmentService::Cancel(const util::UUID &OperationID, core::threading::TaskScheduler &Scheduler)
{
	this->State->RequireActive();
	const auto Pending = std::ranges::find(this->State->Pending, OperationID, &PendingAssignment::OperationID);
	if (Pending == this->State->Pending.end())
		return false;
	Pending->Superseded = true;
	Pending->CommandState->Supersede();
	this->State->LastScheduler = &Scheduler;
	return true;
}

void PrivateMaterialAssignmentService::Shutdown() noexcept
{
	if (this->State == nullptr || this->State->IsShutdown)
		return;
	if (std::this_thread::get_id() != this->State->OwnerThread)
		std::terminate();
	for (BaseColorPreview &Preview : this->State->Previews)
	{
		try
		{
			this->State->RestorePreview(Preview);
		}
		catch (...)
		{
		}
		this->State->ReleasePreviewTargets(Preview);
	}
	this->State->Previews.clear();
	this->Wait();
	this->State->RetirementContent.reset();
	this->State->LastScheduler = nullptr;
	this->State->Assets = nullptr;
	this->State->IsShutdown = true;
}

void PrivateMaterialAssignmentService::Wait() noexcept
{
	if (std::this_thread::get_id() != this->State->OwnerThread)
		std::terminate();
	for (PendingAssignment &Pending : this->State->Pending)
	{
		PersistenceResult Result{.OperationID = Pending.OperationID};
		try
		{
			Result = Pending.Future.get();
			if (Pending.Superseded)
			{
				Pending.CommandState->Supersede();
				std::vector<resource::AssetID> Retirements = Pending.CommandState->ReleaseAfterHandles();
				if (this->State->LastScheduler != nullptr)
				{
					this->QueueRetirementCandidates(std::move(Retirements), *this->State->LastScheduler);
				}
				Result.Committed = false;
				Result.Diagnostic = "Private material assignment was superseded";
				if (!Result.TransactionRoot.empty() && std::filesystem::exists(Result.TransactionRoot))
					RollbackPersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot, this->State->ContentRoot);
			}
			else if (Result.Committed)
			{
				std::vector<resource::GeneratedAssetStage *> Stages;
				for (resource::GeneratedAssetStage &Stage : Pending.AssetStages)
					Stages.push_back(&Stage);
				this->State->Assets->CommitGeneratedAssets(Stages);
				Pending.CommandState->Commit();
				FinalizePersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot);
			}
			else
			{
				Pending.CommandState->Fail();
				this->State->DiscardFailedPublications(*Pending.CommandState);
				if (Pending.Document != nullptr)
					Pending.Document->MarkModified();
			}
			this->State->Results.push_back(
				{.OperationID = Result.OperationID, .Committed = Result.Committed, .Diagnostic = std::move(Result.Diagnostic)});
		}
		catch (...)
		{
			try
			{
				Pending.CommandState->Fail();
				this->State->DiscardFailedPublications(*Pending.CommandState);
				if (Pending.Document != nullptr)
					Pending.Document->MarkModified();
				if (!Result.TransactionRoot.empty() && std::filesystem::exists(Result.TransactionRoot))
					RollbackPersistenceTransaction(Result.TransactionRoot, this->State->IntermediateRoot, this->State->ContentRoot);
			}
			catch (...)
			{
			}
		}
		for (const string &TargetKey : Pending.TargetKeys)
			this->State->ActiveTargetKeys.erase(TargetKey);
	}
	this->State->Pending.clear();
	while (this->State->LastScheduler != nullptr)
	{
		if (!this->State->Active.has_value() && !this->State->StartNextRetirement(*this->State->LastScheduler))
			break;
		this->State->RetirementContent->Wait();
		this->State->CompleteRetirement();
	}
}

bool PrivateMaterialAssignmentService::HasPendingWork() const
{
	this->State->RequireActive();
	std::scoped_lock Lock(this->State->RetirementMutex);
	return !this->State->Pending.empty() || this->State->Active.has_value() || !this->State->RetirementCandidates.empty();
}

bool PrivateMaterialAssignmentService::IsShutdown() const noexcept
{
	return this->State == nullptr || this->State->IsShutdown;
}

std::optional<PrivateMaterialAssignmentResult> PrivateMaterialAssignmentService::TakeResult(const util::UUID &OperationID)
{
	this->State->RequireOwnerThread();
	const auto Match = std::ranges::find(this->State->Results, OperationID, &PrivateMaterialAssignmentResult::OperationID);
	if (Match == this->State->Results.end())
		return std::nullopt;
	PrivateMaterialAssignmentResult Result = std::move(*Match);
	this->State->Results.erase(Match);
	return Result;
}

std::vector<PrivateMaterialAssignmentResult> PrivateMaterialAssignmentService::TakeResults()
{
	this->State->RequireOwnerThread();
	std::vector<PrivateMaterialAssignmentResult> Results = std::move(this->State->Results);
	this->State->Results.clear();
	return Results;
}
} // namespace editor::material
