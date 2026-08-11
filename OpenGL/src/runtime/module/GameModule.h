#pragma once

#include "src/core/EngineAPI.h"

#include "GameModuleAPI.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace runtime::module
{
class ENGINE_API GameModuleException : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ENGINE_API GameModuleLoadException final : public GameModuleException
{
  public:
	using GameModuleException::GameModuleException;
};

class ENGINE_API GameModuleABIException final : public GameModuleException
{
  public:
	using GameModuleException::GameModuleException;
};

class ENGINE_API GameModuleRegistrationException final : public GameModuleException
{
  public:
	using GameModuleException::GameModuleException;
};

class ENGINE_API GameModule final
{
  public:
	~GameModule();

	GameModule(const GameModule &) = delete;
	GameModule &operator=(const GameModule &) = delete;
	GameModule(GameModule &&) = delete;
	GameModule &operator=(GameModule &&) = delete;

	[[nodiscard]] static std::unique_ptr<GameModule> Load(const std::filesystem::path &SourcePath, const std::filesystem::path &CacheRoot);
	void PrepareReload();
	void Activate() noexcept;

	[[nodiscard]] const string &GetName() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetSourcePath() const noexcept;
	[[nodiscard]] const std::filesystem::path &GetLoadedPath() const noexcept;
	[[nodiscard]] const std::vector<behavior::BehaviorDescriptor> &GetBehaviors() const noexcept;

  private:
	class Implementation;
	explicit GameModule(std::shared_ptr<Implementation> Implementation);

	std::shared_ptr<Implementation> State;
};

struct GameModuleReloadHooks final
{
	std::function<void()> Prepare;
	std::function<void()> Apply;
	std::function<void()> Rollback;
};

class ENGINE_API GameModuleManager final
{
  public:
	explicit GameModuleManager(behavior::BehaviorRegistry &Registry) noexcept;
	~GameModuleManager();

	GameModuleManager(const GameModuleManager &) = delete;
	GameModuleManager &operator=(const GameModuleManager &) = delete;
	GameModuleManager(GameModuleManager &&) = delete;
	GameModuleManager &operator=(GameModuleManager &&) = delete;

	void Configure(std::filesystem::path SourcePath, std::filesystem::path CacheRoot);
	[[nodiscard]] bool PollReload(bool RuntimeQuiescent);
	[[nodiscard]] bool PollReload(bool RuntimeQuiescent, const GameModuleReloadHooks &Hooks);
	[[nodiscard]] bool ForceReload(bool RuntimeQuiescent);
	[[nodiscard]] bool ForceReload(bool RuntimeQuiescent, const GameModuleReloadHooks &Hooks);
	void Unload(bool RuntimeQuiescent);

	[[nodiscard]] bool IsConfigured() const noexcept;
	[[nodiscard]] bool IsLoaded() const noexcept;
	[[nodiscard]] bool IsReloadPending() const noexcept;
	[[nodiscard]] const string &GetDiagnostic() const noexcept;
	[[nodiscard]] const GameModule *GetLoadedModule() const noexcept;

  private:
	[[nodiscard]] bool TryReplace(std::filesystem::file_time_type WriteTime, const GameModuleReloadHooks &Hooks);

	behavior::BehaviorRegistry *Registry = nullptr;
	std::filesystem::path SourcePath;
	std::filesystem::path CacheRoot;
	std::filesystem::file_time_type LastAttemptedWriteTime{};
	bool HasAttemptedWriteTime = false;
	bool ReloadPending = false;
	std::unique_ptr<GameModule> LoadedModule;
	string Diagnostic;
};
} // namespace runtime::module
