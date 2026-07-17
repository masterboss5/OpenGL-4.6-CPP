#include "ModelAssetImporter.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "src/resource/asset/ModelAsset.h"

namespace resource::importer
{
	namespace
	{
		void appendTextureDependency(
			const aiMaterial* material,
			aiTextureType textureType,
			const std::filesystem::path& modelPath,
			std::filesystem::path& destination,
			std::vector<AssetDependency>& dependencies)
		{
			if (material == nullptr)
			{
				throw AssetContentValidationException(AssetType::MODEL, modelPath, "Model contains a null material");
			}

			aiString texturePath;
			if (material->GetTexture(textureType, 0, &texturePath) != AI_SUCCESS)
			{
				return;
			}

			destination = (modelPath.parent_path() / texturePath.C_Str()).lexically_normal();
			dependencies.push_back({ .type = AssetType::TEXTURE_2D, .path = destination });
		}

		ModelMaterial importMaterial(const aiMaterial* material, const std::filesystem::path& modelPath, std::vector<AssetDependency>& dependencies)
		{
			if (material == nullptr)
			{
				throw AssetContentValidationException(AssetType::MODEL, modelPath, "Model contains a null material");
			}

			ModelMaterial result;
			aiString name;
			if (material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
			{
				result.name = name.C_Str();
			}

			appendTextureDependency(material, aiTextureType_DIFFUSE, modelPath, result.baseColorTexture, dependencies);
			appendTextureDependency(material, aiTextureType_NORMALS, modelPath, result.normalTexture, dependencies);
			appendTextureDependency(material, aiTextureType_METALNESS, modelPath, result.metallicRoughnessTexture, dependencies);
			appendTextureDependency(material, aiTextureType_AMBIENT_OCCLUSION, modelPath, result.occlusionTexture, dependencies);
			appendTextureDependency(material, aiTextureType_EMISSIVE, modelPath, result.emissiveTexture, dependencies);
			return result;
		}

		void appendNodeMeshes(
			const aiScene* scene,
			const aiNode* node,
			uint32 materialCount,
			const std::filesystem::path& modelPath,
			std::vector<ModelSubmesh>& submeshes)
		{
			if (scene == nullptr || node == nullptr || scene->mMeshes == nullptr)
			{
				throw AssetContentValidationException(AssetType::MODEL, modelPath, "Model scene contains an invalid node or mesh array");
			}

			for (uint32 meshIndex = 0; meshIndex < node->mNumMeshes; ++meshIndex)
			{
				const uint32 sourceMeshIndex = node->mMeshes[meshIndex];
				if (sourceMeshIndex >= scene->mNumMeshes || scene->mMeshes[sourceMeshIndex] == nullptr)
				{
					throw AssetContentValidationException(AssetType::MODEL, modelPath, "Model node references an invalid mesh");
				}

				const aiMesh* mesh = scene->mMeshes[sourceMeshIndex];
				if (!mesh->HasPositions())
				{
					continue;
				}
				if (mesh->mMaterialIndex >= materialCount)
				{
					throw AssetContentValidationException(AssetType::MODEL, modelPath, "Model mesh references an invalid material");
				}

				ModelSubmesh submesh;
				submesh.name = mesh->mName.C_Str();
				submesh.materialIndex = mesh->mMaterialIndex;
				submesh.vertices.reserve(mesh->mNumVertices);
				for (uint32 vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
				{
					renderer::Vertex vertex {};
					vertex.position = { mesh->mVertices[vertexIndex].x, mesh->mVertices[vertexIndex].y, mesh->mVertices[vertexIndex].z };
					vertex.normal = mesh->HasNormals() ? glm::vec3(mesh->mNormals[vertexIndex].x, mesh->mNormals[vertexIndex].y, mesh->mNormals[vertexIndex].z) : glm::vec3(0.0f, 1.0f, 0.0f);
					vertex.uv = mesh->HasTextureCoords(0) ? glm::vec2(mesh->mTextureCoords[0][vertexIndex].x, mesh->mTextureCoords[0][vertexIndex].y) : glm::vec2(0.0f);
					vertex.tanget = mesh->HasTangentsAndBitangents() ? glm::vec3(mesh->mTangents[vertexIndex].x, mesh->mTangents[vertexIndex].y, mesh->mTangents[vertexIndex].z) : glm::vec3(1.0f, 0.0f, 0.0f);
					vertex.bitTangent = mesh->HasTangentsAndBitangents() ? glm::vec3(mesh->mBitangents[vertexIndex].x, mesh->mBitangents[vertexIndex].y, mesh->mBitangents[vertexIndex].z) : glm::vec3(0.0f, 1.0f, 0.0f);
					submesh.vertices.push_back(vertex);
				}

				for (uint32 faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
				{
					const aiFace& face = mesh->mFaces[faceIndex];
					if (face.mNumIndices != 3)
					{
						continue;
					}
					submesh.indices.insert(submesh.indices.end(), face.mIndices, face.mIndices + face.mNumIndices);
				}
				if (!submesh.indices.empty())
				{
					submeshes.push_back(std::move(submesh));
				}
			}

			for (uint32 childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
			{
				appendNodeMeshes(scene, node->mChildren[childIndex], materialCount, modelPath, submeshes);
			}
		}
	}

	bool ModelAssetImporter::canImport(const std::filesystem::path& path) const
	{
		const std::string extension = getNormalizedExtension(path);
		return extension == ".gltf" || extension == ".glb" || extension == ".fbx" || extension == ".obj";
	}

	AssetType ModelAssetImporter::getAssetType() const noexcept
	{
		return AssetType::MODEL;
	}

	AssetImportResult ModelAssetImporter::importCpu(const std::filesystem::path& path) const
	{
		this->validateImportRequest(path);

		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(path.string(),
			aiProcess_Triangulate |
			aiProcess_FlipUVs |
			aiProcess_CalcTangentSpace |
			aiProcess_GenSmoothNormals |
			aiProcess_JoinIdenticalVertices |
			aiProcess_ImproveCacheLocality |
			aiProcess_OptimizeMeshes |
			aiProcess_OptimizeGraph);
		if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || scene->mRootNode == nullptr)
		{
			throw AssetModelParseException(AssetType::MODEL, path, importer.GetErrorString());
		}

		std::vector<ModelMaterial> materials;
		materials.reserve(scene->mNumMaterials);
		std::vector<AssetDependency> dependencies;
		if (scene->mNumMaterials != 0 && scene->mMaterials == nullptr)
		{
			throw AssetContentValidationException(AssetType::MODEL, path, "Model scene contains an invalid material array");
		}
		for (uint32 materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
		{
			materials.push_back(importMaterial(scene->mMaterials[materialIndex], path, dependencies));
		}
		if (materials.empty())
		{
			materials.emplace_back();
		}

		std::vector<ModelSubmesh> submeshes;
		appendNodeMeshes(scene, scene->mRootNode, static_cast<uint32>(materials.size()), path, submeshes);
		if (submeshes.empty())
		{
			throw AssetContentValidationException(AssetType::MODEL, path, "Model contains no renderable triangle meshes");
		}

		return AssetImportResult(AssetPtr<ModelAsset>::make(path.stem().string(), std::move(submeshes), std::move(materials)), std::move(dependencies));
	}
}
