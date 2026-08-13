#pragma once

#include "Source/resource/asset/MaterialAsset.h"
#include "Source/types.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <vector>

namespace resource
{
class AssetManager;
}

namespace editor::material
{
enum class MaterialDocumentType : uint8
{
	Material,
	MaterialInstance
};

struct MaterialParentReference final
{
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Material;
};

struct MaterialTextureReference final
{
	resource::MaterialTextureSemantic Semantic = resource::MaterialTextureSemantic::BaseColor;
	resource::AssetID ID;
	uint32 TextureCoordinateChannel = 0;
};

struct MaterialFactorOverrides final
{
	std::optional<glm::vec4> BaseColor;
	std::optional<glm::vec3> Emissive;
	std::optional<float32> Metallic;
	std::optional<float32> Roughness;
	std::optional<float32> Specular;
	std::optional<float32> NormalScale;
	std::optional<float32> OcclusionStrength;
	std::optional<float32> AlphaCutoff;
	std::optional<float32> ClearCoat;
	std::optional<float32> ClearCoatRoughness;
	std::optional<float32> Transmission;
	std::optional<float32> IndexOfRefraction;

	[[nodiscard]] bool Empty() const noexcept;
	void Apply(resource::PBRMaterialFactors &Factors) const noexcept;
};

struct MaterialDocument final
{
	static constexpr uint32 CurrentFormatVersion = 2;

	uint32 FormatVersion = CurrentFormatVersion;
	resource::AssetID DocumentID;
	MaterialDocumentType Type = MaterialDocumentType::Material;
	string Name;
	std::filesystem::path Path;
	resource::MaterialPipelineContract Pipeline;
	std::optional<resource::MaterialPipelineContract> PipelineOverride;
	resource::PBRMaterialFactors Factors;
	MaterialFactorOverrides FactorOverrides;
	std::optional<MaterialParentReference> Parent;
	std::vector<MaterialTextureReference> Textures;
	std::vector<MaterialTextureReference> TextureOverrides;
};

// MaterialDocument is the editor-facing material definition contract.
using MaterialDefinition = MaterialDocument;

class MaterialDocumentException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class MaterialDocumentStore final
{
  public:
	[[nodiscard]] static MaterialDocument Load(const std::filesystem::path &Path);
	static void Save(const MaterialDocument &Document, const std::filesystem::path &Path = {});
	static void Validate(const MaterialDocument &Document);
	[[nodiscard]] static MaterialDocument Resolve(const MaterialDocument &Document, resource::AssetManager &Assets);
};

enum class MaterialPreviewState : uint8
{
	Empty,
	Requested,
	Realizing,
	Ready,
	RetirementPending,
	Failed
};

struct MaterialPreviewSnapshot final
{
	uint64 Revision = 0;
	uint64 TextureHandle = 0;
	MaterialPreviewState State = MaterialPreviewState::Empty;
	string Diagnostic;
};

class MaterialPreviewResource final
{
  public:
	MaterialPreviewResource() = default;
	~MaterialPreviewResource();
	MaterialPreviewResource(const MaterialPreviewResource &) = delete;
	MaterialPreviewResource &operator=(const MaterialPreviewResource &) = delete;

	void Request(uint64 Revision);
	[[nodiscard]] std::stop_token BeginRealization(uint64 Revision);
	void Publish(uint64 Revision, uint64 TextureHandle, std::function<void(uint64)> QueueRetirement);
	void Fail(uint64 Revision, string Diagnostic);
	void Retire() noexcept;
	[[nodiscard]] MaterialPreviewSnapshot Snapshot() const;

  private:
	mutable std::mutex Mutex;
	std::stop_source Cancellation;
	std::function<void(uint64)> QueueRetirement;
	MaterialPreviewSnapshot Current;
};

class MaterialEditorSession final
{
  public:
	static MaterialEditorSession Open(const std::filesystem::path &Path, resource::AssetManager &Assets);
	[[nodiscard]] const MaterialDocument &GetDocument() const noexcept;
	[[nodiscard]] MaterialDocument &Edit();
	void CommitEdit(MaterialDocument Before);
	void BeginEditGesture(MaterialDocument Before);
	void EndEditGesture();
	[[nodiscard]] bool HasActiveEditGesture() const noexcept;
	[[nodiscard]] bool IsDirty() const noexcept;
	[[nodiscard]] bool HasReloadConflict() const noexcept;
	void Save();
	void Reload(bool DiscardChanges);
	[[nodiscard]] bool Undo();
	[[nodiscard]] uint64 GetPreviewRevision() const noexcept;
	[[nodiscard]] std::stop_token BeginPreviewTask();
	void CancelPreviewTasks() noexcept;
	[[nodiscard]] std::shared_ptr<MaterialPreviewResource> GetPreviewResource() const noexcept;

  private:
	MaterialDocument Document;
	std::vector<MaterialDocument> UndoStack;
	std::optional<MaterialDocument> ActiveGestureBefore;
	std::filesystem::file_time_type BaselineWriteTime{};
	uint64 BaselineRevision = 0;
	uint64 Revision = 0;
	std::stop_source PreviewStop;
	std::shared_ptr<MaterialPreviewResource> Preview = std::make_shared<MaterialPreviewResource>();
	resource::AssetManager *Assets = nullptr;
	bool Dirty = false;
	bool ReloadConflict = false;
};
} // namespace editor::material
