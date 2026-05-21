#pragma once
#include "src/resource/asset/AssetTypes.h"
#include "src/concepts.h"
#include "src/resource/asset/AssetManager.h"

namespace resource
{
	class AssetManager;

	template<typename T> requires IsAsset<T> /*T Must be a sub type of resource::Asset*/
	class AssetHandle final
	{
	private:
		T* asset = nullptr;
		AssetManager* assetManager = nullptr;
	public:
		explicit AssetHandle(T* asset, AssetManager* assetManager)
			: asset(asset),
			assetManager(assetManager)
		{
			if (asset)
			{
				asset->incrementReferenceCount();
			}
		}

		~AssetHandle()
		{
			if (this->asset)
			{
				this->asset->decrementReferenceCount();
			}
			this->asset = nullptr;
		}

		AssetHandle(const AssetHandle& other) noexcept
			: asset(other.asset),
			assetManager(other.assetManager)
		{
			if (this->asset)
			{
				this->asset->incrementReferenceCount();
			}
		}

		AssetHandle& operator=(const AssetHandle& other) noexcept
		{
			if (this != &other)
			{
				if (this->asset)
				{
					this->asset->decrementReferenceCount();
				}
				this->asset = other.asset;
				this->assetManager = other.assetManager;
				if (this->asset)
				{
					this->asset->incrementReferenceCount();
				}
			}

			return *this;
		}

		AssetHandle(AssetHandle&& other) noexcept
			: asset(other.asset),
			assetManager(other.assetManager)
		{
			other.asset = nullptr;
			other.assetManager = nullptr;
		}

		AssetHandle& operator=(AssetHandle&& other) noexcept
		{
			if (this != &other)
			{
				if (this->asset)
				{
					this->asset->decrementReferenceCount();
				}
				this->asset = other.asset;
				this->assetManager = other.assetManager;
				other.asset = nullptr;
				other.assetManager = nullptr;
			}

			return *this;
		}

		T* get() const
		{
			return asset;
		}

		T* operator->() const
		{
			return asset;
		}

		bool isValid() const
		{
			return asset != nullptr;
		}

		explicit operator bool() const
		{
			return asset != nullptr;
		}
	};
}