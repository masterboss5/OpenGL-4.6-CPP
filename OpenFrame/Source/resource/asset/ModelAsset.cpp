#include "ModelAsset.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace resource
{
ModelAsset::ModelAsset(string Name, resource::Bounds Bounds, std::vector<ModelNode> Nodes, std::vector<ModelMeshInstance> MeshInstances,
					   std::vector<AssetHandle<AnimationClipAsset>> AnimationClips, AssetHandle<AnimationGraphAsset> DefaultAnimationGraph)
	: Asset(util::UUID::GenerateRandomUUID()), Name(std::move(Name)), Bounds(Bounds), Nodes(std::move(Nodes)),
	  MeshInstances(std::move(MeshInstances)), AnimationClips(std::move(AnimationClips)),
	  DefaultAnimationGraph(std::move(DefaultAnimationGraph))
{
	if (this->Name.empty() || !this->Bounds.IsValid() || this->Nodes.empty() || this->MeshInstances.empty())
	{
		throw std::invalid_argument("Model asset requires a name, valid bounds, nodes, and mesh instances");
	}
	std::unordered_set<ModelNodeID> NodeIDs;
	for (uint32 Index = 0; Index < this->Nodes.size(); ++Index)
	{
		const ModelNode &Node = this->Nodes[Index];
		if (Node.ID == 0 || Node.Name.empty() || !NodeIDs.insert(Node.ID).second ||
			(Node.ParentIndex != InvalidModelNodeIndex && Node.ParentIndex >= Index))
		{
			throw std::invalid_argument("Model nodes require unique IDs and parent-before-child ordering");
		}
	}
	std::unordered_set<ModelMeshInstanceID> InstanceIDs;
	for (const ModelMeshInstance &Instance : this->MeshInstances)
	{
		if (Instance.ID == 0 || Instance.NodeIndex >= this->Nodes.size() || !Instance.Mesh || !InstanceIDs.insert(Instance.ID).second)
		{
			throw std::invalid_argument("Model mesh instances require unique IDs, valid nodes, and Mesh asset handles");
		}
	}
	for (const auto &Clip : this->AnimationClips)
	{
		if (!Clip)
			throw std::invalid_argument("Model animation clip handles cannot be empty");
	}
	if (!this->AnimationClips.empty() && !this->DefaultAnimationGraph)
		throw std::invalid_argument("Animated model requires a default animation graph");
}

const ModelMeshInstance *ModelAsset::FindMeshInstance(ModelMeshInstanceID ID) const noexcept
{
	const auto Found = std::find_if(this->MeshInstances.begin(), this->MeshInstances.end(),
									[ID](const ModelMeshInstance &Instance) { return Instance.ID == ID; });
	return Found == this->MeshInstances.end() ? nullptr : &*Found;
}
} // namespace resource
