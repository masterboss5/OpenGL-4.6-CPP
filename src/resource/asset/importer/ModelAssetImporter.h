#pragma once
#include "src/resource/asset/importer/AssetImporter.h"
#include <iostream>
#include <vector>
#include "src/renderer/Vertex.h"

namespace resource::importer
{
	struct ReadPBRMaterialContext final
	{
		std::string name;
		std::string baseColorTexturePath;
		std::string normalTexturePath;
		std::string aoTexturePath;
		std::string diffuseRoughnessTexturePath;
		std::string metalnessTexturePath;
		glm::vec4 albedoFactor = glm::vec4(1.0f);
		float aoFactor = 1.0f;
		float roughnessFactor = 1.0f;
		float metallicFactor = 1.0f;
	};

	struct ReadMeshContext final
	{
		uint32_t faceCount;
		ReadPBRMaterialContext material;
		std::vector<renderer::Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct ReadModelContext final
	{
		std::string name;
		uint32_t vertexCount;
		uint32_t indexCount;
		std::vector<ReadMeshContext> submeshes;
		std::vector<ReadPBRMaterialContext> materials;
	};

	class ModelAssetImporter final : public resource::importer::AssetImporter
	{
	private:
		void processAssimpNode(const aiScene* scene, const aiNode* node, ReadModelContext& modelContext) const;
		ReadMeshContext processMesh(const aiScene* scene, const aiMesh* mesh) const;
	public:
		virtual bool canImport(const std::filesystem::path& path) override;
		virtual resource::AssetType getAssetType() const override;
		virtual Asset* import(const std::filesystem::path& path) override;
	};
}