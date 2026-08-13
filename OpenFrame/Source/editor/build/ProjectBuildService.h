#pragma once

#include "Source/core/threading/TaskScheduler.h"
#include "Source/types.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace editor::build
{
enum class ProjectBuildState : uint8
{
	Idle,
	Building,
	Succeeded,
	Cancelled,
	Failed
};

struct GameModuleBuildSpecification final
{
	std::filesystem::path MSBuildExecutable;
	std::filesystem::path Solution;
	string Target = "GameModule";
	string Configuration = "Debug";
	string Platform = "x64";
	std::filesystem::path BuiltModule;
	std::filesystem::path PublishedModule;
	std::filesystem::path PackageOutputRoot;
	std::filesystem::path EngineContentRoot;
};

struct ProjectBuildResult final
{
	ProjectBuildState State = ProjectBuildState::Failed;
	uint32 ExitCode = 0;
	std::filesystem::path PublishedModule;
	std::filesystem::path RuntimeDirectory;
	string Diagnostic;
};

class ProjectBuildException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ProjectBuildService final
{
  public:
	ProjectBuildService() = default;
	~ProjectBuildService();

	ProjectBuildService(const ProjectBuildService &) = delete;
	ProjectBuildService &operator=(const ProjectBuildService &) = delete;
	ProjectBuildService(ProjectBuildService &&) = delete;
	ProjectBuildService &operator=(ProjectBuildService &&) = delete;

	void Configure(GameModuleBuildSpecification Specification);
	void BeginGameModuleBuild(core::threading::TaskScheduler &Scheduler);
	void BeginProjectBuild(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll();
	void Cancel() noexcept;
	void Wait() noexcept;
	void ReportPostBuildFailure(string Diagnostic);

	[[nodiscard]] bool IsConfigured() const;
	[[nodiscard]] bool CanBuildGameModule() const;
	[[nodiscard]] ProjectBuildState GetState() const;
	[[nodiscard]] std::optional<ProjectBuildResult> GetResult() const;

  private:
	[[nodiscard]] ProjectBuildResult ExecuteBuild(bool CompleteProject) const;

	GameModuleBuildSpecification Specification;
	std::future<ProjectBuildResult> Pending;
	std::atomic<bool> CancelRequested = false;
	ProjectBuildState State = ProjectBuildState::Idle;
	std::optional<ProjectBuildResult> Result;
	mutable std::mutex Mutex;
};
} // namespace editor::build
