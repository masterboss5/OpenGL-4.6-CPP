#include "ModelAsset.h"

resource::ModelAsset::ModelAsset(std::string name, std::vector<ModelSubmesh> submeshes, std::vector<ModelMaterial> materials)
	: Asset(util::UUID::generateRandomUUID()),
	name(std::move(name)),
	submeshes(std::move(submeshes)),
	materials(std::move(materials))
{
}

std::string_view resource::ModelAsset::getName() const noexcept
{
	return this->name;
}

const std::vector<resource::ModelSubmesh>& resource::ModelAsset::getSubmeshes() const noexcept
{
	return this->submeshes;
}

const std::vector<resource::ModelMaterial>& resource::ModelAsset::getMaterials() const noexcept
{
	return this->materials;
}
