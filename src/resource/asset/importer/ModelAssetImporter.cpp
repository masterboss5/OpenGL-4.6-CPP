#include "ModelAssetImporter.h"

void resource::importer::ModelAssetImporter::processAssimpNode(const aiScene* scene, const aiNode* node, ReadModelContext& modelContext) const
{
	for (uint32_t i = 0; i < node->mNumMeshes; i++)
	{
		const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		ReadMeshContext meshContext = this->processMesh(scene, mesh);
		modelContext.submeshes.push_back(meshContext);
	}

	for (uint32_t i = 0; i < node->mNumChildren; i++)
	{
		this->processAssimpNode(scene, node->mChildren[i], modelContext);
	}
}

resource::importer::ReadMeshContext resource::importer::ModelAssetImporter::processMesh(const aiScene* scene, const aiMesh* mesh) const
{
	ReadMeshContext meshContext {};
	ReadPBRMaterialContext& materialContext = meshContext.material;
	std::vector<renderer::Vertex> vertices;
	std::vector<uint32_t> indices;

	if (!mesh->HasPositions())
	{
		std::cerr << "Mesh does not have positions" << std::endl;
		return meshContext;
	}

	if (!mesh->HasNormals())
	{
		std::cerr << "Mesh does not have normals" << std::endl;
		return meshContext;
	}

	if (!mesh->HasTextureCoords(0))
	{
		std::cerr << "Mesh does not have texture coordinates" << std::endl;
		return meshContext;
	}

	if (!mesh->HasTangentsAndBitangents())
	{
		std::cerr << "Mesh does not have tangents and bitangents" << std::endl;
		return meshContext;
	}

	vertices.reserve(mesh->mNumVertices);
	for (uint32_t i = 0; i < mesh->mNumVertices; i++)
	{
		renderer::Vertex vertex {};
		vertex.position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
		vertex.normal = glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
		vertex.uv = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
		vertex.tanget = glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
		vertex.bitTangent = glm::vec3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);

		vertices.push_back(vertex);
	}

	for (uint32_t i = 0; i < mesh->mNumFaces; i++)
	{
		const aiFace& face = mesh->mFaces[i];
		for (uint32_t j = 0; j < face.mNumIndices; j++)
		{
			indices.push_back(face.mIndices[j]);
		}
	}

	const aiMaterial* const material = scene->mMaterials[mesh->mMaterialIndex];

	meshContext.faceCount = mesh->mNumFaces;
	//meshContext.material
	meshContext.vertices = std::move(vertices);
	meshContext.indices = std::move(indices);

	return meshContext;
}

bool resource::importer::ModelAssetImporter::canImport(const std::filesystem::path& path)
{
    bool extension = path.extension().string() == ".gltf"; //Currently only gltf is supported

    return extension;
}

resource::AssetType resource::importer::ModelAssetImporter::getAssetType() const
{
    return resource::AssetType::MODEL;
}

resource::Asset* resource::importer::ModelAssetImporter::import(const std::filesystem::path& path)
{
    Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(
		path.string(),
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_CalcTangentSpace |
		aiProcess_GenSmoothNormals |
		aiProcess_JoinIdenticalVertices |
		aiProcess_ImproveCacheLocality |
		aiProcess_OptimizeMeshes |
		aiProcess_OptimizeGraph
	);

	if (scene == nullptr || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		std::cerr << "Assimp Error: " << importer.GetErrorString() << std::endl;
		return 0;
	}

	ReadModelContext modelContext {};
	this->processAssimpNode(scene, scene->mRootNode, modelContext);

	return 0;
}
