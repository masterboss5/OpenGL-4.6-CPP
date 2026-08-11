#include "Texture2DAsset.h"

#include "src/pipeline/device/Device.h"

resource::Texture2DAsset::Texture2DAsset(std::string Name, int32 Width, int32 Height, int32 Channels, std::vector<uint8> Pixels)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), Width(Width), Height(Height), Channels(Channels),
	  Pixels(std::move(Pixels))
{
}

bool resource::Texture2DAsset::RequiresGPURealization() const noexcept
{
	return true;
}

resource::AssetGPURealizationResult resource::Texture2DAsset::RealizeGPU(pipeline::device::Device &Device)
{
	if (this->GPUTexture != nullptr)
	{
		return {};
	}
	if (this->Width <= 0 || this->Height <= 0 || this->Pixels.empty())
	{
		return {.Succeeded = false, .Error = "Texture CPU data is invalid"};
	}

	try
	{
		(void)Device.RequireCurrentContext();
		pipeline::texture::Texture2DSpecification Specification = pipeline::texture::Texture2DSpecification::DefaultInstance;
		pipeline::texture::Texture2DCreationInfo CreationInfo{.Pixels = this->Pixels.data(),
															  .PixelBytes = this->Pixels.size(),
															  .Width = this->Width,
															  .Height = this->Height,
															  .Channels = this->Channels,
															  .Specification = Specification};
		this->GPUTexture = std::make_unique<pipeline::texture::Texture2D>(Device, this->Name, CreationInfo);
		return {};
	}
	catch (const std::exception &Exception)
	{
		return {.Succeeded = false, .Error = Exception.what()};
	}
}

int32 resource::Texture2DAsset::GetWidth() const noexcept
{
	return this->Width;
}
int32 resource::Texture2DAsset::GetHeight() const noexcept
{
	return this->Height;
}
int32 resource::Texture2DAsset::GetChannels() const noexcept
{
	return this->Channels;
}
std::span<const uint8> resource::Texture2DAsset::GetPixels() const noexcept
{
	return this->Pixels;
}
const pipeline::texture::Texture2D *resource::Texture2DAsset::GetGPUTexture() const noexcept
{
	return this->GPUTexture.get();
}
