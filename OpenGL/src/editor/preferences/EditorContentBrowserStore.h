#pragma once

#include "src/resource/asset/AssetTypes.h"
#include "src/types.h"

#include <filesystem>
#include <span>
#include <unordered_set>

namespace editor::preferences
{
class EditorContentBrowserStore final
{
  public:
	explicit EditorContentBrowserStore(std::filesystem::path Path);

	[[nodiscard]] std::unordered_set<resource::AssetID> Load() const;
	void Save(std::span<const resource::AssetID> FavoriteAssetIDs) const;

	[[nodiscard]] const std::filesystem::path &GetPath() const noexcept;

  private:
	static constexpr uint32 CurrentFormatVersion = 1;

	std::filesystem::path Path;
};
} // namespace editor::preferences
