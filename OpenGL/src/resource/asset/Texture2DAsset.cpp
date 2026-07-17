#include "Texture2DAsset.h"

#include "src/pipeline/device/OpenGLRuntime.h"

resource::Texture2DAsset::Texture2DAsset(std::string name, int32 width, int32 height, int32 channels, std::vector<uint8> pixels)
	: Asset(util::UUID::generateRandomUUID()),
	name(std::move(name)),
	width(width),
	height(height),
	channels(channels),
	pixels(std::move(pixels))
{
}

bool resource::Texture2DAsset::requiresGpuRealization() const noexcept
{
	return true;
}

resource::AssetGpuRealizationResult resource::Texture2DAsset::realizeGpu()
{
	if (this->gpuTexture != nullptr)
	{
		return {};
	}
	if (this->width <= 0 || this->height <= 0 || this->pixels.empty())
	{
		return { .succeeded = false, .error = "Texture CPU data is invalid" };
	}

	try
	{
		pipeline::device::requireOpenGL46Context();
		renderer::texture::Texture2DSpecification specification = renderer::texture::Texture2DSpecification::DEFAULT_INSTANCE;
		renderer::texture::Texture2DCreationInfo creationInfo {
			.pixels = this->pixels.data(),
			.width = this->width,
			.height = this->height,
			.channels = this->channels,
			.specification = specification
		};
		this->gpuTexture = std::make_unique<renderer::texture::Texture2D>(this->name, creationInfo);
		return {};
	}
	catch (const std::exception& exception)
	{
		return { .succeeded = false, .error = exception.what() };
	}
}

int32 resource::Texture2DAsset::getWidth() const noexcept { return this->width; }
int32 resource::Texture2DAsset::getHeight() const noexcept { return this->height; }
int32 resource::Texture2DAsset::getChannels() const noexcept { return this->channels; }
const renderer::texture::Texture2D* resource::Texture2DAsset::getGpuTexture() const noexcept { return this->gpuTexture.get(); }
