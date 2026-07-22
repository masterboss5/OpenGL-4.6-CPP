#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetRecord.h"

#include <type_traits>
#include <utility>

namespace resource
{
namespace importer
{
class AssetImportContext;
}
template <IsAsset T> class WeakAssetHandle;

template <IsAsset T> class AssetHandle final
{
  public:
	AssetHandle() = default;
	AssetHandle(std::nullptr_t) noexcept
	{
	}

	AssetHandle(const AssetHandle &Other) noexcept : Record(Other.Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainStrong();
	}

	template <IsAsset U>
		requires std::is_convertible_v<U *, T *>
	AssetHandle(const AssetHandle<U> &Other) noexcept : Record(Other.Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainStrong();
	}

	AssetHandle(AssetHandle &&Other) noexcept : Record(std::exchange(Other.Record, nullptr))
	{
	}

	template <IsAsset U>
		requires std::is_convertible_v<U *, T *>
	AssetHandle(AssetHandle<U> &&Other) noexcept : Record(std::exchange(Other.Record, nullptr))
	{
	}

	~AssetHandle()
	{
		this->Reset();
	}

	AssetHandle &operator=(const AssetHandle &Other) noexcept
	{
		if (this != &Other)
		{
			AssetRecord *Replacement = Other.Record;
			if (Replacement != nullptr)
				Replacement->RetainStrong();
			this->Reset();
			this->Record = Replacement;
		}
		return *this;
	}

	AssetHandle &operator=(AssetHandle &&Other) noexcept
	{
		if (this != &Other)
		{
			this->Reset();
			this->Record = std::exchange(Other.Record, nullptr);
		}
		return *this;
	}

	[[nodiscard]] AssetPtr<T> Pin() const
	{
		if (this->Record == nullptr)
		{
			throw AssetUnavailableException({}, AssetType::Count, "handle is empty");
		}
		return this->Record->Pin<T>();
	}

	[[nodiscard]] AssetPtr<T> TryPin() const noexcept
	{
		return this->Record == nullptr ? AssetPtr<T>{} : this->Record->TryPin<T>();
	}

	[[nodiscard]] WeakAssetHandle<T> Weak() const noexcept;

	[[nodiscard]] const AssetID &GetID() const
	{
		if (this->Record == nullptr)
		{
			throw AssetUnavailableException({}, AssetType::Count, "handle is empty");
		}
		return this->Record->GetID();
	}

	[[nodiscard]] AssetLoadState GetState() const
	{
		return this->Record == nullptr ? AssetLoadState::Unloaded : this->Record->GetState();
	}

	[[nodiscard]] uint64 GetPublishedGeneration() const noexcept
	{
		return this->Record == nullptr ? 0 : this->Record->GetPublishedGeneration();
	}

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Record != nullptr;
	}
	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->IsValid();
	}

	void Reset() noexcept
	{
		if (this->Record != nullptr)
		{
			AssetRecord *Previous = std::exchange(this->Record, nullptr);
			Previous->ReleaseStrong();
		}
	}

  private:
	struct AdoptStrongReference final
	{
	};
	AssetRecord *Record = nullptr;

	explicit AssetHandle(AssetRecord *Record) noexcept : Record(Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainStrong();
	}

	AssetHandle(AssetRecord *Record, AdoptStrongReference) noexcept : Record(Record)
	{
	}

	friend class AssetManager;
	friend class importer::AssetImportContext;
	template <IsAsset U> friend class AssetHandle;
	template <IsAsset U> friend class WeakAssetHandle;
};

template <IsAsset T> class WeakAssetHandle final
{
  public:
	WeakAssetHandle() = default;

	WeakAssetHandle(const WeakAssetHandle &Other) noexcept : Record(Other.Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainWeak();
	}

	WeakAssetHandle(WeakAssetHandle &&Other) noexcept : Record(std::exchange(Other.Record, nullptr))
	{
	}
	~WeakAssetHandle()
	{
		this->Reset();
	}

	WeakAssetHandle &operator=(const WeakAssetHandle &Other) noexcept
	{
		if (this != &Other)
		{
			AssetRecord *Replacement = Other.Record;
			if (Replacement != nullptr)
				Replacement->RetainWeak();
			this->Reset();
			this->Record = Replacement;
		}
		return *this;
	}

	WeakAssetHandle &operator=(WeakAssetHandle &&Other) noexcept
	{
		if (this != &Other)
		{
			this->Reset();
			this->Record = std::exchange(Other.Record, nullptr);
		}
		return *this;
	}

	[[nodiscard]] AssetHandle<T> Lock() const noexcept
	{
		if (this->Record == nullptr || !this->Record->TryRetainStrong())
			return {};
		return AssetHandle<T>(this->Record, typename AssetHandle<T>::AdoptStrongReference{});
	}

	[[nodiscard]] bool Expired() const noexcept
	{
		AssetHandle<T> Strong = this->Lock();
		return !Strong;
	}

	void Reset() noexcept
	{
		if (this->Record != nullptr)
		{
			AssetRecord *Previous = std::exchange(this->Record, nullptr);
			Previous->ReleaseWeak();
		}
	}

  private:
	AssetRecord *Record = nullptr;
	explicit WeakAssetHandle(AssetRecord *Record) noexcept : Record(Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainWeak();
	}
	friend class AssetHandle<T>;
};

template <IsAsset T> WeakAssetHandle<T> AssetHandle<T>::Weak() const noexcept
{
	return WeakAssetHandle<T>(this->Record);
}
} // namespace resource
