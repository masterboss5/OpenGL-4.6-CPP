#pragma once

#include "src/pipeline/texture/Texture2D.h"
#include "src/resource/Asset.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace resource
{
class Texture2DAsset final : public Asset
{
  public:
	Texture2DAsset(std::string Name, int32 Width, int32 Height, int32 Channels, std::vector<uint8> Pixels);

	[[nodiscard]] bool RequiresGPURealization() const noexcept override;
	[[nodiscard]] AssetGPURealizationResult RealizeGPU(pipeline::device::Device &Device) override;

	[[nodiscard]] int32 GetWidth() const noexcept;
	[[nodiscard]] int32 GetHeight() const noexcept;
	[[nodiscard]] int32 GetChannels() const noexcept;
	[[nodiscard]] std::span<const uint8> GetPixels() const noexcept;
	[[nodiscard]] const renderer::texture::Texture2D *GetGPUTexture() const noexcept;

  private:
	std::string Name;
	int32 Width;
	int32 Height;
	int32 Channels;
	std::vector<uint8> Pixels;
	std::unique_ptr<renderer::texture::Texture2D> GPUTexture;
};
} // namespace resource
