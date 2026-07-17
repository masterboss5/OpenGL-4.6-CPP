#pragma once

#include "src/concepts.h"

namespace resource
{
	class AssetManager;

	template<typename T> requires IsAsset<T>
	class AssetHandle final
	{
	public:
		AssetHandle() = default;

		explicit AssetHandle(T* asset, AssetManager* assetManager)
			: asset(asset),
			assetManager(assetManager)
		{
			if (this->asset != nullptr)
			{
				this->asset->incrementReferenceCount();
			}
		}

		~AssetHandle()
		{
			if (this->asset != nullptr)
			{
				this->asset->decrementReferenceCount();
			}
			this->asset = nullptr;
			this->assetManager = nullptr;
		}

		AssetHandle(const AssetHandle& other) noexcept
			: asset(other.asset),
			assetManager(other.assetManager)
		{
			if (this->asset != nullptr)
			{
				this->asset->incrementReferenceCount();
			}
		}

		AssetHandle& operator=(const AssetHandle& other) noexcept
		{
			if (this != &other)
			{
				if (this->asset != nullptr)
				{
					this->asset->decrementReferenceCount();
				}

				this->asset = other.asset;
				this->assetManager = other.assetManager;

				if (this->asset != nullptr)
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
				if (this->asset != nullptr)
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

		[[nodiscard]] T* get() const noexcept
		{
			return this->asset;
		}

		[[nodiscard]] T* operator->() const noexcept
		{
			return this->get();
		}

		[[nodiscard]] T& operator*() const
		{
			return *this->get();
		}

		[[nodiscard]] bool isValid() const noexcept
		{
			return this->get() != nullptr;
		}

		explicit operator bool() const noexcept
		{
			return this->isValid();
		}

	private:
		T* asset = nullptr;
		AssetManager* assetManager = nullptr;

		friend class AssetManager;
	};
}
