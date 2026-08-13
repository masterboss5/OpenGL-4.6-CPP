#pragma once

#include "Source/core/threading/TaskScheduler.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace resource
{
class AssetManager;
}

namespace editor
{
namespace document
{
class SceneDocument;
}
namespace reflection
{
class ReflectionRegistry;
}
} // namespace editor

namespace editor::recovery
{
class EditorRecoveryException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

struct EditorRecoverySpecification final
{
	std::chrono::seconds AutosaveInterval{60};
	std::chrono::seconds QuietPeriod{3};
};

struct EditorRecoveryCandidate final
{
	util::UUID DocumentID;
	string DocumentName;
	std::filesystem::path SnapshotPath;
	std::filesystem::path OriginalPath;
	uint64 Revision = 0;
	int64 TimestampMilliseconds = 0;
	uint64 SnapshotChecksum = 0;
};

struct EditorRecoveryResult final
{
	EditorRecoveryCandidate Candidate;
	string Diagnostic;
};

class EditorRecoveryStore final
{
  public:
	EditorRecoveryStore(std::filesystem::path AutosaveRoot, std::filesystem::path RecoveryRoot,
						EditorRecoverySpecification Specification = {});
	~EditorRecoveryStore();

	EditorRecoveryStore(const EditorRecoveryStore &) = delete;
	EditorRecoveryStore &operator=(const EditorRecoveryStore &) = delete;
	EditorRecoveryStore(EditorRecoveryStore &&) = delete;
	EditorRecoveryStore &operator=(EditorRecoveryStore &&) = delete;

	void Tick(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
			  core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll();
	void Wait() noexcept;
	void Force(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
			   core::threading::TaskScheduler &Scheduler);
	void SetSpecification(EditorRecoverySpecification Specification);

	[[nodiscard]] std::vector<EditorRecoveryCandidate> Scan() const;
	void Discard(const EditorRecoveryCandidate &Candidate);
	void Discard(const util::UUID &DocumentID);
	void AcknowledgeSaved(const util::UUID &DocumentID, uint64 Revision);
	void ResetTracking() noexcept;
	[[nodiscard]] bool IsBusy() const noexcept;
	[[nodiscard]] const std::optional<EditorRecoveryResult> &GetLastResult() const noexcept;

  private:
	void Begin(document::SceneDocument &Document, const reflection::ReflectionRegistry &Reflection, resource::AssetManager &Assets,
			   core::threading::TaskScheduler &Scheduler);
	void CompletePending() noexcept;
	[[nodiscard]] std::filesystem::path ManifestPath(const util::UUID &DocumentID) const;

	std::filesystem::path AutosaveRoot;
	std::filesystem::path RecoveryRoot;
	EditorRecoverySpecification Specification;
	std::future<EditorRecoveryResult> Pending;
	std::optional<EditorRecoveryResult> LastResult;
	uint64 ObservedRevision = 0;
	uint64 PublishedRevision = 0;
	std::chrono::steady_clock::time_point LastMutation = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point LastPublication = std::chrono::steady_clock::time_point::min();
	util::UUID SuppressedDocument;
	uint64 SuppressedThroughRevision = 0;
};
} // namespace editor::recovery
