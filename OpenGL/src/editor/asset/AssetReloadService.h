#pragma once

#include "src/core/threading/TaskScheduler.h"
#include "src/resource/asset/AssetManager.h"

#include <filesystem>
#include <future>
#include <optional>

namespace editor::asset
{
struct AssetReloadResult final
{
	resource::AssetID ID;
	bool Succeeded = false;
	string Diagnostic;
};

class AssetReloadService final
{
  public:
	AssetReloadService(resource::AssetManager &Assets, std::filesystem::path ContentRoot);
	~AssetReloadService();

	AssetReloadService(const AssetReloadService &) = delete;
	AssetReloadService &operator=(const AssetReloadService &) = delete;

	void Begin(core::threading::TaskScheduler &Scheduler, resource::AssetID ID, std::filesystem::path MetadataPath);
	void BeginChanged(core::threading::TaskScheduler &Scheduler);
	[[nodiscard]] bool Poll();
	void Wait() noexcept;

	[[nodiscard]] bool IsBusy() const noexcept;
	[[nodiscard]] const std::optional<AssetReloadResult> &GetResult() const noexcept;

  private:
	resource::AssetManager *Assets = nullptr;
	std::filesystem::path ContentRoot;
	std::future<AssetReloadResult> Pending;
	std::optional<AssetReloadResult> Result;
};
} // namespace editor::asset
