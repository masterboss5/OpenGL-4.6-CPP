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
#include <cmath>
#include <format>
#include <limits>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

namespace editor::project
{
namespace
{
using Json = nlohmann::json;

constexpr uint32 RecentProjectsFormatVersion = 2;
constexpr usize MaximumRecentProjects = 20;
constexpr uint32 ProjectThumbnailWidth = 480;
constexpr uint32 ProjectThumbnailHeight = 270;
constexpr uint64 MaximumProjectThumbnailBytes = 16U * 1024U * 1024U;
constexpr string_view ProjectThumbnailDirectory = "ProjectThumbnails";

[[nodiscard]] int64 CurrentTimeMilliseconds() noexcept
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] int64 FileTimeMilliseconds(const std::filesystem::file_time_type Time) noexcept
{
	const auto SystemTime = std::chrono::time_point_cast<std::chrono::milliseconds>(Time - std::filesystem::file_time_type::clock::now() +
																					std::chrono::system_clock::now());
	return SystemTime.time_since_epoch().count();
}

[[nodiscard]] int64 LastWriteMilliseconds(const std::filesystem::path &Path) noexcept
{
	std::error_code Error;
	const std::filesystem::file_time_type Time = std::filesystem::last_write_time(Path, Error);
	return Error ? int64{0} : FileTimeMilliseconds(Time);
}

[[nodiscard]] int64 LatestAuthoredWriteMilliseconds(const ProjectDescriptor &Descriptor) noexcept
{
	int64 Result = LastWriteMilliseconds(Descriptor.DescriptorPath);
	const std::filesystem::path Root = Descriptor.DescriptorPath.parent_path();
	constexpr std::array<string_view, 3> AuthoredDirectories{"Content", "Source", "Config"};
	constexpr usize MaximumInspectedEntries = 250'000;
	usize InspectedEntries = 0;
	for (const string_view DirectoryName : AuthoredDirectories)
	{
		const std::filesystem::path Directory = Root / DirectoryName;
		std::error_code Error;
		if (!std::filesystem::is_directory(Directory, Error) || Error)
			continue;
		std::filesystem::recursive_directory_iterator Iterator(Directory, std::filesystem::directory_options::skip_permission_denied,
															   Error);
		const std::filesystem::recursive_directory_iterator End;
		while (!Error && Iterator != End && InspectedEntries < MaximumInspectedEntries)
		{
			const std::filesystem::directory_entry Entry = *Iterator;
			++InspectedEntries;
			std::error_code EntryError;
			if (Entry.is_symlink(EntryError))
			{
				if (Entry.is_directory(EntryError))
					Iterator.disable_recursion_pending();
			}
			else if (!EntryError && Entry.is_regular_file(EntryError))
			{
				Result = std::max(Result, LastWriteMilliseconds(Entry.path()));
			}
			Iterator.increment(Error);
		}
	}
	return Result;
}

[[nodiscard]] uint64 HashPath(const std::filesystem::path &Path) noexcept
{
	constexpr uint64 OffsetBasis = 14'695'981'039'346'656'037ULL;
	constexpr uint64 Prime = 1'099'511'628'211ULL;
	uint64 Result = OffsetBasis;
	const string Text = Path.generic_string();
	for (const unsigned char Value : Text)
	{
		Result ^= Value;
		Result *= Prime;
	}
	return Result;
}

[[nodiscard]] std::filesystem::path ThumbnailRelativePath(const RecentProject &Recent)
{
	const string Stem = Recent.ID.IsValid() ? Recent.ID.ToString() : std::format("path-{:016x}", HashPath(Recent.DescriptorPath));
	return std::filesystem::path(ProjectThumbnailDirectory) / (Stem + ".bmp");
}

void WriteUInt16(std::vector<uint8> &Bytes, const usize Offset, const uint16 Value)
{
	Bytes.at(Offset) = static_cast<uint8>(Value);
	Bytes.at(Offset + 1U) = static_cast<uint8>(Value >> 8U);
}

void WriteUInt32(std::vector<uint8> &Bytes, const usize Offset, const uint32 Value)
{
	Bytes.at(Offset) = static_cast<uint8>(Value);
	Bytes.at(Offset + 1U) = static_cast<uint8>(Value >> 8U);
	Bytes.at(Offset + 2U) = static_cast<uint8>(Value >> 16U);
	Bytes.at(Offset + 3U) = static_cast<uint8>(Value >> 24U);
}

[[nodiscard]] uint16 ReadUInt16(const std::span<const uint8> Bytes, const usize Offset)
{
	if (Offset + 2U > Bytes.size())
		throw ProjectHubException("Project thumbnail contains a truncated 16-bit field");
	return static_cast<uint16>(Bytes[Offset]) | static_cast<uint16>(static_cast<uint16>(Bytes[Offset + 1U]) << 8U);
}

[[nodiscard]] uint32 ReadUInt32(const std::span<const uint8> Bytes, const usize Offset)
{
	if (Offset + 4U > Bytes.size())
		throw ProjectHubException("Project thumbnail contains a truncated 32-bit field");
	return static_cast<uint32>(Bytes[Offset]) | (static_cast<uint32>(Bytes[Offset + 1U]) << 8U) |
		   (static_cast<uint32>(Bytes[Offset + 2U]) << 16U) | (static_cast<uint32>(Bytes[Offset + 3U]) << 24U);
}

[[nodiscard]] ProjectThumbnailImage CreateDefaultThumbnail(const RecentProject &Recent)
{
	ProjectThumbnailImage Result{.Width = ProjectThumbnailWidth, .Height = ProjectThumbnailHeight};
	Result.Pixels.resize(static_cast<usize>(ProjectThumbnailWidth) * ProjectThumbnailHeight * 4U);
	const uint64 Seed = Recent.ID.IsValid() ? Recent.ID.GetLeft() ^ Recent.ID.GetRight() : HashPath(Recent.DescriptorPath);
	const float32 HueShift = static_cast<float32>(Seed & 0xFFU) / 255.0F;
	for (uint32 Y = 0; Y < ProjectThumbnailHeight; ++Y)
	{
		for (uint32 X = 0; X < ProjectThumbnailWidth; ++X)
		{
			const float32 U = static_cast<float32>(X) / static_cast<float32>(ProjectThumbnailWidth - 1U);
			const float32 V = static_cast<float32>(Y) / static_cast<float32>(ProjectThumbnailHeight - 1U);
			const float32 Distance = std::sqrt((U - 0.68F) * (U - 0.68F) + (V - 0.32F) * (V - 0.32F));
			const float32 Glow = std::clamp(1.0F - Distance * 2.25F, 0.0F, 1.0F);
			const float32 Horizon = std::exp(-std::abs(V - (0.70F - U * 0.12F)) * 65.0F);
			const float32 GridX = std::exp(-std::abs(std::fmod(U * 12.0F + 0.5F, 1.0F) - 0.5F) * 75.0F);
			const float32 GridY = std::exp(-std::abs(std::fmod((V + U * 0.10F) * 9.0F + 0.5F, 1.0F) - 0.5F) * 75.0F);
			const float32 Grid = std::max(GridX, GridY) * std::clamp((V - 0.34F) * 1.8F, 0.0F, 1.0F);
			const float32 Vignette = std::clamp(1.15F - std::sqrt((U - 0.5F) * (U - 0.5F) + (V - 0.5F) * (V - 0.5F)), 0.5F, 1.0F);
			const float32 Red = (11.0F + Glow * (10.0F + HueShift * 4.0F) + Horizon * 45.0F + Grid * 7.0F) * Vignette;
			const float32 Green = (17.0F + Glow * 54.0F + Horizon * 31.0F + Grid * 18.0F) * Vignette;
			const float32 Blue = (19.0F + Glow * 51.0F + Horizon * 13.0F + Grid * 19.0F) * Vignette;
			const usize Offset = (static_cast<usize>(Y) * ProjectThumbnailWidth + X) * 4U;
			Result.Pixels[Offset] = static_cast<uint8>(std::clamp(Red, 0.0F, 255.0F));
			Result.Pixels[Offset + 1U] = static_cast<uint8>(std::clamp(Green, 0.0F, 255.0F));
			Result.Pixels[Offset + 2U] = static_cast<uint8>(std::clamp(Blue, 0.0F, 255.0F));
			Result.Pixels[Offset + 3U] = 255U;
		}
	}
	return Result;
}

[[nodiscard]] ProjectThumbnailImage ResizeThumbnail(const uint32 SourceWidth, const uint32 SourceHeight,
													const std::span<const uint8> SourcePixels, const bool SourceRowsAreBottomUp)
{
	const uint64 SourcePixelCount = static_cast<uint64>(SourceWidth) * SourceHeight;
	if (SourceWidth == 0 || SourceHeight == 0 || SourcePixelCount > std::numeric_limits<usize>::max() / 4U ||
		SourcePixels.size() != static_cast<usize>(SourcePixelCount * 4U))
	{
		throw ProjectHubException("Project thumbnail source pixels do not match their declared extent");
	}
	ProjectThumbnailImage Result{.Width = ProjectThumbnailWidth, .Height = ProjectThumbnailHeight};
	Result.Pixels.resize(static_cast<usize>(ProjectThumbnailWidth) * ProjectThumbnailHeight * 4U);
	constexpr float64 TargetAspect = static_cast<float64>(ProjectThumbnailWidth) / ProjectThumbnailHeight;
	const float64 SourceAspect = static_cast<float64>(SourceWidth) / SourceHeight;
	float64 CropX = 0.0;
	float64 CropY = 0.0;
	float64 CropWidth = SourceWidth;
	float64 CropHeight = SourceHeight;
	if (SourceAspect > TargetAspect)
	{
		CropWidth = CropHeight * TargetAspect;
		CropX = (static_cast<float64>(SourceWidth) - CropWidth) * 0.5;
	}
	else
	{
		CropHeight = CropWidth / TargetAspect;
		CropY = (static_cast<float64>(SourceHeight) - CropHeight) * 0.5;
	}
	const auto SourceComponent = [&](const uint32 X, const uint32 YFromTop, const uint32 Component)
	{
		const uint32 StorageY = SourceRowsAreBottomUp ? SourceHeight - 1U - YFromTop : YFromTop;
		return SourcePixels[(static_cast<usize>(StorageY) * SourceWidth + X) * 4U + Component];
	};
	for (uint32 Y = 0; Y < ProjectThumbnailHeight; ++Y)
	{
		const float64 SourceY = CropY + (static_cast<float64>(Y) + 0.5) * CropHeight / ProjectThumbnailHeight - 0.5;
		const uint32 Y0 = static_cast<uint32>(std::clamp(std::floor(SourceY), 0.0, static_cast<float64>(SourceHeight - 1U)));
		const uint32 Y1 = std::min(Y0 + 1U, SourceHeight - 1U);
		const float64 WeightY = std::clamp(SourceY - std::floor(SourceY), 0.0, 1.0);
		for (uint32 X = 0; X < ProjectThumbnailWidth; ++X)
		{
			const float64 SourceX = CropX + (static_cast<float64>(X) + 0.5) * CropWidth / ProjectThumbnailWidth - 0.5;
			const uint32 X0 = static_cast<uint32>(std::clamp(std::floor(SourceX), 0.0, static_cast<float64>(SourceWidth - 1U)));
			const uint32 X1 = std::min(X0 + 1U, SourceWidth - 1U);
			const float64 WeightX = std::clamp(SourceX - std::floor(SourceX), 0.0, 1.0);
			const usize Destination = (static_cast<usize>(Y) * ProjectThumbnailWidth + X) * 4U;
			for (uint32 Component = 0; Component < 4U; ++Component)
			{
				const float64 Top = std::lerp(static_cast<float64>(SourceComponent(X0, Y0, Component)),
											  static_cast<float64>(SourceComponent(X1, Y0, Component)), WeightX);
				const float64 Bottom = std::lerp(static_cast<float64>(SourceComponent(X0, Y1, Component)),
												 static_cast<float64>(SourceComponent(X1, Y1, Component)), WeightX);
				Result.Pixels[Destination + Component] =
					static_cast<uint8>(std::clamp(std::lround(std::lerp(Top, Bottom, WeightY)), 0L, 255L));
			}
		}
	}
	return Result;
}

[[nodiscard]] std::vector<uint8> EncodeThumbnailBitmap(const ProjectThumbnailImage &Image)
{
	if (!Image.IsValid())
		throw ProjectHubException("Cannot encode an invalid project thumbnail");
	const uint64 PixelBytes = static_cast<uint64>(Image.Width) * Image.Height * 4U;
	constexpr uint32 HeaderBytes = 54U;
	if (PixelBytes > std::numeric_limits<uint32>::max() - HeaderBytes)
		throw ProjectHubException("Project thumbnail exceeds the bitmap file-size limit");
	std::vector<uint8> Result(static_cast<usize>(HeaderBytes + PixelBytes), 0U);
	Result[0] = 'B';
	Result[1] = 'M';
	WriteUInt32(Result, 2U, static_cast<uint32>(Result.size()));
	WriteUInt32(Result, 10U, HeaderBytes);
	WriteUInt32(Result, 14U, 40U);
	WriteUInt32(Result, 18U, Image.Width);
	WriteUInt32(Result, 22U, Image.Height);
	WriteUInt16(Result, 26U, 1U);
	WriteUInt16(Result, 28U, 32U);
	WriteUInt32(Result, 34U, static_cast<uint32>(PixelBytes));
	for (uint32 Y = 0; Y < Image.Height; ++Y)
	{
		const uint32 SourceY = Image.Height - 1U - Y;
		for (uint32 X = 0; X < Image.Width; ++X)
		{
			const usize Source = (static_cast<usize>(SourceY) * Image.Width + X) * 4U;
			const usize Destination = HeaderBytes + (static_cast<usize>(Y) * Image.Width + X) * 4U;
			Result[Destination] = Image.Pixels[Source + 2U];
			Result[Destination + 1U] = Image.Pixels[Source + 1U];
			Result[Destination + 2U] = Image.Pixels[Source];
			Result[Destination + 3U] = Image.Pixels[Source + 3U];
		}
	}
	return Result;
}

[[nodiscard]] ProjectThumbnailImage DecodeThumbnailBitmap(const std::span<const uint8> Bytes)
{
	if (Bytes.size() < 54U || Bytes[0] != 'B' || Bytes[1] != 'M' || ReadUInt32(Bytes, 14U) < 40U || ReadUInt16(Bytes, 26U) != 1U ||
		ReadUInt16(Bytes, 28U) != 32U || ReadUInt32(Bytes, 30U) != 0U)
	{
		throw ProjectHubException("Project thumbnail is not an uncompressed 32-bit bitmap");
	}
	const uint32 Width = ReadUInt32(Bytes, 18U);
	const int32 SignedHeight = static_cast<int32>(ReadUInt32(Bytes, 22U));
	if (Width == 0 || SignedHeight == 0 || SignedHeight == std::numeric_limits<int32>::min())
		throw ProjectHubException("Project thumbnail bitmap has an invalid extent");
	const uint32 Height = static_cast<uint32>(SignedHeight < 0 ? -SignedHeight : SignedHeight);
	const uint64 PixelBytes = static_cast<uint64>(Width) * Height * 4U;
	const uint32 PixelOffset = ReadUInt32(Bytes, 10U);
	if (PixelBytes > std::numeric_limits<usize>::max() || PixelOffset > Bytes.size() || PixelBytes > Bytes.size() - PixelOffset)
		throw ProjectHubException("Project thumbnail bitmap pixels are truncated");
	ProjectThumbnailImage Result{.Width = Width, .Height = Height};
	Result.Pixels.resize(static_cast<usize>(PixelBytes));
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		const uint32 SourceY = SignedHeight > 0 ? Height - 1U - Y : Y;
		for (uint32 X = 0; X < Width; ++X)
		{
			const usize Source = PixelOffset + (static_cast<usize>(SourceY) * Width + X) * 4U;
			const usize Destination = (static_cast<usize>(Y) * Width + X) * 4U;
			Result.Pixels[Destination] = Bytes[Source + 2U];
			Result.Pixels[Destination + 1U] = Bytes[Source + 1U];
			Result.Pixels[Destination + 2U] = Bytes[Source];
			Result.Pixels[Destination + 3U] = Bytes[Source + 3U];
		}
	}
	return Result;
}

void SaveThumbnail(const std::filesystem::path &StateRoot, const std::filesystem::path &RelativePath, const ProjectThumbnailImage &Image)
{
	core::io::SecurePath::CreateDirectoriesWithin(StateRoot, RelativePath.parent_path(), "project thumbnail directory");
	const std::vector<uint8> Encoded = EncodeThumbnailBitmap(Image);
	const std::filesystem::path Temporary =
		RelativePath.parent_path() / (RelativePath.stem().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString() + ".bmp");
	core::io::SecurePath::WriteFileWithin(StateRoot, Temporary, Encoded, false, true, "project thumbnail temporary file");
	core::io::SecurePath::ReplaceWithin(StateRoot, Temporary, RelativePath, "project thumbnail publication");
}

[[nodiscard]] uint64 ThumbnailRevision(const std::filesystem::path &Path) noexcept
{
	std::error_code Error;
	const uint64 Size = std::filesystem::file_size(Path, Error);
	if (Error)
		return 0;
	const int64 Write = LastWriteMilliseconds(Path);
	return Size ^ (static_cast<uint64>(Write) + 0x9E3779B97F4A7C15ULL + (Size << 6U) + (Size >> 2U));
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
	Instances.push_back(Instance(LightID, instance::class_ids::DirectionalLight, "DirectionalLight", "Sun", WorkspaceID, 2, false,
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

bool ProjectThumbnailImage::IsValid() const noexcept
{
	const uint64 PixelCount = static_cast<uint64>(this->Width) * this->Height;
	return this->Width != 0 && this->Height != 0 && PixelCount <= std::numeric_limits<usize>::max() / 4U &&
		   this->Pixels.size() == static_cast<usize>(PixelCount * 4U);
}

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
		this->RefreshRecentProject(Recent);
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

void ProjectHub::MarkProjectEdited(const ProjectDescriptor &Descriptor)
{
	const std::filesystem::path Normal = std::filesystem::absolute(Descriptor.DescriptorPath).lexically_normal();
	auto Found = std::ranges::find(this->RecentProjects, Normal, &RecentProject::DescriptorPath);
	if (Found == this->RecentProjects.end())
	{
		this->RecordRecent(Descriptor);
		Found = std::ranges::find(this->RecentProjects, Normal, &RecentProject::DescriptorPath);
	}
	if (Found == this->RecentProjects.end())
		throw ProjectHubException("Edited project could not be published to the recent-project list");
	Found->LastEditedMilliseconds =
		std::max({Found->LastEditedMilliseconds, LatestAuthoredWriteMilliseconds(Descriptor), CurrentTimeMilliseconds()});
	this->SaveRecentProjects();
}

void ProjectHub::UpdateProjectThumbnail(const ProjectDescriptor &Descriptor, const uint32 SourceWidth, const uint32 SourceHeight,
										const std::span<const uint8> SourcePixels, const bool SourceRowsAreBottomUp)
{
	const std::filesystem::path Normal = std::filesystem::absolute(Descriptor.DescriptorPath).lexically_normal();
	auto Found = std::ranges::find(this->RecentProjects, Normal, &RecentProject::DescriptorPath);
	if (Found == this->RecentProjects.end())
	{
		this->RecordRecent(Descriptor);
		Found = std::ranges::find(this->RecentProjects, Normal, &RecentProject::DescriptorPath);
	}
	if (Found == this->RecentProjects.end())
		throw ProjectHubException("Project thumbnail could not resolve its recent-project record");
	Found->ID = Descriptor.ID;
	Found->Name = Descriptor.Name;
	const std::filesystem::path RelativePath = ThumbnailRelativePath(*Found);
	const ProjectThumbnailImage Thumbnail = ResizeThumbnail(SourceWidth, SourceHeight, SourcePixels, SourceRowsAreBottomUp);
	SaveThumbnail(this->StateRoot, RelativePath, Thumbnail);
	Found->ThumbnailPath = this->StateRoot / RelativePath;
	Found->ThumbnailRevision = ThumbnailRevision(Found->ThumbnailPath);
	Found->Thumbnail = Thumbnail;
	this->SaveRecentProjects();
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
		const uint32 FormatVersion = Root.value("FormatVersion", uint32{0});
		if (!Root.is_object() || (FormatVersion != 1U && FormatVersion != RecentProjectsFormatVersion) || !Root.contains("Projects") ||
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
			util::UUID ID;
			if (Entry.contains("ID") && Entry["ID"].is_string())
				ID = util::UUID::TryParse(Entry["ID"].get_ref<const string &>()).value_or(util::UUID{});
			this->RecentProjects.push_back({.ID = ID,
											.Name = Entry["Name"].get<string>(),
											.DescriptorPath = Path,
											.LastOpenedMilliseconds = Entry.value("LastOpenedMilliseconds", int64{0}),
											.LastEditedMilliseconds = Entry.value("LastEditedMilliseconds", int64{0})});
			try
			{
				this->RefreshRecentProject(this->RecentProjects.back());
			}
			catch (...)
			{
				RecentProject &Recent = this->RecentProjects.back();
				Recent.Thumbnail = CreateDefaultThumbnail(Recent);
			}
			if (this->RecentProjects.size() == MaximumRecentProjects)
				break;
		}
		if (FormatVersion != RecentProjectsFormatVersion)
			this->SaveRecentProjects();
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
		Projects.push_back({{"ID", Recent.ID.IsValid() ? Recent.ID.ToString() : string{}},
							{"Name", Recent.Name},
							{"Path", Recent.DescriptorPath.generic_string()},
							{"LastOpenedMilliseconds", Recent.LastOpenedMilliseconds},
							{"LastEditedMilliseconds", Recent.LastEditedMilliseconds}});
	const string Serialized = Json{{"FormatVersion", RecentProjectsFormatVersion}, {"Projects", std::move(Projects)}}.dump(2) + '\n';
	const std::filesystem::path Temporary = "RecentProjects.tmp-" + util::UUID::GenerateRandomUUID().ToString() + ".json";
	WriteTextFile(this->StateRoot, Temporary, Serialized, "recent projects temporary file");
	core::io::SecurePath::ReplaceWithin(this->StateRoot, Temporary, "RecentProjects.json", "recent projects publication");
}

void ProjectHub::RecordRecent(const ProjectDescriptor &Descriptor)
{
	const std::filesystem::path Normal = std::filesystem::absolute(Descriptor.DescriptorPath).lexically_normal();
	std::erase_if(this->RecentProjects, [&Normal](const RecentProject &Recent) { return Recent.DescriptorPath == Normal; });
	this->RecentProjects.insert(this->RecentProjects.begin(), {.ID = Descriptor.ID,
															   .Name = Descriptor.Name,
															   .DescriptorPath = Normal,
															   .LastOpenedMilliseconds = CurrentTimeMilliseconds(),
															   .LastEditedMilliseconds = LatestAuthoredWriteMilliseconds(Descriptor),
															   .Available = true});
	this->RefreshRecentProject(this->RecentProjects.front(), &Descriptor);
	if (this->RecentProjects.size() > MaximumRecentProjects)
		this->RecentProjects.resize(MaximumRecentProjects);
	this->SaveRecentProjects();
}

void ProjectHub::RefreshRecentProject(RecentProject &Recent, const ProjectDescriptor *const KnownDescriptor)
{
	const bool WasAvailable = Recent.Available;
	std::error_code Error;
	Recent.Available = std::filesystem::is_regular_file(Recent.DescriptorPath, Error) && !Error;
	std::optional<ProjectDescriptor> LoadedDescriptor;
	const ProjectDescriptor *Descriptor = KnownDescriptor;
	if (Descriptor == nullptr && Recent.Available && (!WasAvailable || !Recent.ID.IsValid()))
	{
		try
		{
			LoadedDescriptor = serialization::ProjectDescriptorSerializer::Load(Recent.DescriptorPath);
			Descriptor = &*LoadedDescriptor;
		}
		catch (...)
		{
			Recent.Available = false;
		}
	}
	if (Descriptor != nullptr)
	{
		Recent.ID = Descriptor->ID;
		Recent.Name = Descriptor->Name;
		Recent.LastEditedMilliseconds = std::max(Recent.LastEditedMilliseconds, LatestAuthoredWriteMilliseconds(*Descriptor));
	}

	const std::filesystem::path RelativePath = ThumbnailRelativePath(Recent);
	Recent.ThumbnailPath = this->StateRoot / RelativePath;
	const bool ThumbnailExists = std::filesystem::is_regular_file(Recent.ThumbnailPath, Error) && !Error;
	if (!ThumbnailExists)
		SaveThumbnail(this->StateRoot, RelativePath, CreateDefaultThumbnail(Recent));
	const uint64 Revision = ThumbnailRevision(Recent.ThumbnailPath);
	if (Revision == 0)
		throw ProjectHubException("Project thumbnail has no readable file revision");
	if (Recent.ThumbnailRevision != Revision || !Recent.Thumbnail.IsValid())
	{
		try
		{
			const std::vector<uint8> Bytes =
				core::io::SecurePath::ReadFileWithin(this->StateRoot, RelativePath, MaximumProjectThumbnailBytes, "project thumbnail");
			Recent.Thumbnail = DecodeThumbnailBitmap(Bytes);
		}
		catch (...)
		{
			Recent.Thumbnail = CreateDefaultThumbnail(Recent);
			SaveThumbnail(this->StateRoot, RelativePath, Recent.Thumbnail);
		}
		Recent.ThumbnailRevision = ThumbnailRevision(Recent.ThumbnailPath);
	}
}
} // namespace editor::project
