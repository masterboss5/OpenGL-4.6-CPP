#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <glm.hpp>

#include "src/renderer/Vertex.h"
#include "src/resource/Asset.h"

namespace resource
{
	struct ModelMaterial final
	{
		std::string name;
		std::filesystem::path baseColorTexture;
		std::filesystem::path normalTexture;
		std::filesystem::path metallicRoughnessTexture;
		std::filesystem::path occlusionTexture;
		std::filesystem::path emissiveTexture;
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
	};

	struct ModelSubmesh final
	{
		std::string name;
		uint32 materialIndex = 0;
		std::vector<renderer::Vertex> vertices;
		std::vector<uint32> indices;
	};

	class ModelAsset final : public Asset
	{
	public:
		ModelAsset(std::string name, std::vector<ModelSubmesh> submeshes, std::vector<ModelMaterial> materials);

		[[nodiscard]] std::string_view getName() const noexcept;
		[[nodiscard]] const std::vector<ModelSubmesh>& getSubmeshes() const noexcept;
		[[nodiscard]] const std::vector<ModelMaterial>& getMaterials() const noexcept;

	private:
		std::string name;
		std::vector<ModelSubmesh> submeshes;
		std::vector<ModelMaterial> materials;
	};
}
