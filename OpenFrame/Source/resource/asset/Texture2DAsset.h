#pragma once

#include "Source/core/EngineAPI.h"

#include "Source/pipeline/texture/Texture2D.h"
#include "Source/resource/Asset.h"
#include "Source/resource/asset/AssetTypes.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace resource
{
class ENGINE_API Texture2DAsset final : public Asset
{
  public:
	inline static constexpr resource::AssetType AssetType = resource::AssetType::Texture2D;

	Texture2DAsset(std::string Name, int32 Width, int32 Height, int32 Channels, std::vector<uint8> Pixels);

	[[nodiscard]] bool RequiresGPURealization() const noexcept override;
	[[nodiscard]] AssetGPURealizationResult RealizeGPU(pipeline::device::Device &Device) override;

	[[nodiscard]] int32 GetWidth() const noexcept;
	[[nodiscard]] int32 GetHeight() const noexcept;
	[[nodiscard]] int32 GetChannels() const noexcept;
	[[nodiscard]] std::span<const uint8> GetPixels() const noexcept;
	[[nodiscard]] const pipeline::texture::Texture2D *GetGPUTexture() const noexcept;

  private:
	std::string Name;
	int32 Width;
	int32 Height;
	int32 Channels;
	std::vector<uint8> Pixels;
	std::unique_ptr<pipeline::texture::Texture2D> GPUTexture;
};
} // namespace resource
