#include "InstanceTypeRegistry.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace editor::instance
{
namespace
{
[[nodiscard]] InstancePropertyMap WorldTransformDefaults()
{
	return {{"Position", glm::vec3(0.0F)}, {"Rotation", glm::quat(1.0F, 0.0F, 0.0F, 0.0F)}, {"Scale", glm::vec3(1.0F)}};
}

[[nodiscard]] InstancePropertyMap ShadowDefaults()
{
	return {{"CastShadows", true},	  {"ShadowResolution", uint64{2'048}}, {"ShadowConstantBias", 0.0005},
			{"ShadowSlopeBias", 1.5}, {"ShadowNormalBias", 0.02},		   {"ShadowFilterRadius", 1.0}};
}

void MergeDefaults(InstancePropertyMap &Destination, InstancePropertyMap Source)
{
	for (auto &[Name, Value] : Source)
		Destination.insert_or_assign(std::move(Name), std::move(Value));
}

[[nodiscard]] string PropertyDisplayName(const string_view Name)
{
	string Result;
	Result.reserve(Name.size() + 8U);
	for (usize Index = 0; Index < Name.size(); ++Index)
	{
		const string::value_type Character = Name[Index];
		if (Index != 0 && std::isupper(static_cast<unsigned char>(Character)) != 0 &&
			std::islower(static_cast<unsigned char>(Name[Index - 1])) != 0)
		{
			Result.push_back(' ');
		}
		Result.push_back(Character);
	}
	return Result;
}

void BuildPropertySchema(InstanceTypeDescriptor &Descriptor)
{
	Descriptor.Properties.clear();
	Descriptor.Properties.reserve(Descriptor.DefaultProperties.size());
	for (const auto &[Name, Value] : Descriptor.DefaultProperties)
	{
		InstancePropertyDescriptor Property{.Name = Name, .DisplayName = PropertyDisplayName(Name), .DefaultValue = Value};
		if (std::holds_alternative<glm::quat>(Value))
			Property.Presentation = InstancePropertyPresentation::Rotation;
		else if (std::holds_alternative<InstanceAssetReference>(Value))
			Property.Presentation = InstancePropertyPresentation::Asset;
		if (Name == "Color")
		{
			Property.Presentation = InstancePropertyPresentation::Color;
			Property.Category = "Appearance";
		}
		else if (Name == "Position" || Name == "Rotation" || Name == "Scale" || Name.starts_with("Pivot"))
			Property.Category = "Transform";
		else if (Name.starts_with("Shadow") || Name == "CastShadows")
			Property.Category = "Shadows";
		else if (Name == "Shape")
		{
			Property.Presentation = InstancePropertyPresentation::Choice;
			Property.Choices = {"Box", "Sphere", "Capsule", "Cylinder", "Cone", "Plane"};
		}
		else if (Name == "Projection")
		{
			Property.Presentation = InstancePropertyPresentation::Choice;
			Property.Choices = {"Perspective", "Orthographic"};
		}
		if (Name == "FieldOfView")
			Property.Minimum = 1.0, Property.Maximum = 179.0, Property.Step = 0.25;
		else if (Name == "OrthographicHeight")
			Property.Minimum = 0.001, Property.Maximum = 1'000'000.0;
		else if (Name == "NearPlane")
			Property.Minimum = 0.0001, Property.Maximum = 100'000.0;
		else if (Name == "FarPlane")
			Property.Minimum = 0.001, Property.Maximum = 10'000'000.0;
		else if (Name == "IlluminanceLux" || Name == "LuminousPowerLumens" || Name == "Range" || Name == "SourceRadius" ||
				 Name == "AngularDiameterDegrees" || Name == "ShadowConstantBias" || Name == "ShadowSlopeBias" ||
				 Name == "ShadowNormalBias" || Name == "ShadowFilterRadius")
			Property.Minimum = 0.0;
		else if (Name == "InnerConeDegrees" || Name == "OuterConeDegrees")
			Property.Minimum = 0.0, Property.Maximum = 179.0, Property.Step = 0.25;
		else if (Name == "CascadeCount")
			Property.Minimum = 1.0, Property.Maximum = 4.0, Property.Step = 1.0;
		else if (Name == "CascadeDistributionExponent")
			Property.Minimum = 0.0, Property.Maximum = 8.0;
		else if (Name == "ShadowResolution")
		{
			Property.Presentation = InstancePropertyPresentation::Choice;
			Property.Choices = {"256", "512", "1024", "2048", "4096", "8192"};
			Property.Minimum = 256.0;
			Property.Maximum = 8192.0;
			Property.Step = 256.0;
		}
		else if (Name == "Weight")
			Property.Minimum = 0.0, Property.Maximum = 1.0;
		else if (Name == "Speed")
			Property.Minimum = -100.0, Property.Maximum = 100.0;
		if (Descriptor.ClassID == class_ids::Script &&
			(Name == "BehaviorType" || Name == "BehaviorName" || Name == "ModuleName" || Name == "StableTypeID" || Name == "SchemaVersion"))
		{
			Property.ReadOnly = true;
			Property.Category = "Behavior";
		}
		Descriptor.Properties.push_back(std::move(Property));
	}
	std::ranges::sort(Descriptor.Properties, [](const InstancePropertyDescriptor &Left, const InstancePropertyDescriptor &Right)
					  { return std::tie(Left.Category, Left.DisplayName) < std::tie(Right.Category, Right.DisplayName); });
}

[[nodiscard]] InstanceTypeDescriptor MakeType(const InstanceClassID ClassID, string ClassName, string Category, string IconGlyph,
											  const glm::vec4 IconColor, string Description,
											  const InstanceAvailability Availability = InstanceAvailability::Available)
{
	const string DisplayName = ClassName;
	return {.ClassID = ClassID,
			.ClassName = std::move(ClassName),
			.DisplayName = DisplayName,
			.Category = std::move(Category),
			.IconGlyph = std::move(IconGlyph),
			.IconColor = IconColor,
			.Description = std::move(Description),
			.Availability = Availability};
}
} // namespace

InstanceTypeRegistry::InstanceTypeRegistry()
{
	const auto RegisterService = [this](const InstanceClassID ID, string Name, string Icon, const glm::vec4 Color)
	{
		auto Descriptor = MakeType(ID, std::move(Name), "Services", std::move(Icon), Color, "Protected document service");
		Descriptor.Creatable = false;
		Descriptor.Service = true;
		this->Register(std::move(Descriptor));
	};
	RegisterService(class_ids::Workspace, "Workspace", "W", {0.35F, 0.75F, 1.0F, 1.0F});
	RegisterService(class_ids::Lighting, "Lighting", "L", {1.0F, 0.82F, 0.25F, 1.0F});
	RegisterService(class_ids::GUI, "GUI", "G", {0.45F, 0.9F, 0.65F, 1.0F});
	RegisterService(class_ids::Audio, "Audio", "A", {0.45F, 0.7F, 1.0F, 1.0F});
	RegisterService(class_ids::Scripts, "Scripts", "S", {0.75F, 0.55F, 1.0F, 1.0F});

	this->Register(MakeType(class_ids::Folder, "Folder", "Organization", "F", {1.0F, 0.78F, 0.3F, 1.0F}, "Hierarchy-only folder"));
	auto Model =
		MakeType(class_ids::Model, "Model", "Organization", "M", {0.65F, 0.75F, 1.0F, 1.0F}, "Pivoted collection of descendant parts");
	Model.DefaultProperties = {
		{"PivotPosition", glm::vec3(0.0F)}, {"PivotRotation", glm::quat(1.0F, 0.0F, 0.0F, 0.0F)}, {"PivotScale", glm::vec3(1.0F)}};
	Model.AllowedServiceClasses = {class_ids::Workspace};
	this->Register(std::move(Model));

	auto Part = MakeType(class_ids::Part, "Part", "World", "P", {0.75F, 0.8F, 0.88F, 1.0F}, "Primitive world geometry");
	Part.DefaultProperties = WorldTransformDefaults();
	Part.DefaultProperties.emplace("Shape", string("Box"));
	Part.AllowedServiceClasses = {class_ids::Workspace};
	this->Register(std::move(Part));
	auto MeshPart =
		MakeType(class_ids::MeshPart, "MeshPart", "World", "N", {0.55F, 0.85F, 0.75F, 1.0F}, "World geometry backed by a model asset");
	MeshPart.DefaultProperties = WorldTransformDefaults();
	MeshPart.DefaultProperties.emplace("Model", InstanceAssetReference{.Type = resource::AssetType::Model});
	MeshPart.AllowedServiceClasses = {class_ids::Workspace};
	this->Register(std::move(MeshPart));
	auto Camera = MakeType(class_ids::Camera, "Camera", "World", "C", {0.45F, 0.75F, 1.0F, 1.0F}, "Workspace camera");
	Camera.DefaultProperties = WorldTransformDefaults();
	Camera.AllowedServiceClasses = {class_ids::Workspace};
	MergeDefaults(Camera.DefaultProperties, {{"Projection", string("Perspective")},
											 {"FieldOfView", 60.0},
											 {"OrthographicHeight", 10.0},
											 {"NearPlane", 0.05},
											 {"FarPlane", 100'000.0},
											 {"ExposureCompensation", 0.0},
											 {"Primary", false},
											 {"TemporalJitter", true}});
	this->Register(std::move(Camera));
	auto Attachment = MakeType(class_ids::Attachment, "Attachment", "World", "T", {0.55F, 0.95F, 0.95F, 1.0F},
							   "Local attachment point on an immediate Part parent");
	Attachment.DefaultProperties = {{"Position", glm::vec3(0.0F)}, {"Rotation", glm::quat(1.0F, 0.0F, 0.0F, 0.0F)}};
	Attachment.ExactParentClasses = {class_ids::Part, class_ids::MeshPart};
	this->Register(std::move(Attachment));

	auto AddAvailable = [this](const InstanceClassID ID, string Name, string Category, string Icon, const glm::vec4 Color,
							   string Description, std::vector<InstanceClassID> Parents = {})
	{
		auto Descriptor = MakeType(ID, std::move(Name), std::move(Category), std::move(Icon), Color, std::move(Description));
		Descriptor.ExactParentClasses = std::move(Parents);
		this->Register(std::move(Descriptor));
	};
	auto DirectionalLight = MakeType(class_ids::DirectionalLight, "DirectionalLight", "Lighting and Effects", "D",
									 {1.0F, 0.85F, 0.3F, 1.0F}, "Directional world light");
	DirectionalLight.ExactParentClasses = {class_ids::Workspace, class_ids::Lighting};
	DirectionalLight.AllowedServiceClasses = {class_ids::Workspace, class_ids::Lighting};
	DirectionalLight.DefaultProperties = {{"Rotation", glm::quat(1.0F, 0.0F, 0.0F, 0.0F)},
										  {"Color", glm::vec3(1.0F)},
										  {"IlluminanceLux", 110'000.0},
										  {"AngularDiameterDegrees", 0.5357},
										  {"CascadeCount", uint64{4}},
										  {"CascadeDistributionExponent", 2.0}};
	MergeDefaults(DirectionalLight.DefaultProperties, ShadowDefaults());
	this->Register(std::move(DirectionalLight));
	auto PointLight = MakeType(class_ids::PointLight, "PointLight", "Lighting and Effects", "O", {1.0F, 0.65F, 0.25F, 1.0F},
							   "Omnidirectional light on its immediate spatial parent");
	PointLight.ExactParentClasses = {class_ids::Part, class_ids::MeshPart, class_ids::Attachment};
	PointLight.DefaultProperties = {{"Color", glm::vec3(1.0F)}, {"LuminousPowerLumens", 1'500.0}, {"Range", 20.0}, {"SourceRadius", 0.0}};
	MergeDefaults(PointLight.DefaultProperties, ShadowDefaults());
	this->Register(std::move(PointLight));
	auto SpotLight = MakeType(class_ids::SpotLight, "SpotLight", "Lighting and Effects", "Q", {1.0F, 0.6F, 0.25F, 1.0F},
							  "Cone light on its immediate spatial parent");
	SpotLight.ExactParentClasses = {class_ids::Part, class_ids::MeshPart, class_ids::Attachment};
	SpotLight.DefaultProperties = {{"Color", glm::vec3(1.0F)},
								   {"LuminousPowerLumens", 2'000.0},
								   {"Range", 30.0},
								   {"InnerConeDegrees", 25.0},
								   {"OuterConeDegrees", 35.0}};
	MergeDefaults(SpotLight.DefaultProperties, ShadowDefaults());
	this->Register(std::move(SpotLight));
	AddAvailable(class_ids::Animator, "Animator", "Animation", "R", {0.75F, 0.55F, 1.0F, 1.0F}, "Model animation controller",
				 {class_ids::Model});
	auto AnimationTrack = MakeType(class_ids::AnimationTrack, "AnimationTrack", "Animation", "K", {0.9F, 0.55F, 1.0F, 1.0F},
								   "Playback instance referencing one animation clip asset");
	AnimationTrack.ExactParentClasses = {class_ids::Animator};
	AnimationTrack.DefaultProperties = {
		{"Clip", InstanceAssetReference{.Type = resource::AssetType::AnimationClip}}, {"Playing", false}, {"Speed", 1.0}, {"Weight", 1.0}};
	this->Register(std::move(AnimationTrack));
	auto Script = MakeType(class_ids::Script, "Script", "Scripting", "S", {0.45F, 0.9F, 0.6F, 1.0F},
						   "Executable behavior bound to its immediate parent");
	Script.DefaultProperties = {{"BehaviorType", uint64{0}},
								{"BehaviorName", string{}},
								{"ModuleName", string("Engine")},
								{"StableTypeID", util::UUID{}},
								{"SchemaVersion", uint64{0}}};
	Script.ExactParentClasses = {class_ids::Model, class_ids::Part, class_ids::MeshPart, class_ids::Camera};
	Script.AllowedServiceClasses = {class_ids::Workspace};
	this->Register(std::move(Script));
	auto ModuleScript = MakeType(class_ids::ModuleScript, "ModuleScript", "Scripting", "U", {0.35F, 0.8F, 0.55F, 1.0F},
								 "Reusable script module definition");
	ModuleScript.DefaultProperties = {{"ModuleName", string("Game")}, {"ExportName", string{}}};
	ModuleScript.AllowedServiceClasses = {class_ids::Scripts};
	this->Register(std::move(ModuleScript));

	const auto AddUnavailable = [this](const InstanceClassID ID, string Name, string Category, string Icon, const glm::vec4 Color,
									   string Description, std::vector<InstanceClassID> Parents = {})
	{
		auto Descriptor = MakeType(ID, std::move(Name), std::move(Category), std::move(Icon), Color, std::move(Description),
								   InstanceAvailability::Unavailable);
		Descriptor.ExactParentClasses = std::move(Parents);
		this->Register(std::move(Descriptor));
	};
	AddUnavailable(class_ids::Sky, "Sky", "Lighting and Effects", "Y", {0.45F, 0.7F, 1.0F, 1.0F}, "Sky environment", {class_ids::Lighting});
	AddUnavailable(class_ids::Atmosphere, "Atmosphere", "Lighting and Effects", "H", {0.6F, 0.75F, 0.9F, 1.0F}, "Atmospheric scattering",
				   {class_ids::Lighting});
	AddUnavailable(class_ids::Decal, "Decal", "Lighting and Effects", "E", {1.0F, 0.55F, 0.75F, 1.0F}, "Surface decal",
				   {class_ids::Part, class_ids::MeshPart});
	AddUnavailable(class_ids::ParticleEmitter, "ParticleEmitter", "Lighting and Effects", "V", {1.0F, 0.65F, 0.35F, 1.0F},
				   "Particle effect", {class_ids::Part, class_ids::MeshPart, class_ids::Attachment});
	AddUnavailable(class_ids::Sound, "Sound", "Audio", "S", {0.35F, 0.75F, 1.0F, 1.0F}, "Playable audio clip");
	AddUnavailable(class_ids::AudioEmitter, "AudioEmitter", "Audio", "E", {0.35F, 0.8F, 1.0F, 1.0F}, "Spatial audio emitter",
				   {class_ids::Part, class_ids::MeshPart, class_ids::Attachment});
	AddUnavailable(class_ids::AudioListener, "AudioListener", "Audio", "L", {0.4F, 0.75F, 1.0F, 1.0F}, "Audio listener");
	AddUnavailable(class_ids::AudioGroup, "AudioGroup", "Audio", "G", {0.45F, 0.7F, 1.0F, 1.0F}, "Audio routing group", {class_ids::Audio});
	AddUnavailable(class_ids::ScreenGUI, "ScreenGUI", "GUI", "G", {0.35F, 0.9F, 0.65F, 1.0F}, "Screen-space UI root", {class_ids::GUI});
	AddUnavailable(class_ids::Frame, "Frame", "GUI", "F", {0.4F, 0.85F, 0.65F, 1.0F}, "GUI layout frame",
				   {class_ids::ScreenGUI, class_ids::Frame, class_ids::ScrollView});
	AddUnavailable(class_ids::Text, "Text", "GUI", "T", {0.5F, 0.9F, 0.7F, 1.0F}, "GUI text element",
				   {class_ids::ScreenGUI, class_ids::Frame, class_ids::ScrollView, class_ids::Button});
	AddUnavailable(class_ids::Image, "Image", "GUI", "I", {0.5F, 0.9F, 0.7F, 1.0F}, "GUI image element",
				   {class_ids::ScreenGUI, class_ids::Frame, class_ids::ScrollView, class_ids::Button});
	AddUnavailable(class_ids::Button, "Button", "GUI", "B", {0.45F, 0.85F, 0.65F, 1.0F}, "GUI button",
				   {class_ids::ScreenGUI, class_ids::Frame, class_ids::ScrollView});
	AddUnavailable(class_ids::ScrollView, "ScrollView", "GUI", "J", {0.4F, 0.85F, 0.65F, 1.0F}, "Scrollable GUI container",
				   {class_ids::ScreenGUI, class_ids::Frame, class_ids::ScrollView});
	AddUnavailable(class_ids::RigidBody, "RigidBody", "Physics", "R", {0.95F, 0.55F, 0.3F, 1.0F}, "Physics body",
				   {class_ids::Part, class_ids::MeshPart});
	AddUnavailable(class_ids::BoxCollider, "BoxCollider", "Physics", "B", {0.9F, 0.5F, 0.3F, 1.0F}, "Box collision shape",
				   {class_ids::Part, class_ids::MeshPart});
	AddUnavailable(class_ids::SphereCollider, "SphereCollider", "Physics", "O", {0.9F, 0.5F, 0.3F, 1.0F}, "Sphere collision shape",
				   {class_ids::Part, class_ids::MeshPart});
	AddUnavailable(class_ids::CapsuleCollider, "CapsuleCollider", "Physics", "C", {0.9F, 0.5F, 0.3F, 1.0F}, "Capsule collision shape",
				   {class_ids::Part, class_ids::MeshPart});
	AddUnavailable(class_ids::MeshCollider, "MeshCollider", "Physics", "M", {0.9F, 0.5F, 0.3F, 1.0F}, "Mesh collision shape",
				   {class_ids::MeshPart});
	AddUnavailable(class_ids::HingeConstraint, "HingeConstraint", "Physics", "H", {0.95F, 0.6F, 0.3F, 1.0F}, "Hinge constraint");
	AddUnavailable(class_ids::SpringConstraint, "SpringConstraint", "Physics", "S", {0.95F, 0.6F, 0.3F, 1.0F}, "Spring constraint");
}

void InstanceTypeRegistry::Register(InstanceTypeDescriptor Descriptor)
{
	if (!Descriptor.ClassID.IsValid() || Descriptor.ClassName.empty() || Descriptor.DisplayName.empty() || Descriptor.Category.empty())
		throw std::invalid_argument("Instance type descriptor is incomplete");
	BuildPropertySchema(Descriptor);
	std::unique_lock Lock(this->Mutex);
	if (this->ByID.contains(Descriptor.ClassID) || this->ByName.contains(Descriptor.ClassName))
		throw std::invalid_argument("Instance type identity is already registered");
	const InstanceClassID ID = Descriptor.ClassID;
	const string Name = Descriptor.ClassName;
	this->ByID.emplace(ID, std::make_shared<const InstanceTypeDescriptor>(std::move(Descriptor)));
	try
	{
		this->ByName.emplace(Name, ID);
	}
	catch (...)
	{
		this->ByID.erase(ID);
		throw;
	}
}

std::shared_ptr<const InstanceTypeDescriptor> InstanceTypeRegistry::Find(const InstanceClassID &ClassID) const
{
	std::shared_lock Lock(this->Mutex);
	const auto Iterator = this->ByID.find(ClassID);
	return Iterator == this->ByID.end() ? nullptr : Iterator->second;
}

std::shared_ptr<const InstanceTypeDescriptor> InstanceTypeRegistry::Find(const string_view ClassName) const
{
	std::shared_lock Lock(this->Mutex);
	const auto NameIterator = this->ByName.find(string(ClassName));
	if (NameIterator == this->ByName.end())
		return nullptr;
	const auto TypeIterator = this->ByID.find(NameIterator->second);
	return TypeIterator == this->ByID.end() ? nullptr : TypeIterator->second;
}

std::vector<InstanceTypeDescriptor> InstanceTypeRegistry::GetCreatableTypes() const
{
	std::vector<InstanceTypeDescriptor> Result;
	std::shared_lock Lock(this->Mutex);
	Result.reserve(this->ByID.size());
	for (const auto &[ID, Descriptor] : this->ByID)
	{
		(void)ID;
		if (Descriptor->Creatable)
			Result.push_back(*Descriptor);
	}
	std::ranges::sort(Result, [](const auto &Left, const auto &Right)
					  { return std::tie(Left.Category, Left.DisplayName) < std::tie(Right.Category, Right.DisplayName); });
	return Result;
}

std::vector<InstanceTypeDescriptor> InstanceTypeRegistry::GetTypes() const
{
	std::vector<InstanceTypeDescriptor> Result;
	std::shared_lock Lock(this->Mutex);
	Result.reserve(this->ByID.size());
	for (const auto &[ID, Descriptor] : this->ByID)
	{
		(void)ID;
		Result.push_back(*Descriptor);
	}
	std::ranges::sort(Result, [](const auto &Left, const auto &Right) { return Left.ClassName < Right.ClassName; });
	return Result;
}
} // namespace editor::instance
