#pragma once

#include "src/core/threading/TaskScheduler.h"
#include "src/resource/asset/AssetManager.h"
#include "src/types.h"

#include <deque>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace editor::asset
{
enum class AssetThumbnailColorSpace : uint8
{
	Linear,
	SRGB
};

enum class AssetThumbnailPriority : uint8
{
	Visible,
	Near,
	Prefetch
};

enum class AssetThumbnailGPUState : uint8
{
	CPUReady,
	UploadPending,
	Ready,
	RetirementPending,
	Retired,
	Failed
};

struct AssetThumbnailRequest final
{
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Count;
	string SourceHash;
	uint32 Width = 96;
	uint32 Height = 96;
	AssetThumbnailColorSpace ColorSpace = AssetThumbnailColorSpace::SRGB;
	uint32 RenderVariant = 0;
	AssetThumbnailPriority Priority = AssetThumbnailPriority::Visible;
};

struct AssetThumbnailKey final
{
	resource::AssetID ID;
	resource::AssetType Type = resource::AssetType::Count;
	string SourceHash;
	uint32 Width = 0;
	uint32 Height = 0;
	AssetThumbnailColorSpace ColorSpace = AssetThumbnailColorSpace::SRGB;
	uint32 RenderVariant = 0;

	[[nodiscard]] bool operator==(const AssetThumbnailKey &) const noexcept = default;
};

struct AssetThumbnailKeyHash final
{
	[[nodiscard]] usize operator()(const AssetThumbnailKey &Key) const noexcept;
};

struct AssetThumbnailImage final
{
	resource::AssetID ID;
	string SourceHash;
	uint32 Width = 0;
	uint32 Height = 0;
	std::vector<uint8> Pixels;
	string Diagnostic;
	resource::AssetType Type = resource::AssetType::Count;
	AssetThumbnailColorSpace ColorSpace = AssetThumbnailColorSpace::SRGB;
	uint32 RenderVariant = 0;

	[[nodiscard]] bool IsValid() const noexcept;
};

struct AssetThumbnailGPUHandle final
{
	AssetThumbnailKey Key;
	uint64 Generation = 0;
	AssetThumbnailGPUState State = AssetThumbnailGPUState::CPUReady;

	[[nodiscard]] bool IsReady() const noexcept
	{
		return this->State == AssetThumbnailGPUState::Ready;
	}
};

class AssetThumbnailService final
{
  public:
	explicit AssetThumbnailService(resource::AssetManager &Assets) noexcept;
	~AssetThumbnailService();

	AssetThumbnailService(const AssetThumbnailService &) = delete;
	AssetThumbnailService &operator=(const AssetThumbnailService &) = delete;
	AssetThumbnailService(AssetThumbnailService &&) = delete;
	AssetThumbnailService &operator=(AssetThumbnailService &&) = delete;

	void Request(AssetThumbnailRequest Request);
	void Tick(core::threading::TaskScheduler &Scheduler);
	void Wait() noexcept;
	void Clear();

	[[nodiscard]] std::shared_ptr<const AssetThumbnailImage> Find(const AssetThumbnailRequest &Request);
	[[nodiscard]] usize GetPendingCount() const;
	[[nodiscard]] static AssetThumbnailKey MakeKey(const AssetThumbnailRequest &Request);

  private:
	struct PendingThumbnail final
	{
		AssetThumbnailRequest Request;
		std::shared_ptr<std::atomic_bool> Cancelled;
		std::future<AssetThumbnailImage> Future;
	};
	struct CachedImage final
	{
		std::shared_ptr<const AssetThumbnailImage> Image;
		uint64 LastUseSerial = 0;
		usize Bytes = 0;
		std::chrono::steady_clock::time_point RetryAfter{};
	};

	[[nodiscard]] static AssetThumbnailImage Generate(resource::AssetManager &Assets, AssetThumbnailRequest Request,
													  const std::shared_ptr<std::atomic_bool> &Cancelled);
	void RequireOwnerThread() const;
	void CollectCompleted();
	void Cache(AssetThumbnailImage Image);
	void TrimCache();

	static constexpr usize MaximumConcurrentTasks = 2;
	static constexpr usize MaximumCachedImages = 512;
	static constexpr usize MaximumCachedBytes = 128U * 1024U * 1024U;
	resource::AssetManager *Assets = nullptr;
	std::deque<AssetThumbnailRequest> Queued;
	std::vector<PendingThumbnail> Pending;
	std::unordered_map<AssetThumbnailKey, CachedImage, AssetThumbnailKeyHash> Images;
	uint64 UseSerial = 0;
	usize CachedBytes = 0;
	std::thread::id OwnerThread;
};
} // namespace editor::asset
