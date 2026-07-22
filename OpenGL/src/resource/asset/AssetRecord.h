#pragma once

#include "src/resource/Asset.h"
#include "src/resource/asset/AssetException.h"
#include "src/resource/asset/AssetTypes.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace resource
{
enum class AssetLoadState : uint8
{
	Unloaded,
	LoadingCPU,
	CPUReady,
	RealizingGPU,
	Ready,
	Failed
};

struct AssetDependency final
{
	AssetType Type;
	std::filesystem::path Path;
};

class AssetManager;
template <IsAsset T> class AssetHandle;
template <IsAsset T> class WeakAssetHandle;

class AssetRecord final
{
  public:
	AssetRecord(const AssetRecord &) = delete;
	AssetRecord &operator=(const AssetRecord &) = delete;

	[[nodiscard]] const AssetID &GetID() const noexcept
	{
		return this->ID;
	}
	[[nodiscard]] const std::filesystem::path &GetCanonicalPath() const noexcept
	{
		return this->CanonicalPath;
	}
	[[nodiscard]] AssetType GetType() const noexcept
	{
		return this->Type;
	}
	[[nodiscard]] AssetLoadState GetState() const
	{
		std::shared_lock Lock(this->PublicationMutex);
		return this->State;
	}
	[[nodiscard]] string GetError() const
	{
		std::shared_lock Lock(this->PublicationMutex);
		return this->Error;
	}
	[[nodiscard]] uint64 GetPublishedGeneration() const noexcept
	{
		return this->PublishedGeneration.load(std::memory_order_acquire);
	}

	template <IsAsset T> [[nodiscard]] AssetPtr<T> Pin() const
	{
		AssetPtr<resource::Asset> Current;
		AssetLoadState CurrentState;
		string Diagnostic;
		{
			std::shared_lock Lock(this->PublicationMutex);
			Current = this->Asset;
			CurrentState = this->State;
			Diagnostic = this->Error;
		}

		if (Current == nullptr)
		{
			if (Diagnostic.empty())
			{
				Diagnostic = "record state is " + std::to_string(static_cast<uint32>(CurrentState));
			}
			throw AssetUnavailableException(this->ID, this->Type, Diagnostic);
		}

		T *Typed = dynamic_cast<T *>(Current.Get());
		if (Typed == nullptr)
		{
			throw AssetTypeMismatchException(this->ID, this->Type);
		}
		return AssetPtr<T>::Retain(Typed);
	}

	template <IsAsset T> [[nodiscard]] AssetPtr<T> TryPin() const noexcept
	{
		try
		{
			return this->Pin<T>();
		}
		catch (...)
		{
			return {};
		}
	}

  private:
	AssetID ID;
	std::filesystem::path CanonicalPath;
	std::filesystem::file_time_type SourceWriteTime{};
	AssetType Type = AssetType::Count;
	AssetLoadState State = AssetLoadState::Unloaded;
	string Error;
	std::vector<AssetID> Dependencies;
	std::unordered_map<AssetID, std::filesystem::file_time_type> DependencyWriteTimes;
	AssetPtr<Asset> Asset;
	mutable std::shared_mutex PublicationMutex;
	mutable std::condition_variable_any PublicationChanged;
	std::exception_ptr LoadException;
	uint64 ActiveLoadOperation = 0;
	bool IsImportRoot = false;
	std::atomic<uint64> PublishedGeneration{0};
	std::atomic<uint32> StrongReferences{1};
	std::atomic<uint32> WeakReferences{1};

	AssetRecord(AssetID ID, std::filesystem::path CanonicalPath, AssetType Type)
		: ID(std::move(ID)), CanonicalPath(std::move(CanonicalPath)), Type(Type)
	{
	}

	~AssetRecord() = default;

	void RetainStrong() noexcept
	{
		this->StrongReferences.fetch_add(1, std::memory_order_relaxed);
	}

	[[nodiscard]] bool TryRetainStrong() noexcept
	{
		uint32 References = this->StrongReferences.load(std::memory_order_acquire);
		while (References != 0)
		{
			if (this->StrongReferences.compare_exchange_weak(References, References + 1, std::memory_order_acq_rel,
															 std::memory_order_acquire))
			{
				return true;
			}
		}
		return false;
	}

	void ReleaseStrong() noexcept
	{
		if (this->StrongReferences.fetch_sub(1, std::memory_order_acq_rel) != 1)
		{
			return;
		}
		{
			std::unique_lock Lock(this->PublicationMutex);
			this->Asset.Reset();
		}
		this->ReleaseWeak();
	}

	void RetainWeak() noexcept
	{
		this->WeakReferences.fetch_add(1, std::memory_order_relaxed);
	}

	void ReleaseWeak() noexcept
	{
		if (this->WeakReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			delete this;
		}
	}

	friend class AssetManager;
	template <IsAsset T> friend class AssetHandle;
	template <IsAsset T> friend class WeakAssetHandle;
};
} // namespace resource
