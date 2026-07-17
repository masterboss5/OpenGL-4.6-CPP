#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/pipeline/texture/Texture2D.h"

namespace resource
{
	class Texture2DAsset final : public Asset
	{
	public:
		Texture2DAsset(std::string name, int32 width, int32 height, int32 channels, std::vector<uint8> pixels);

		[[nodiscard]] bool requiresGpuRealization() const noexcept override;
		[[nodiscard]] AssetGpuRealizationResult realizeGpu() override;

		[[nodiscard]] int32 getWidth() const noexcept;
		[[nodiscard]] int32 getHeight() const noexcept;
		[[nodiscard]] int32 getChannels() const noexcept;
		[[nodiscard]] const renderer::texture::Texture2D* getGpuTexture() const noexcept;

	private:
		std::string name;
		int32 width;
		int32 height;
		int32 channels;
		std::vector<uint8> pixels;
		std::unique_ptr<renderer::texture::Texture2D> gpuTexture;
	};
}
