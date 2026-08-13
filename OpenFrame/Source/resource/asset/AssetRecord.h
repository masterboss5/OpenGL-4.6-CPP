#pragma once

#include "Source/resource/Asset.h"
#include "Source/resource/asset/AssetException.h"
#include "Source/resource/asset/AssetTypes.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
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
class AssetLoadTransaction;
class GeneratedAssetStage;
class AssetRecordHandle;
template <IsAsset T> class AssetHandle;
template <IsAsset T> class WeakAssetHandle;

class ENGINE_API AssetRecord final
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
	AssetPtr<resource::Asset> Asset;
	AssetPtr<resource::Asset> PendingGPUAsset;
	std::vector<AssetID> PendingDependencies;
	std::unordered_map<AssetID, std::filesystem::file_time_type> PendingDependencyWriteTimes;
	mutable std::shared_mutex PublicationMutex;
	mutable std::condition_variable_any PublicationChanged;
	std::exception_ptr LoadException;
	uint64 ActiveLoadOperation = 0;
	bool IsImportRoot = false;
	bool HasReadyGeneration = false;
	bool GPURealizationQueued = false;
	std::atomic<uint64> PublishedGeneration{0};
	std::atomic<uint32> StrongReferences{1};
	std::atomic<uint32> WeakReferences{1};
	std::atomic<bool> AcceptingStrongReferences{true};

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
		if (!this->AcceptingStrongReferences.load(std::memory_order_acquire))
			return false;
		uint32 References = this->StrongReferences.load(std::memory_order_acquire);
		while (References != 0)
		{
			if (this->StrongReferences.compare_exchange_weak(References, References + 1, std::memory_order_acq_rel,
															 std::memory_order_acquire))
			{
				if (!this->AcceptingStrongReferences.load(std::memory_order_acquire))
				{
					this->ReleaseStrong();
					return false;
				}
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
		this->AcceptingStrongReferences.store(false, std::memory_order_release);
		{
			std::unique_lock Lock(this->PublicationMutex);
			this->PendingGPUAsset.Reset();
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
	friend class AssetLoadTransaction;
	friend class GeneratedAssetStage;
	friend class AssetRecordHandle;
	template <IsAsset T> friend class AssetHandle;
	template <IsAsset T> friend class WeakAssetHandle;
};

class ENGINE_API AssetRecordHandle final
{
  public:
	AssetRecordHandle() = default;
	AssetRecordHandle(std::nullptr_t) noexcept
	{
	}

	AssetRecordHandle(const AssetRecordHandle &Other) noexcept : Record(Other.Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainStrong();
	}

	AssetRecordHandle(AssetRecordHandle &&Other) noexcept : Record(std::exchange(Other.Record, nullptr))
	{
	}

	~AssetRecordHandle()
	{
		this->Reset();
	}

	AssetRecordHandle &operator=(const AssetRecordHandle &Other) noexcept
	{
		if (this == &Other)
			return *this;
		AssetRecord *Replacement = Other.Record;
		if (Replacement != nullptr)
			Replacement->RetainStrong();
		this->Reset();
		this->Record = Replacement;
		return *this;
	}

	AssetRecordHandle &operator=(AssetRecordHandle &&Other) noexcept
	{
		if (this == &Other)
			return *this;
		this->Reset();
		this->Record = std::exchange(Other.Record, nullptr);
		return *this;
	}

	[[nodiscard]] const AssetRecord *Get() const noexcept
	{
		return this->Record;
	}

	[[nodiscard]] const AssetRecord &operator*() const
	{
		if (this->Record == nullptr)
			throw std::logic_error("Cannot dereference an empty asset record handle");
		return *this->Record;
	}

	[[nodiscard]] const AssetRecord *operator->() const
	{
		return &**this;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->Record != nullptr;
	}

	[[nodiscard]] bool operator==(std::nullptr_t) const noexcept
	{
		return this->Record == nullptr;
	}

	void Reset() noexcept
	{
		if (this->Record != nullptr)
			std::exchange(this->Record, nullptr)->ReleaseStrong();
	}

  private:
	struct AdoptStrongReference final
	{
	};

	AssetRecord *Record = nullptr;

	explicit AssetRecordHandle(AssetRecord *Record) noexcept : Record(Record)
	{
		if (this->Record != nullptr)
			this->Record->RetainStrong();
	}

	AssetRecordHandle(AssetRecord *Record, AdoptStrongReference) noexcept : Record(Record)
	{
	}

	friend class AssetManager;
	friend class AssetLoadTransaction;
	friend class GeneratedAssetStage;
};
} // namespace resource
