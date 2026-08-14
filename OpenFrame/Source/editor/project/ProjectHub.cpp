#include "ProjectHub.h"

#include "Source/core/io/SecurePath.h"
#include "Source/core/io/UserPaths.h"
#include "Source/editor/instance/InstanceTypes.h"
#include "Source/editor/serialization/SceneDocumentSerializer.h"
#include "Source/editor/serialization/ProjectDescriptorSerializer.h"
#include "Source/util/UUID.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

namespace editor::project
{
namespace
{
using Json = nlohmann::json;

constexpr uint32 RecentProjectsFormatVersion = 1;
constexpr usize MaximumRecentProjects = 20;

[[nodiscard]] int64 CurrentTimeMilliseconds() noexcept
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] bool IsValidProjectName(const string_view Name) noexcept
{
	if (Name.empty() || Name.size() > 96 || Name == "." || Name == ".." || Name.back() == ' ' || Name.back() == '.')
		return false;
	constexpr string_view InvalidCharacters = "<>:\"/\\|?*";
	return std::ranges::none_of(Name, [](const char Character) { return static_cast<unsigned char>(Character) < 32U; }) &&
		   Name.find_first_of(InvalidCharacters) == string_view::npos;
}

[[nodiscard]] std::span<const uint8> BytesOf(const string &Text) noexcept
{
	return {reinterpret_cast<const uint8 *>(Text.data()), Text.size()};
}

void WriteTextFile(const std::filesystem::path &Root, const std::filesystem::path &Relative, const string &Text, const string_view Purpose)
{
	core::io::SecurePath::WriteFileWithin(Root, Relative, BytesOf(Text), false, true, Purpose);
}

[[nodiscard]] string BuildBaseplateScene(const string_view ProjectName)
{
	const util::UUID SceneID = util::UUID::GenerateRandomUUID();
	const util::UUID WorkspaceID = util::UUID::GenerateRandomUUID();
	const util::UUID LightingServiceID = util::UUID::GenerateRandomUUID();
	const util::UUID GUIID = util::UUID::GenerateRandomUUID();
	const util::UUID AudioID = util::UUID::GenerateRandomUUID();
	const util::UUID ScriptsID = util::UUID::GenerateRandomUUID();
	const util::UUID CameraID = util::UUID::GenerateRandomUUID();
	const util::UUID BaseplateID = util::UUID::GenerateRandomUUID();
	const util::UUID LightID = util::UUID::GenerateRandomUUID();
	const auto Property = [](string Kind, Json Value) { return Json{{"Kind", std::move(Kind)}, {"Value", std::move(Value)}}; };
	const auto Instance = [](const util::UUID &ID, const instance::InstanceClassID ClassID, string ClassName, string Name,
							 const util::UUID &Parent, const uint32 Order, const bool Protected, Json Properties = Json::object())
	{
		return Json{{"ID", ID.ToString()},
					{"ClassID", ClassID.ToString()},
					{"ClassName", std::move(ClassName)},
					{"Name", std::move(Name)},
					{"Parent", Parent.IsValid() ? Json(Parent.ToString()) : Json(nullptr)},
					{"SiblingOrder", Order},
					{"Enabled", true},
					{"Protected", Protected},
					{"Properties", std::move(Properties)}};
	};
	const auto TransformProperties =
		[&Property](const std::array<float32, 3> Position, const std::array<float32, 3> RotationEuler, const std::array<float32, 3> Scale)
	{
		const glm::quat Rotation = glm::quat(glm::radians(glm::vec3(RotationEuler[0], RotationEuler[1], RotationEuler[2])));
		return Json{{"Position", Property("Vector3", Json::array({Position[0], Position[1], Position[2]}))},
					{"Rotation", Property("Quaternion", Json::array({Rotation.w, Rotation.x, Rotation.y, Rotation.z}))},
					{"Scale", Property("Vector3", Json::array({Scale[0], Scale[1], Scale[2]}))}};
	};
	const auto Identity = [](const util::UUID &ID, const string_view Name, const uint32 Order)
	{
		return Json{{"ID", ID.ToString()},
					{"Name", Name},
					{"Parent", nullptr},
					{"SiblingOrder", Order},
					{"Components", Json{{"CObjectIdentityComponent",
										 {{"Type", "CObjectIdentityComponent"},
										  {"SchemaVersion", 1},
										  {"Enabled", true},
										  {"Properties",
										   {{"PersistentID", ID.ToString()},
											{"Name", Name},
											{"Tags", Json::array()},
											{"Mobility", 2},
											{"EditorVisible", true},
											{"Locked", false}}}}},
										{"CObjectHierarchyComponent",
										 {{"Type", "CObjectHierarchyComponent"},
										  {"SchemaVersion", 1},
										  {"Enabled", true},
										  {"Properties", {{"Parent", nullptr}, {"SiblingOrder", Order}}}}}}}};
	};
	const auto Transform =
		[](const std::array<float32, 3> Position, const std::array<float32, 3> Rotation, const std::array<float32, 3> Scale)
	{
		return Json{{"Type", "CObjectTransformComponent"},
					{"SchemaVersion", 1},
					{"Enabled", true},
					{"Properties", {{"Position", Position}, {"RotationEuler", Rotation}, {"Scale", Scale}}}};
	};

	Json Camera = Identity(CameraID, "Primary Camera", 0);
	Camera["Components"]["CObjectTransformComponent"] = Transform({0.0f, 6.0f, 12.0f}, {-18.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	Camera["Components"]["CObjectCameraComponent"] = Json{{"Type", "CObjectCameraComponent"},
														  {"SchemaVersion", 1},
														  {"Enabled", true},
														  {"Properties",
														   {{"Projection", 0},
															{"VerticalFieldOfViewDegrees", 60.0f},
															{"OrthographicHeight", 10.0f},
															{"NearPlane", 0.05f},
															{"FarPlane", 100000.0f},
															{"ExposureCompensation", 0.0f},
															{"Primary", true},
															{"TemporalJitterEnabled", true}}}};

	Json Baseplate = Identity(BaseplateID, "Baseplate", 1);
	Baseplate["Components"]["CObjectTransformComponent"] = Transform({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	Baseplate["Components"]["CObjectMeshComponent"] = Json{{"Type", "CObjectMeshComponent"},
														   {"SchemaVersion", 1},
														   {"Enabled", true},
														   {"Properties",
															{{"Model", {{"ID", ""}, {"Type", 3}, {"Path", "Meshes/Baseplate.obj"}}},
															 {"Visibility", 15},
															 {"LODMode", 0},
															 {"LODBias", 0},
															 {"ForcedLOD", 0},
															 {"RenderLayerMask", 4294967295ULL}}},
														   {"MaterialOverrides", Json::array()}};

	Json Light = Identity(LightID, "Sun", 2);
	Light["Components"]["CObjectTransformComponent"] = Transform({4.0f, 8.0f, 4.0f}, {-45.0f, 35.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	Light["Components"]["CObjectDirectionalLightComponent"] = Json{{"Type", "CObjectDirectionalLightComponent"},
																   {"SchemaVersion", 1},
																   {"Enabled", true},
																   {"Properties",
																	{{"Color", {1.0f, 0.96f, 0.9f}},
																	 {"IlluminanceLux", 50000.0f},
																	 {"AngularDiameterDegrees", 0.53f},
																	 {"CascadeCount", 4},
																	 {"CascadeDistributionExponent", 2.0f},
																	 {"CastShadows", true},
																	 {"ShadowResolution", 2048},
																	 {"ShadowConstantBias", 0.0005f},
																	 {"ShadowSlopeBias", 1.5f},
																	 {"ShadowNormalBias", 0.02f},
																	 {"ShadowFilterRadius", 1.5f}}}};

	Json CameraProperties = TransformProperties({0.0F, 6.0F, 12.0F}, {-18.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F});
	CameraProperties.update({{"Projection", Property("String", "Perspective")},
							 {"FieldOfView", Property("Scalar", 60.0)},
							 {"OrthographicHeight", Property("Scalar", 10.0)},
							 {"NearPlane", Property("Scalar", 0.05)},
							 {"FarPlane", Property("Scalar", 100'000.0)},
							 {"ExposureCompensation", Property("Scalar", 0.0)},
							 {"Primary", Property("Boolean", true)},
							 {"TemporalJitter", Property("Boolean", true)}});
	Json BaseplateProperties = TransformProperties({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {16.0F, 1.0F, 16.0F});
	BaseplateProperties["Shape"] = Property("String", "Plane");
	Json LightProperties = TransformProperties({0.0F, 0.0F, 0.0F}, {-45.0F, 35.0F, 0.0F}, {1.0F, 1.0F, 1.0F});
	LightProperties.erase("Position");
	LightProperties.erase("Scale");
	LightProperties.update({{"Color", Property("Vector3", Json::array({1.0F, 0.96F, 0.9F}))},
							{"IlluminanceLux", Property("Scalar", 50'000.0)},
							{"AngularDiameterDegrees", Property("Scalar", 0.53)},
							{"CascadeCount", Property("UnsignedInteger", uint64{4})},
							{"CascadeDistributionExponent", Property("Scalar", 2.0)},
							{"CastShadows", Property("Boolean", true)},
							{"ShadowResolution", Property("UnsignedInteger", uint64{2'048})},
							{"ShadowConstantBias", Property("Scalar", 0.0005)},
							{"ShadowSlopeBias", Property("Scalar", 1.5)},
							{"ShadowNormalBias", Property("Scalar", 0.02)},
							{"ShadowFilterRadius", Property("Scalar", 1.5)}});
	Json Instances = Json::array();
	Instances.push_back(Instance(WorkspaceID, instance::class_ids::Workspace, "Workspace", "Workspace", {}, 0, true));
	Instances.push_back(
		Instance(CameraID, instance::class_ids::Camera, "Camera", "Primary Camera", WorkspaceID, 0, false, std::move(CameraProperties)));
	Instances.push_back(
		Instance(BaseplateID, instance::class_ids::Part, "Part", "Baseplate", WorkspaceID, 1, false, std::move(BaseplateProperties)));
	Instances.push_back(Instance(LightingServiceID, instance::class_ids::Lighting, "Lighting", "Lighting", {}, 1, true));
	Instances.push_back(Instance(LightID, instance::class_ids::DirectionalLight, "DirectionalLight", "Sun", LightingServiceID, 0, false,
								 std::move(LightProperties)));
	Instances.push_back(Instance(GUIID, instance::class_ids::GUI, "GUI", "GUI", {}, 2, true));
	Instances.push_back(Instance(AudioID, instance::class_ids::Audio, "Audio", "Audio", {}, 3, true));
	Instances.push_back(Instance(ScriptsID, instance::class_ids::Scripts, "Scripts", "Scripts", {}, 4, true));

	return Json{{"FormatVersion", serialization::SceneDocumentSerializer::CurrentFormatVersion},
				{"EngineSchemaVersion", 1},
				{"MigrationData", Json::object()},
				{"ID", SceneID.ToString()},
				{"Name", string(ProjectName) + " Baseplate"},
				{"Instances", std::move(Instances)},
				{"Objects", Json::array({std::move(Camera), std::move(Baseplate), std::move(Light)})}}
			   .dump(2) +
		   '\n';
}
} // namespace

ProjectHub::ProjectHub() : ProjectHub(GetDefaultProjectsRoot(), GetApplicationDataRoot() / "Editor")
{
}

ProjectHub::ProjectHub(std::filesystem::path ProjectsRoot, std::filesystem::path StateRoot)
	: ProjectsRoot(std::filesystem::absolute(std::move(ProjectsRoot)).lexically_normal()),
	  StateRoot(std::filesystem::absolute(std::move(StateRoot)).lexically_normal())
{
	if (this->ProjectsRoot.empty() || this->StateRoot.empty())
		throw ProjectHubException("Project Hub roots cannot be empty");
	if (this->ProjectsRoot == this->StateRoot)
		throw ProjectHubException("Project and editor-state roots must be distinct");
	core::io::SecurePath::CreateTrustedRoot(this->ProjectsRoot, "OpenFrame projects root");
	core::io::SecurePath::CreateTrustedRoot(this->StateRoot, "OpenFrame editor state root");
	this->LoadRecentProjects();
}

std::filesystem::path ProjectHub::GetDefaultProjectsRoot()
{
	return core::io::UserPaths::OpenFrameProjects();
}

std::filesystem::path ProjectHub::GetApplicationDataRoot()
{
	return core::io::UserPaths::OpenFrameApplicationData();
}

const std::filesystem::path &ProjectHub::GetProjectsRoot() const noexcept
{
	return this->ProjectsRoot;
}

std::span<const RecentProject> ProjectHub::GetRecentProjects() const noexcept
{
	return this->RecentProjects;
}

void ProjectHub::Refresh()
{
	for (RecentProject &Recent : this->RecentProjects)
	{
		std::error_code Error;
		Recent.Available = std::filesystem::is_regular_file(Recent.DescriptorPath, Error) && !Error;
	}
}

ProjectDescriptor ProjectHub::OpenProject(const std::filesystem::path &DescriptorPath)
{
	ProjectDescriptor Descriptor = serialization::ProjectDescriptorSerializer::Load(DescriptorPath);
	this->RecordRecent(Descriptor);
	return Descriptor;
}

ProjectDescriptor ProjectHub::CreateBaseplateProject(const NewProjectSpecification &Specification)
{
	if (!IsValidProjectName(Specification.Name))
		throw ProjectHubException("Project name is empty or contains characters Windows cannot use in a folder name");
	const std::filesystem::path Parent =
		std::filesystem::absolute(Specification.ParentDirectory.empty() ? this->ProjectsRoot : Specification.ParentDirectory)
			.lexically_normal();
	core::io::SecurePath::CreateTrustedRoot(Parent, "new project parent");
	const std::filesystem::path Root = Parent / Specification.Name;
	std::error_code Error;
	if (std::filesystem::exists(Root, Error) || Error)
		throw ProjectHubException("A file or folder already exists at the requested project location");
	core::io::SecurePath::CreateDirectoriesWithin(Parent, Specification.Name, "new project root");
	try
	{
		core::io::SecurePath::CreateDirectoriesWithin(Root, "Content/Scenes", "baseplate scenes");
		core::io::SecurePath::CreateDirectoriesWithin(Root, "Content/Meshes", "baseplate meshes");
		core::io::SecurePath::CreateDirectoriesWithin(Root, "Source", "project source");
		core::io::SecurePath::CreateDirectoriesWithin(Root, "Config", "project config");
		const string Plane = "o Baseplate\n"
							 "v -8.0 0.0 -8.0\n"
							 "v  8.0 0.0 -8.0\n"
							 "v  8.0 0.0  8.0\n"
							 "v -8.0 0.0  8.0\n"
							 "vn 0.0 1.0 0.0\n"
							 "vt 0.0 0.0\nvt 8.0 0.0\nvt 8.0 8.0\nvt 0.0 8.0\n"
							 "f 1/1/1 4/4/1 3/3/1\n"
							 "f 1/1/1 3/3/1 2/2/1\n";
		WriteTextFile(Root, "Content/Meshes/Baseplate.obj", Plane, "baseplate mesh");
		WriteTextFile(Root, "Content/Scenes/Baseplate.enginelevel", BuildBaseplateScene(Specification.Name), "baseplate scene");

		ProjectDescriptor Descriptor{.Name = Specification.Name,
									 .DescriptorPath = Root / (Specification.Name + ".engineproject"),
									 .ContentMounts = {{.VirtualRoot = "/Game", .PhysicalRoot = "Content", .ReadOnly = false}},
									 .StartupScene = "Scenes/Baseplate.enginelevel",
									 .GameModule = {}};
		serialization::ProjectDescriptorSerializer::Save(Descriptor);
		Descriptor = serialization::ProjectDescriptorSerializer::Load(Descriptor.DescriptorPath);
		this->RecordRecent(Descriptor);
		return Descriptor;
	}
	catch (...)
	{
		try
		{
			core::io::SecurePath::RemoveWithin(Parent, Specification.Name, true, "failed project creation rollback");
		}
		catch (...)
		{
		}
		throw;
	}
}

void ProjectHub::RemoveRecent(const std::filesystem::path &DescriptorPath)
{
	const std::filesystem::path Normal = std::filesystem::absolute(DescriptorPath).lexically_normal();
	std::erase_if(this->RecentProjects, [&Normal](const RecentProject &Recent) { return Recent.DescriptorPath == Normal; });
	this->SaveRecentProjects();
}

void ProjectHub::LoadRecentProjects()
{
	this->RecentProjects.clear();
	const std::filesystem::path StateFile = this->StateRoot / "RecentProjects.json";
	if (!std::filesystem::is_regular_file(StateFile))
		return;
	try
	{
		constexpr uint64 MaximumStateBytes = 2U * 1024U * 1024U;
		const std::vector<uint8> Bytes =
			core::io::SecurePath::ReadFileWithin(this->StateRoot, StateFile.filename(), MaximumStateBytes, "recent projects");
		const Json Root = Json::parse(Bytes.begin(), Bytes.end(), nullptr, true, true);
		if (!Root.is_object() || Root.value("FormatVersion", uint32{0}) != RecentProjectsFormatVersion || !Root.contains("Projects") ||
			!Root["Projects"].is_array())
		{
			throw ProjectHubException("Recent-project state has an unsupported format");
		}
		for (const Json &Entry : Root["Projects"])
		{
			if (!Entry.is_object() || !Entry.contains("Name") || !Entry["Name"].is_string() || !Entry.contains("Path") ||
				!Entry["Path"].is_string())
				continue;
			const std::filesystem::path Path = std::filesystem::absolute(Entry["Path"].get<string>()).lexically_normal();
			std::error_code Error;
			this->RecentProjects.push_back({.Name = Entry["Name"].get<string>(),
											.DescriptorPath = Path,
											.LastOpenedMilliseconds = Entry.value("LastOpenedMilliseconds", int64{0}),
											.Available = std::filesystem::is_regular_file(Path, Error) && !Error});
			if (this->RecentProjects.size() == MaximumRecentProjects)
				break;
		}
	}
	catch (...)
	{
		this->RecentProjects.clear();
	}
}

void ProjectHub::SaveRecentProjects() const
{
	Json Projects = Json::array();
	for (const RecentProject &Recent : this->RecentProjects)
		Projects.push_back({{"Name", Recent.Name},
							{"Path", Recent.DescriptorPath.generic_string()},
							{"LastOpenedMilliseconds", Recent.LastOpenedMilliseconds}});
	const string Serialized = Json{{"FormatVersion", RecentProjectsFormatVersion}, {"Projects", std::move(Projects)}}.dump(2) + '\n';
	const std::filesystem::path Temporary = "RecentProjects.tmp-" + util::UUID::GenerateRandomUUID().ToString() + ".json";
	WriteTextFile(this->StateRoot, Temporary, Serialized, "recent projects temporary file");
	core::io::SecurePath::ReplaceWithin(this->StateRoot, Temporary, "RecentProjects.json", "recent projects publication");
}

void ProjectHub::RecordRecent(const ProjectDescriptor &Descriptor)
{
	const std::filesystem::path Normal = std::filesystem::absolute(Descriptor.DescriptorPath).lexically_normal();
	std::erase_if(this->RecentProjects, [&Normal](const RecentProject &Recent) { return Recent.DescriptorPath == Normal; });
	this->RecentProjects.insert(
		this->RecentProjects.begin(),
		{.Name = Descriptor.Name, .DescriptorPath = Normal, .LastOpenedMilliseconds = CurrentTimeMilliseconds(), .Available = true});
	if (this->RecentProjects.size() > MaximumRecentProjects)
		this->RecentProjects.resize(MaximumRecentProjects);
	this->SaveRecentProjects();
}
} // namespace editor::project
