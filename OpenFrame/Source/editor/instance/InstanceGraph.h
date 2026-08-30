#pragma once

#include "Source/editor/instance/InstanceTypeRegistry.h"

#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace editor::instance
{
struct InstanceGraphSnapshot final
{
	uint64 Revision = 0;
	std::vector<InstanceRecord> Instances;
};

class InstanceGraph final
{
  public:
	explicit InstanceGraph(const InstanceTypeRegistry &Types, bool CreateServices = true);

	InstanceGraph(const InstanceGraph &) = delete;
	InstanceGraph &operator=(const InstanceGraph &) = delete;
	InstanceGraph(InstanceGraph &&) = delete;
	InstanceGraph &operator=(InstanceGraph &&) = delete;

	[[nodiscard]] util::UUID Create(const InstanceClassID &ClassID, const util::UUID &Parent, string Name = {},
									util::UUID ID = util::UUID::GenerateRandomUUID(), InstancePropertyMap InitialProperties = {});
	void Destroy(const util::UUID &ID);
	void Rename(const util::UUID &ID, string Name);
	void SetClass(const util::UUID &ID, const InstanceClassID &ClassID);
	void Reparent(const util::UUID &ID, const util::UUID &Parent, uint32 SiblingOrder);
	void SetEnabled(const util::UUID &ID, bool Enabled);
	void SetProperty(const util::UUID &ID, string Name, InstancePropertyValue Value);
	void RemoveProperty(const util::UUID &ID, string_view Name);
	void ApplyModelPivotDelta(const util::UUID &Model, const glm::vec3 &Translation, const glm::quat &Rotation);
	void SetWorldTransform(const util::UUID &ID, const glm::vec3 &Position, const glm::quat &Rotation, const glm::vec3 &Scale);

	[[nodiscard]] bool Contains(const util::UUID &ID) const;
	[[nodiscard]] InstanceRecord Get(const util::UUID &ID) const;
	[[nodiscard]] InstanceActivation GetActivation(const util::UUID &ID) const;
	[[nodiscard]] InstanceGraphSnapshot Snapshot() const;
	void LoadSnapshot(const InstanceGraphSnapshot &Snapshot);
	[[nodiscard]] uint64 GetRevision() const noexcept;
	[[nodiscard]] const InstanceTypeRegistry &GetTypes() const noexcept;

	[[nodiscard]] util::UUID GetWorkspace() const noexcept;
	[[nodiscard]] util::UUID GetLighting() const noexcept;
	[[nodiscard]] util::UUID GetGUI() const noexcept;
	[[nodiscard]] util::UUID GetAudio() const noexcept;
	[[nodiscard]] util::UUID GetScripts() const noexcept;
	[[nodiscard]] util::UUID FindByClass(const InstanceClassID &ClassID) const;

  private:
	const InstanceTypeRegistry *Types = nullptr;
	mutable std::shared_mutex Mutex;
	std::unordered_map<util::UUID, InstanceRecord> Records;
	std::vector<util::UUID> Roots;
	uint64 Revision = 1;
	std::thread::id OwnerThread;
	util::UUID Workspace;
	util::UUID Lighting;
	util::UUID GUI;
	util::UUID Audio;
	util::UUID Scripts;

	void AssertOwnerThread() const;
	void CreateServices();
	[[nodiscard]] bool IsDescendantUnlocked(const util::UUID &Candidate, const util::UUID &Ancestor) const;
	[[nodiscard]] string MakeUniqueNameUnlocked(const util::UUID &Parent, string BaseName) const;
	void NormalizeSiblingOrderUnlocked(const util::UUID &Parent);
	void Touch() noexcept;
};
} // namespace editor::instance
