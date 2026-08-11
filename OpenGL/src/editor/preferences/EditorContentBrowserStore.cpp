#include "EditorContentBrowserStore.h"

#include "src/core/io/SecurePath.h"
#include "src/util/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace editor::preferences
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] std::span<const uint8> BytesOf(const string &Text) noexcept
{
	return {reinterpret_cast<const uint8 *>(Text.data()), Text.size()};
}
} // namespace

EditorContentBrowserStore::EditorContentBrowserStore(std::filesystem::path Path)
	: Path(std::filesystem::absolute(std::move(Path)).lexically_normal())
{
	if (this->Path.empty())
		throw std::invalid_argument("Content Browser state requires a path");
}

std::unordered_set<resource::AssetID> EditorContentBrowserStore::Load() const
{
	if (!std::filesystem::is_regular_file(this->Path))
		return {};

	try
	{
		constexpr uint64 MaximumStateBytes = 4U * 1024U * 1024U;
		const std::vector<uint8> Bytes = core::io::SecurePath::ReadFileWithin(this->Path.parent_path(), this->Path.filename(),
																			  MaximumStateBytes, "Content Browser state");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object() || Root.at("FormatVersion").get<uint32>() != CurrentFormatVersion || !Root.at("FavoriteAssetIDs").is_array())
		{
			throw std::runtime_error("state root, version, or favorite table is invalid");
		}

		std::unordered_set<resource::AssetID> FavoriteAssetIDs;
		for (const Json &Value : Root.at("FavoriteAssetIDs"))
		{
			if (!Value.is_string())
				throw std::runtime_error("favorite asset ID is not a string");
			resource::AssetID ID = Value.get<string>();
			if (!util::UUID::TryParse(ID).has_value())
				throw std::runtime_error("favorite asset ID is not a canonical UUID");
			if (!FavoriteAssetIDs.emplace(std::move(ID)).second)
				throw std::runtime_error("favorite asset ID is duplicated");
		}
		return FavoriteAssetIDs;
	}
	catch (const std::exception &Exception)
	{
		throw std::runtime_error("Could not load Content Browser state '" + this->Path.string() + "': " + Exception.what());
	}
}

void EditorContentBrowserStore::Save(const std::span<const resource::AssetID> FavoriteAssetIDs) const
{
	std::vector<resource::AssetID> SortedIDs(FavoriteAssetIDs.begin(), FavoriteAssetIDs.end());
	std::ranges::sort(SortedIDs);
	if (std::ranges::adjacent_find(SortedIDs) != SortedIDs.end())
		throw std::invalid_argument("Content Browser favorites contain duplicate asset IDs");
	for (const resource::AssetID &ID : SortedIDs)
	{
		if (!util::UUID::TryParse(ID).has_value())
			throw std::invalid_argument("Content Browser favorite is not a canonical asset ID");
	}

	const Json Root{{"FormatVersion", CurrentFormatVersion}, {"FavoriteAssetIDs", SortedIDs}};
	const std::filesystem::path DirectoryRoot = this->Path.parent_path();
	const std::filesystem::path Destination = this->Path.filename();
	const std::filesystem::path Temporary = Destination.string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	const string Serialized = Root.dump(2) + '\n';
	core::io::SecurePath::CreateTrustedRoot(DirectoryRoot, "Content Browser state root");
	core::io::SecurePath::WriteFileWithin(DirectoryRoot, Temporary, BytesOf(Serialized), false, true,
										  "Content Browser state temporary file");
	try
	{
		core::io::SecurePath::ReplaceWithin(DirectoryRoot, Temporary, Destination, "Content Browser state publication");
	}
	catch (...)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(DirectoryRoot, Temporary, false, "Content Browser state temporary cleanup");
		}
		catch (...)
		{
		}
		throw;
	}
}

const std::filesystem::path &EditorContentBrowserStore::GetPath() const noexcept
{
	return this->Path;
}
} // namespace editor::preferences
