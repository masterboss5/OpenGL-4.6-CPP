#pragma once
#include <atomic>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#include "src/util/UUID.h"

namespace resource
{
	template<typename T>
	class AssetPtr;

	struct AssetGpuRealizationResult final
	{
		bool succeeded = true;
		std::string error;
	};

	class Asset
	{
	private:
		util::UUID uuid;
		std::atomic<uint32> referenceCount = 0;

	public:
		explicit Asset(util::UUID uuid)
			: uuid(uuid)
		{
		}

		Asset(const Asset&) = delete;
		Asset& operator=(const Asset&) = delete;

		Asset(Asset&& other) noexcept
			: uuid(std::move(other.uuid))
		{
		}

		Asset& operator=(Asset&&) = delete;

		virtual ~Asset() = default;

		const util::UUID& getUUID() const
		{
			return this->uuid;
		}

		[[nodiscard]] uint32 getReferenceCount() const noexcept
		{
			return this->referenceCount.load(std::memory_order_acquire);
		}

		void incrementReferenceCount() noexcept
		{
			this->referenceCount.fetch_add(1, std::memory_order_relaxed);
		}

		void decrementReferenceCount() noexcept
		{
			uint32 references = this->referenceCount.load(std::memory_order_acquire);
			while (references != 0)
			{
				if (this->referenceCount.compare_exchange_weak(
					references,
					references - 1,
					std::memory_order_acq_rel,
					std::memory_order_acquire))
				{
					if (references == 1)
					{
						delete this;
					}
					return;
				}
			}

			std::terminate();
		}

		virtual bool requiresGpuRealization() const noexcept
		{
			return false;
		}

		virtual AssetGpuRealizationResult realizeGpu()
		{
			return {};
		}
	};

	template<typename T>
	class AssetPtr final
	{
		static_assert(std::is_base_of_v<Asset, T>, "AssetPtr requires an Asset-derived type");

	public:
		AssetPtr() = default;

		AssetPtr(std::nullptr_t) noexcept
		{
		}

		AssetPtr(const AssetPtr& other) noexcept
			: asset(other.asset)
		{
			if (this->asset != nullptr)
			{
				this->asset->incrementReferenceCount();
			}
		}

		template<typename U>
		requires std::is_convertible_v<U*, T*>
		AssetPtr(const AssetPtr<U>& other) noexcept
			: asset(other.asset)
		{
			if (this->asset != nullptr)
			{
				this->asset->incrementReferenceCount();
			}
		}

		AssetPtr(AssetPtr&& other) noexcept
			: asset(std::exchange(other.asset, nullptr))
		{
		}

		template<typename U>
		requires std::is_convertible_v<U*, T*>
		AssetPtr(AssetPtr<U>&& other) noexcept
			: asset(other.detach())
		{
		}

		~AssetPtr()
		{
			this->reset();
		}

		AssetPtr& operator=(const AssetPtr& other) noexcept
		{
			if (this != &other)
			{
				this->assign(other.asset);
			}
			return *this;
		}

		AssetPtr& operator=(AssetPtr&& other) noexcept
		{
			if (this != &other)
			{
				this->reset();
				this->asset = std::exchange(other.asset, nullptr);
			}
			return *this;
		}

		[[nodiscard]] T* get() const noexcept
		{
			return this->asset;
		}

		[[nodiscard]] T* operator->() const noexcept
		{
			return this->asset;
		}

		[[nodiscard]] T& operator*() const noexcept
		{
			return *this->asset;
		}

		explicit operator bool() const noexcept
		{
			return this->asset != nullptr;
		}

		[[nodiscard]] bool operator==(std::nullptr_t) const noexcept
		{
			return this->asset == nullptr;
		}

		void reset() noexcept
		{
			if (this->asset != nullptr)
			{
				T* previousAsset = std::exchange(this->asset, nullptr);
				previousAsset->decrementReferenceCount();
			}
		}

		template<typename... Arguments>
		[[nodiscard]] static AssetPtr make(Arguments&&... arguments)
		{
			AssetPtr pointer;
			pointer.asset = new T(std::forward<Arguments>(arguments)...);
			pointer.asset->incrementReferenceCount();
			return pointer;
		}

		[[nodiscard]] static AssetPtr retain(T* asset) noexcept
		{
			return AssetPtr(asset);
		}

	private:
		T* asset = nullptr;

		explicit AssetPtr(T* asset) noexcept
			: asset(asset)
		{
			if (this->asset != nullptr)
			{
				this->asset->incrementReferenceCount();
			}
		}

		void assign(T* newAsset) noexcept
		{
			if (newAsset != nullptr)
			{
				newAsset->incrementReferenceCount();
			}
			this->reset();
			this->asset = newAsset;
		}

		[[nodiscard]] T* detach() noexcept
		{
			return std::exchange(this->asset, nullptr);
		}

		template<typename U>
		friend class AssetPtr;
	};
}
