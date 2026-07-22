#pragma once

#include "src/resource/asset/AssetTypes.h"
#include "src/types.h"

#include <stdexcept>
#include <utility>

namespace resource
{
class AssetAccessException : public std::runtime_error
{
  public:
	AssetAccessException(AssetID ID, AssetType Type, string Diagnostic)
		: std::runtime_error(std::move(Diagnostic)), ID(std::move(ID)), Type(Type)
	{
	}

	[[nodiscard]] const AssetID &GetAssetID() const noexcept
	{
		return this->ID;
	}
	[[nodiscard]] AssetType GetAssetType() const noexcept
	{
		return this->Type;
	}

  private:
	AssetID ID;
	AssetType Type;
};

class AssetUnavailableException final : public AssetAccessException
{
  public:
	AssetUnavailableException(const AssetID &ID, AssetType Type, const string &Diagnostic)
		: AssetAccessException(ID, Type, "Asset '" + ID + "' is unavailable: " + Diagnostic)
	{
	}
};

class AssetTypeMismatchException final : public AssetAccessException
{
  public:
	AssetTypeMismatchException(const AssetID &ID, AssetType Type)
		: AssetAccessException(ID, Type, "Asset '" + ID + "' does not match the requested C++ asset type")
	{
	}
};
} // namespace resource
