#include "AssetThumbnailService.h"

#include "Source/resource/asset/MaterialAsset.h"
#include "Source/resource/asset/Texture2DAsset.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace editor::asset
{
namespace
{
void HashCombine(usize &Seed, const usize Value) noexcept
{
	Seed ^= Value + static_cast<usize>(0x9e3779b97f4a7c15ULL) + (Seed << 6U) + (Seed >> 2U);
}

struct ThumbnailColor final
{
	uint8 Red = 0;
	uint8 Green = 0;
	uint8 Blue = 0;
	uint8 Alpha = 255;
};

[[nodiscard]] uint8 ToByte(const float32 Value) noexcept
{
	return static_cast<uint8>(std::clamp(Value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void WritePixel(AssetThumbnailImage &Image, const uint32 X, const uint32 Y, const ThumbnailColor Color)
{
	const usize Index = (static_cast<usize>(Y) * Image.Width + X) * 4U;
	Image.Pixels[Index] = Color.Red;
	Image.Pixels[Index + 1U] = Color.Green;
	Image.Pixels[Index + 2U] = Color.Blue;
	Image.Pixels[Index + 3U] = Color.Alpha;
}

[[nodiscard]] ThumbnailColor ReadTexturePixel(const resource::Texture2DAsset &Texture, const uint32 X, const uint32 Y)
{
	const uint32 Channels = static_cast<uint32>(Texture.GetChannels());
	const usize Index = (static_cast<usize>(Y) * static_cast<uint32>(Texture.GetWidth()) + X) * Channels;
	const std::span<const uint8> Pixels = Texture.GetPixels();
	if (Index + Channels > Pixels.size())
		throw std::runtime_error("texture pixel storage is smaller than its declared dimensions");
	if (Channels == 1U)
		return {.Red = Pixels[Index], .Green = Pixels[Index], .Blue = Pixels[Index]};
	if (Channels == 2U)
		return {.Red = Pixels[Index], .Green = Pixels[Index], .Blue = Pixels[Index], .Alpha = Pixels[Index + 1U]};
	return {.Red = Pixels[Index],
			.Green = Pixels[Index + 1U],
			.Blue = Pixels[Index + 2U],
			.Alpha = Channels == 4U ? Pixels[Index + 3U] : static_cast<uint8>(255)};
}

[[nodiscard]] AssetThumbnailImage GenerateTextureThumbnail(const resource::Texture2DAsset &Texture, const AssetThumbnailRequest &Request)
{
	if (Texture.GetWidth() <= 0 || Texture.GetHeight() <= 0 || Texture.GetChannels() <= 0 || Texture.GetChannels() > 4 ||
		Texture.GetPixels().empty())
	{
		throw std::runtime_error("texture CPU data cannot produce a thumbnail");
	}
	AssetThumbnailImage Image{.ID = Request.ID,
							  .SourceHash = Request.SourceHash,
							  .Width = Request.Width,
							  .Height = Request.Height,
							  .Pixels = std::vector<uint8>(static_cast<usize>(Request.Width) * Request.Height * 4U),
							  .Type = Request.Type,
							  .ColorSpace = Request.ColorSpace,
							  .RenderVariant = Request.RenderVariant};
	const float64 Scale =
		std::min(static_cast<float64>(Request.Width) / Texture.GetWidth(), static_cast<float64>(Request.Height) / Texture.GetHeight());
	const uint32 DrawWidth = std::max<uint32>(1U, static_cast<uint32>(std::floor(Texture.GetWidth() * Scale)));
	const uint32 DrawHeight = std::max<uint32>(1U, static_cast<uint32>(std::floor(Texture.GetHeight() * Scale)));
	const uint32 OffsetX = (Request.Width - DrawWidth) / 2U;
	const uint32 OffsetY = (Request.Height - DrawHeight) / 2U;
	for (uint32 Y = 0; Y < Request.Height; ++Y)
	{
		for (uint32 X = 0; X < Request.Width; ++X)
		{
			const uint8 Background = ((X / 8U + Y / 8U) % 2U) == 0U ? 38U : 52U;
			ThumbnailColor Color{.Red = Background, .Green = Background, .Blue = Background};
			if (X >= OffsetX && X < OffsetX + DrawWidth && Y >= OffsetY && Y < OffsetY + DrawHeight)
			{
				const uint32 SourceX = std::min<uint32>(static_cast<uint32>(Texture.GetWidth()) - 1U,
														((X - OffsetX) * static_cast<uint32>(Texture.GetWidth())) / DrawWidth);
				const uint32 SourceY = std::min<uint32>(static_cast<uint32>(Texture.GetHeight()) - 1U,
														((Y - OffsetY) * static_cast<uint32>(Texture.GetHeight())) / DrawHeight);
				const ThumbnailColor Source = ReadTexturePixel(Texture, SourceX, SourceY);
				const uint32 Alpha = Source.Alpha;
				Color.Red = static_cast<uint8>((static_cast<uint32>(Source.Red) * Alpha + Background * (255U - Alpha)) / 255U);
				Color.Green = static_cast<uint8>((static_cast<uint32>(Source.Green) * Alpha + Background * (255U - Alpha)) / 255U);
				Color.Blue = static_cast<uint8>((static_cast<uint32>(Source.Blue) * Alpha + Background * (255U - Alpha)) / 255U);
			}
			WritePixel(Image, X, Y, Color);
		}
	}
	return Image;
}

[[nodiscard]] AssetThumbnailImage GenerateMaterialThumbnail(const resource::PBRMaterialFactors &Factors,
															const AssetThumbnailRequest &Request)
{
	AssetThumbnailImage Image{.ID = Request.ID,
							  .SourceHash = Request.SourceHash,
							  .Width = Request.Width,
							  .Height = Request.Height,
							  .Pixels = std::vector<uint8>(static_cast<usize>(Request.Width) * Request.Height * 4U),
							  .Type = Request.Type,
							  .ColorSpace = Request.ColorSpace,
							  .RenderVariant = Request.RenderVariant};
	const float32 Radius = static_cast<float32>(std::min(Request.Width, Request.Height)) * 0.38f;
	const float32 CenterX = static_cast<float32>(Request.Width) * 0.5f;
	const float32 CenterY = static_cast<float32>(Request.Height) * 0.5f;
	const float32 Roughness = std::clamp(Factors.Roughness, 0.0f, 1.0f);
	const float32 Metallic = std::clamp(Factors.Metallic, 0.0f, 1.0f);
	for (uint32 Y = 0; Y < Request.Height; ++Y)
	{
		for (uint32 X = 0; X < Request.Width; ++X)
		{
			const float32 NormalX = (static_cast<float32>(X) + 0.5f - CenterX) / Radius;
			const float32 NormalY = (CenterY - static_cast<float32>(Y) - 0.5f) / Radius;
			const float32 RadiusSquared = NormalX * NormalX + NormalY * NormalY;
			ThumbnailColor Color{.Red = 24U, .Green = 27U, .Blue = 34U};
			if (RadiusSquared <= 1.0f)
			{
				const float32 NormalZ = std::sqrt(1.0f - RadiusSquared);
				const float32 Diffuse = std::max(0.0f, NormalX * -0.35f + NormalY * 0.55f + NormalZ * 0.76f);
				const float32 HighlightBase = std::max(0.0f, NormalX * -0.28f + NormalY * 0.47f + NormalZ * 0.84f);
				const float32 Highlight = std::pow(HighlightBase, 4.0f + (1.0f - Roughness) * 92.0f);
				const float32 Ambient = 0.12f + NormalZ * 0.08f;
				const float32 SpecularStrength = (0.04f + Metallic * 0.96f) * Highlight;
				Color = {.Red = ToByte(Factors.BaseColor.r * (Ambient + Diffuse * 0.82f) + SpecularStrength + Factors.Emissive.r),
						 .Green = ToByte(Factors.BaseColor.g * (Ambient + Diffuse * 0.82f) + SpecularStrength + Factors.Emissive.g),
						 .Blue = ToByte(Factors.BaseColor.b * (Ambient + Diffuse * 0.82f) + SpecularStrength + Factors.Emissive.b)};
			}
			WritePixel(Image, X, Y, Color);
		}
	}
	return Image;
}

[[nodiscard]] std::array<uint8, 3> TypeColor(const resource::AssetType Type) noexcept
{
	constexpr std::array<std::array<uint8, 3>, static_cast<usize>(resource::AssetType::Count)> Colors{{
		{70U, 146U, 214U},
		{180U, 106U, 203U},
		{206U, 126U, 185U},
		{74U, 176U, 126U},
		{67U, 163U, 147U},
		{73U, 170U, 145U},
		{224U, 161U, 66U},
		{223U, 144U, 67U},
		{218U, 126U, 66U},
		{208U, 112U, 82U},
		{103U, 153U, 224U},
	}};
	const usize Index = static_cast<usize>(Type);
	return Index < Colors.size() ? Colors[Index] : std::array<uint8, 3>{128U, 128U, 128U};
}

[[nodiscard]] AssetThumbnailImage GenerateTypeThumbnail(const AssetThumbnailRequest &Request)
{
	AssetThumbnailImage Image{.ID = Request.ID,
							  .SourceHash = Request.SourceHash,
							  .Width = Request.Width,
							  .Height = Request.Height,
							  .Pixels = std::vector<uint8>(static_cast<usize>(Request.Width) * Request.Height * 4U),
							  .Type = Request.Type,
							  .ColorSpace = Request.ColorSpace,
							  .RenderVariant = Request.RenderVariant};
	const std::array<uint8, 3> Accent = TypeColor(Request.Type);
	const uint32 Margin = std::max<uint32>(8U, std::min(Request.Width, Request.Height) / 7U);
	for (uint32 Y = 0; Y < Request.Height; ++Y)
	{
		for (uint32 X = 0; X < Request.Width; ++X)
		{
			ThumbnailColor Color{.Red = 24U, .Green = 27U, .Blue = 34U};
			const bool Inside = X >= Margin && X + Margin < Request.Width && Y >= Margin && Y + Margin < Request.Height;
			if (Inside)
			{
				const bool Border =
					X < Margin + 3U || X + Margin + 3U >= Request.Width || Y < Margin + 3U || Y + Margin + 3U >= Request.Height;
				const float32 Shade = Border ? 0.92f : 0.34f + static_cast<float32>(Request.Height - Y) / Request.Height * 0.18f;
				Color = {.Red = ToByte(static_cast<float32>(Accent[0]) / 255.0f * Shade),
						 .Green = ToByte(static_cast<float32>(Accent[1]) / 255.0f * Shade),
						 .Blue = ToByte(static_cast<float32>(Accent[2]) / 255.0f * Shade)};
			}
			WritePixel(Image, X, Y, Color);
		}
	}
	return Image;
}
} // namespace

usize AssetThumbnailKeyHash::operator()(const AssetThumbnailKey &Key) const noexcept
{
	usize Result = std::hash<string>{}(Key.ID);
	HashCombine(Result, std::hash<uint8>{}(static_cast<uint8>(Key.Type)));
	HashCombine(Result, std::hash<string>{}(Key.SourceHash));
	HashCombine(Result, std::hash<uint32>{}(Key.Width));
	HashCombine(Result, std::hash<uint32>{}(Key.Height));
	HashCombine(Result, std::hash<uint8>{}(static_cast<uint8>(Key.ColorSpace)));
	HashCombine(Result, std::hash<uint32>{}(Key.RenderVariant));
	return Result;
}

bool AssetThumbnailImage::IsValid() const noexcept
{
	if (this->Width == 0 || this->Height == 0 || this->Width > std::numeric_limits<usize>::max() / this->Height)
		return false;
	const usize PixelCount = static_cast<usize>(this->Width) * this->Height;
	return PixelCount <= std::numeric_limits<usize>::max() / 4U && this->Pixels.size() == PixelCount * 4U;
}

AssetThumbnailService::AssetThumbnailService(resource::AssetManager &Assets) noexcept
	: Assets(&Assets), OwnerThread(std::this_thread::get_id())
{
}

AssetThumbnailService::~AssetThumbnailService()
{
	this->Wait();
}

void AssetThumbnailService::Request(AssetThumbnailRequest Request)
{
	this->RequireOwnerThread();
	if (Request.ID.empty() || Request.Type == resource::AssetType::Count)
		throw std::invalid_argument("thumbnail request requires a registered asset identity and concrete type");
	if (Request.Width == 0 || Request.Height == 0 || Request.Width > 512U || Request.Height > 512U)
		throw std::invalid_argument("thumbnail dimensions must be between 1 and 512 pixels");
	const AssetThumbnailKey Key = MakeKey(Request);
	if (const auto Cached = this->Images.find(Key); Cached != this->Images.end())
	{
		if (Cached->second.Image->IsValid() || std::chrono::steady_clock::now() < Cached->second.RetryAfter)
			return;
		this->CachedBytes -= Cached->second.Bytes;
		this->Images.erase(Cached);
	}
	if (std::ranges::any_of(this->Queued, [&](const AssetThumbnailRequest &QueuedRequest) { return MakeKey(QueuedRequest) == Key; }) ||
		std::ranges::any_of(this->Pending, [&](const PendingThumbnail &Thumbnail) { return MakeKey(Thumbnail.Request) == Key; }))
		return;
	const auto SameVariant = [&Request](const AssetThumbnailRequest &Candidate)
	{
		return Candidate.ID == Request.ID && Candidate.Type == Request.Type && Candidate.Width == Request.Width &&
			   Candidate.Height == Request.Height && Candidate.ColorSpace == Request.ColorSpace &&
			   Candidate.RenderVariant == Request.RenderVariant;
	};
	std::erase_if(this->Queued, SameVariant);
	for (PendingThumbnail &Thumbnail : this->Pending)
	{
		if (SameVariant(Thumbnail.Request))
			Thumbnail.Cancelled->store(true, std::memory_order_release);
	}
	for (auto Iterator = this->Images.begin(); Iterator != this->Images.end();)
	{
		if (Iterator->first.ID == Request.ID && Iterator->first.Type == Request.Type && Iterator->first.Width == Request.Width &&
			Iterator->first.Height == Request.Height && Iterator->first.ColorSpace == Request.ColorSpace &&
			Iterator->first.RenderVariant == Request.RenderVariant)
		{
			this->CachedBytes -= Iterator->second.Bytes;
			Iterator = this->Images.erase(Iterator);
		}
		else
			++Iterator;
	}
	const auto Position = std::ranges::find_if(this->Queued, [&Request](const AssetThumbnailRequest &QueuedRequest)
											   { return QueuedRequest.Priority > Request.Priority; });
	this->Queued.insert(Position, std::move(Request));
}

void AssetThumbnailService::Tick(core::threading::TaskScheduler &Scheduler)
{
	this->RequireOwnerThread();
	this->CollectCompleted();
	while (this->Pending.size() < MaximumConcurrentTasks && !this->Queued.empty())
	{
		AssetThumbnailRequest Request = std::move(this->Queued.front());
		this->Queued.pop_front();
		try
		{
			AssetThumbnailRequest TaskRequest = Request;
			auto Cancelled = std::make_shared<std::atomic_bool>(false);
			std::future<AssetThumbnailImage> Future =
				Scheduler.Submit([Assets = this->Assets, Request = std::move(TaskRequest), Cancelled]() mutable
								 { return AssetThumbnailService::Generate(*Assets, std::move(Request), Cancelled); },
								 core::threading::TaskPriority::Background);
			this->Pending.push_back({.Request = std::move(Request), .Cancelled = std::move(Cancelled), .Future = std::move(Future)});
		}
		catch (...)
		{
			this->Queued.push_front(std::move(Request));
			throw;
		}
	}
}

void AssetThumbnailService::Wait() noexcept
{
	if (std::this_thread::get_id() != this->OwnerThread)
		std::terminate();
	this->Queued.clear();
	for (PendingThumbnail &Thumbnail : this->Pending)
	{
		Thumbnail.Cancelled->store(true, std::memory_order_release);
		try
		{
			Thumbnail.Future.wait();
			(void)Thumbnail.Future.get();
		}
		catch (...)
		{
		}
	}
	this->Pending.clear();
}

void AssetThumbnailService::Clear()
{
	this->RequireOwnerThread();
	this->Queued.clear();
	for (PendingThumbnail &Thumbnail : this->Pending)
		Thumbnail.Cancelled->store(true, std::memory_order_release);
	this->Images.clear();
	this->CachedBytes = 0;
}

std::shared_ptr<const AssetThumbnailImage> AssetThumbnailService::Find(const AssetThumbnailRequest &Request)
{
	this->RequireOwnerThread();
	const auto Found = this->Images.find(MakeKey(Request));
	if (Found == this->Images.end())
		return {};
	Found->second.LastUseSerial = ++this->UseSerial;
	return Found->second.Image;
}

usize AssetThumbnailService::GetPendingCount() const
{
	this->RequireOwnerThread();
	return this->Queued.size() + this->Pending.size();
}

AssetThumbnailKey AssetThumbnailService::MakeKey(const AssetThumbnailRequest &Request)
{
	return {.ID = Request.ID,
			.Type = Request.Type,
			.SourceHash = Request.SourceHash,
			.Width = Request.Width,
			.Height = Request.Height,
			.ColorSpace = Request.ColorSpace,
			.RenderVariant = Request.RenderVariant};
}

void AssetThumbnailService::RequireOwnerThread() const
{
	if (std::this_thread::get_id() != this->OwnerThread)
		throw std::logic_error("Asset thumbnail service must be accessed from its owner thread");
}

AssetThumbnailImage AssetThumbnailService::Generate(resource::AssetManager &Assets, AssetThumbnailRequest Request,
													const std::shared_ptr<std::atomic_bool> &Cancelled)
{
	try
	{
		if (Cancelled->load(std::memory_order_acquire))
			throw std::runtime_error("thumbnail generation was superseded");
		switch (Request.Type)
		{
		case resource::AssetType::Texture2D:
		{
			resource::AssetHandle<resource::Texture2DAsset> Texture = Assets.GetAssetByID<resource::Texture2DAsset>(Request.ID);
			resource::AssetPtr<resource::Texture2DAsset> PinnedTexture = Texture.Pin();
			return GenerateTextureThumbnail(*PinnedTexture, Request);
		}
		case resource::AssetType::Material:
		{
			resource::AssetHandle<resource::MaterialAsset> Material = Assets.GetAssetByID<resource::MaterialAsset>(Request.ID);
			resource::AssetPtr<resource::MaterialAsset> PinnedMaterial = Material.Pin();
			return GenerateMaterialThumbnail(PinnedMaterial->GetFactors(), Request);
		}
		case resource::AssetType::MaterialInstance:
		{
			resource::AssetHandle<resource::MaterialInstanceAsset> Material =
				Assets.GetAssetByID<resource::MaterialInstanceAsset>(Request.ID);
			resource::AssetPtr<resource::MaterialInstanceAsset> PinnedMaterial = Material.Pin();
			return GenerateMaterialThumbnail(PinnedMaterial->GetFactors(), Request);
		}
		default:
			return GenerateTypeThumbnail(Request);
		}
	}
	catch (const std::exception &Exception)
	{
		AssetThumbnailImage Failure{.ID = std::move(Request.ID),
									.SourceHash = std::move(Request.SourceHash),
									.Width = Request.Width,
									.Height = Request.Height,
									.Diagnostic = Exception.what(),
									.Type = Request.Type,
									.ColorSpace = Request.ColorSpace,
									.RenderVariant = Request.RenderVariant};
		return Failure;
	}
}

void AssetThumbnailService::CollectCompleted()
{
	for (usize Index = 0; Index < this->Pending.size();)
	{
		PendingThumbnail &Thumbnail = this->Pending[Index];
		if (Thumbnail.Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++Index;
			continue;
		}
		AssetThumbnailImage Image;
		try
		{
			Image = Thumbnail.Future.get();
		}
		catch (const std::exception &Exception)
		{
			Image = {.ID = Thumbnail.Request.ID,
					 .SourceHash = Thumbnail.Request.SourceHash,
					 .Width = Thumbnail.Request.Width,
					 .Height = Thumbnail.Request.Height,
					 .Diagnostic = Exception.what(),
					 .Type = Thumbnail.Request.Type,
					 .ColorSpace = Thumbnail.Request.ColorSpace,
					 .RenderVariant = Thumbnail.Request.RenderVariant};
		}
		catch (...)
		{
			Image = {.ID = Thumbnail.Request.ID,
					 .SourceHash = Thumbnail.Request.SourceHash,
					 .Width = Thumbnail.Request.Width,
					 .Height = Thumbnail.Request.Height,
					 .Diagnostic = "thumbnail task failed with a non-standard exception",
					 .Type = Thumbnail.Request.Type,
					 .ColorSpace = Thumbnail.Request.ColorSpace,
					 .RenderVariant = Thumbnail.Request.RenderVariant};
		}
		if (!Thumbnail.Cancelled->load(std::memory_order_acquire))
			this->Cache(std::move(Image));
		this->Pending.erase(this->Pending.begin() + static_cast<std::ptrdiff_t>(Index));
	}
}

void AssetThumbnailService::Cache(AssetThumbnailImage Image)
{
	AssetThumbnailKey Key{.ID = Image.ID,
						  .Type = Image.Type,
						  .SourceHash = Image.SourceHash,
						  .Width = Image.Width,
						  .Height = Image.Height,
						  .ColorSpace = Image.ColorSpace,
						  .RenderVariant = Image.RenderVariant};
	const usize Bytes = Image.Pixels.size();
	if (const auto Existing = this->Images.find(Key); Existing != this->Images.end())
		this->CachedBytes -= Existing->second.Bytes;
	auto OwnedImage = std::make_shared<const AssetThumbnailImage>(std::move(Image));
	const std::chrono::steady_clock::time_point RetryAfter =
		OwnedImage->IsValid() ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now() + std::chrono::seconds(2);
	this->Images.insert_or_assign(
		std::move(Key),
		CachedImage{.Image = std::move(OwnedImage), .LastUseSerial = ++this->UseSerial, .Bytes = Bytes, .RetryAfter = RetryAfter});
	this->CachedBytes += Bytes;
	this->TrimCache();
}

void AssetThumbnailService::TrimCache()
{
	while ((this->Images.size() > MaximumCachedImages || this->CachedBytes > MaximumCachedBytes) && !this->Images.empty())
	{
		const auto Oldest = std::ranges::min_element(this->Images, {}, [](const auto &Entry) { return Entry.second.LastUseSerial; });
		this->CachedBytes -= Oldest->second.Bytes;
		this->Images.erase(Oldest);
	}
}
} // namespace editor::asset
