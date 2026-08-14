#pragma once

#include "Source/editor/document/SceneDocument.h"
#include "Source/editor/reflection/ReflectionRegistry.h"
#include "Source/resource/asset/AssetManager.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <shared_mutex>
#include <stdexcept>

namespace editor::serialization
{
class SceneDocumentSerializationException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class SceneDocumentMigrationRegistry final
{
  public:
	using MigrationFunction = std::function<void(nlohmann::json &)>;

	void RegisterDocumentMigration(uint32 FromFormatVersion, uint32 FromEngineSchemaVersion, uint32 ToFormatVersion,
								   uint32 ToEngineSchemaVersion, MigrationFunction Migration);
	void RegisterComponentMigration(string ComponentName, uint32 FromSchemaVersion, uint32 ToSchemaVersion, MigrationFunction Migration);
	void Migrate(nlohmann::json &Root, uint32 TargetFormatVersion, uint32 TargetEngineSchemaVersion,
				 uint32 TargetComponentSchemaVersion) const;

  private:
	struct DocumentMigration final
	{
		uint32 ToFormatVersion = 0;
		uint32 ToEngineSchemaVersion = 0;
		MigrationFunction Apply;
	};
	struct ComponentMigration final
	{
		uint32 ToSchemaVersion = 0;
		MigrationFunction Apply;
	};

	mutable std::shared_mutex Mutex;
	std::map<std::pair<uint32, uint32>, DocumentMigration> DocumentMigrations;
	std::map<std::pair<string, uint32>, ComponentMigration> ComponentMigrations;
};

class SceneDocumentSerializer final
{
  public:
	static constexpr uint32 CurrentFormatVersion = 2;
	static constexpr uint32 CurrentEngineSchemaVersion = 1;
	static constexpr uint32 CurrentComponentSchemaVersion = 1;

	static void Save(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
					 const std::filesystem::path &Path = {});
	static void SaveSnapshot(const util::UUID &DocumentID, string_view DocumentName, const world::Scene &Scene,
							 const instance::InstanceGraphSnapshot &Instances, const reflection::ReflectionRegistry &Reflection,
							 resource::AssetManager &Assets, const std::filesystem::path &Path, uint64 Revision,
							 int64 TimestampMilliseconds);
	[[nodiscard]] static std::unique_ptr<document::SceneDocument> Load(const std::filesystem::path &Path,
																	   const reflection::ReflectionRegistry &Reflection,
																	   resource::AssetManager &Assets, usize CommandHistoryCapacity = 4'096,
																	   const SceneDocumentMigrationRegistry *Migrations = nullptr);
};
} // namespace editor::serialization
