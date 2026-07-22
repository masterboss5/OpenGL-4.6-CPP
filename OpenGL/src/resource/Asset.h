#pragma once
#include "src/concepts.h"
#include "src/util/UUID.h"

#include <atomic>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

namespace pipeline::device
{
class Device;
}

namespace resource
{
template <IsAsset T> class AssetPtr;

struct AssetGPURealizationResult final
{
	bool Succeeded = true;
	std::string Error;
};

class Asset
{
  private:
	util::UUID UUID;
	std::atomic<uint32> ReferenceCount = 0;

  public:
	explicit Asset(util::UUID UUID) : UUID(UUID)
	{
	}

	Asset(const Asset &) = delete;
	Asset &operator=(const Asset &) = delete;

	Asset(Asset &&Other) noexcept : UUID(std::move(Other.UUID))
	{
	}

	Asset &operator=(Asset &&) = delete;

	virtual ~Asset() = default;

	[[nodiscard]] const util::UUID &GetUUID() const
	{
		return this->UUID;
	}

	[[nodiscard]] uint32 GetReferenceCount() const noexcept
	{
		return this->ReferenceCount.load(std::memory_order_acquire);
	}

	void IncrementReferenceCount() noexcept
	{
		this->ReferenceCount.fetch_add(1, std::memory_order_relaxed);
	}

	void DecrementReferenceCount() noexcept
	{
		uint32 References = this->ReferenceCount.load(std::memory_order_acquire);
		while (References != 0)
		{
			if (this->ReferenceCount.compare_exchange_weak(References, References - 1, std::memory_order_acq_rel,
														   std::memory_order_acquire))
			{
				if (References == 1)
				{
					delete this;
				}
				return;
			}
		}

		std::terminate();
	}

	[[nodiscard]] virtual bool RequiresGPURealization() const noexcept
	{
		return false;
	}

	[[nodiscard]] virtual AssetGPURealizationResult RealizeGPU(pipeline::device::Device &)
	{
		return {};
	}
};

template <IsAsset T> class AssetPtr final
{
  public:
	AssetPtr() = default;

	AssetPtr(std::nullptr_t) noexcept
	{
	}

	AssetPtr(const AssetPtr &Other) noexcept : Asset(Other.Asset)
	{
		if (this->Asset != nullptr)
		{
			this->Asset->IncrementReferenceCount();
		}
	}

	template <IsAsset U>
		requires std::is_convertible_v<U *, T *>
	AssetPtr(const AssetPtr<U> &Other) noexcept : Asset(Other.Asset)
	{
		if (this->Asset != nullptr)
		{
			this->Asset->IncrementReferenceCount();
		}
	}

	AssetPtr(AssetPtr &&Other) noexcept : Asset(std::exchange(Other.Asset, nullptr))
	{
	}

	template <IsAsset U>
		requires std::is_convertible_v<U *, T *>
	AssetPtr(AssetPtr<U> &&Other) noexcept : Asset(Other.Detach())
	{
	}

	~AssetPtr()
	{
		this->Reset();
	}

	AssetPtr &operator=(const AssetPtr &Other) noexcept
	{
		if (this != &Other)
		{
			this->Assign(Other.Asset);
		}
		return *this;
	}

	AssetPtr &operator=(AssetPtr &&Other) noexcept
	{
		if (this != &Other)
		{
			this->Reset();
			this->Asset = std::exchange(Other.Asset, nullptr);
		}
		return *this;
	}

	[[nodiscard]] T *Get() const noexcept
	{
		return this->Asset;
	}

	[[nodiscard]] T *operator->() const noexcept
	{
		return this->Asset;
	}

	[[nodiscard]] T &operator*() const noexcept
	{
		return *this->Asset;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->Asset != nullptr;
	}

	[[nodiscard]] bool operator==(std::nullptr_t) const noexcept
	{
		return this->Asset == nullptr;
	}

	void Reset() noexcept
	{
		if (this->Asset != nullptr)
		{
			T *PreviousAsset = std::exchange(this->Asset, nullptr);
			PreviousAsset->DecrementReferenceCount();
		}
	}

	template <typename... ArgumentTypes> [[nodiscard]] static AssetPtr Make(ArgumentTypes &&...Arguments)
	{
		AssetPtr Pointer;
		Pointer.Asset = new T(std::forward<ArgumentTypes>(Arguments)...);
		Pointer.Asset->IncrementReferenceCount();
		return Pointer;
	}

	[[nodiscard]] static AssetPtr Retain(T *Asset) noexcept
	{
		return AssetPtr(Asset);
	}

  private:
	T *Asset = nullptr;

	explicit AssetPtr(T *Asset) noexcept : Asset(Asset)
	{
		if (this->Asset != nullptr)
		{
			this->Asset->IncrementReferenceCount();
		}
	}

	void Assign(T *NewAsset) noexcept
	{
		if (NewAsset != nullptr)
		{
			NewAsset->IncrementReferenceCount();
		}
		this->Reset();
		this->Asset = NewAsset;
	}

	[[nodiscard]] T *Detach() noexcept
	{
		return std::exchange(this->Asset, nullptr);
	}

	template <IsAsset U> friend class AssetPtr;
};
} // namespace resource
