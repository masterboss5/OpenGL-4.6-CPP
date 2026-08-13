#include "PrimitiveMeshFactory.h"

#include "Source/resource/asset/AssetManager.h"
#include "Source/resource/asset/MaterialAsset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace editor::asset
{
namespace
{
struct PrimitiveVertex final
{
	glm::vec3 Position{0.0f};
	glm::vec3 Normal{0.0f, 1.0f, 0.0f};
	glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
	glm::vec2 TextureCoordinate{0.0f};
};

struct PrimitiveGeometry final
{
	std::vector<PrimitiveVertex> Vertices;
	std::vector<uint32> Indices;
	resource::Bounds Bounds;
};

constexpr resource::MaterialSlotID DefaultMaterialSlot = 1;
constexpr resource::MeshSectionID DefaultSection = 1;
constexpr resource::ModelNodeID DefaultNode = 1;
constexpr resource::ModelMeshInstanceID DefaultMeshInstance = 1;
constexpr string_view DefaultMaterialID = "00000000-0000-4000-8000-000000000001";
constexpr std::array<string_view, static_cast<usize>(PrimitiveShape::Count)> MeshIDs{
	"00000000-0000-4000-8000-000000000101", "00000000-0000-4000-8000-000000000102", "00000000-0000-4000-8000-000000000103",
	"00000000-0000-4000-8000-000000000104", "00000000-0000-4000-8000-000000000105", "00000000-0000-4000-8000-000000000106"};
constexpr std::array<string_view, static_cast<usize>(PrimitiveShape::Count)> ModelIDs{
	"00000000-0000-4000-8000-000000000201", "00000000-0000-4000-8000-000000000202", "00000000-0000-4000-8000-000000000203",
	"00000000-0000-4000-8000-000000000204", "00000000-0000-4000-8000-000000000205", "00000000-0000-4000-8000-000000000206"};

template <typename T>
	requires std::is_trivially_copyable_v<T>
[[nodiscard]] std::vector<uint8> CopyBytes(const std::span<const T> Values)
{
	std::vector<uint8> Bytes(Values.size_bytes());
	if (!Bytes.empty())
		std::memcpy(Bytes.data(), Values.data(), Bytes.size());
	return Bytes;
}

[[nodiscard]] resource::Bounds MakeBounds(const glm::vec3 Minimum, const glm::vec3 Maximum)
{
	const glm::vec3 Center = (Minimum + Maximum) * 0.5f;
	return {.Minimum = Minimum, .Maximum = Maximum, .Sphere = glm::vec4(Center, glm::length(Maximum - Center))};
}

void AddQuad(PrimitiveGeometry &Geometry, const glm::vec3 A, const glm::vec3 B, const glm::vec3 C, const glm::vec3 D,
			 const glm::vec3 Normal, const glm::vec4 Tangent)
{
	const uint32 Base = static_cast<uint32>(Geometry.Vertices.size());
	Geometry.Vertices.insert(Geometry.Vertices.end(), {{A, Normal, Tangent, {0.0f, 0.0f}},
													   {B, Normal, Tangent, {1.0f, 0.0f}},
													   {C, Normal, Tangent, {1.0f, 1.0f}},
													   {D, Normal, Tangent, {0.0f, 1.0f}}});
	Geometry.Indices.insert(Geometry.Indices.end(), {Base, Base + 1U, Base + 2U, Base, Base + 2U, Base + 3U});
}

[[nodiscard]] PrimitiveGeometry BuildBox()
{
	PrimitiveGeometry Result;
	AddQuad(Result, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f},
			{1.0f, 0.0f, 0.0f, 1.0f});
	AddQuad(Result, {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f},
			{-1.0f, 0.0f, 0.0f, 1.0f});
	AddQuad(Result, {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 1.0f, 1.0f});
	AddQuad(Result, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, -1.0f, 1.0f});
	AddQuad(Result, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f},
			{1.0f, 0.0f, 0.0f, 1.0f});
	AddQuad(Result, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f},
			{1.0f, 0.0f, 0.0f, 1.0f});
	Result.Bounds = MakeBounds(glm::vec3(-0.5f), glm::vec3(0.5f));
	return Result;
}

[[nodiscard]] PrimitiveGeometry BuildPlane()
{
	PrimitiveGeometry Result;
	AddQuad(Result, {-0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, -0.5f}, {0.0f, 1.0f, 0.0f},
			{1.0f, 0.0f, 0.0f, 1.0f});
	Result.Bounds = MakeBounds({-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f});
	return Result;
}

[[nodiscard]] PrimitiveGeometry BuildUVSurface(const uint32 Segments, const uint32 Rings, const bool Capsule)
{
	PrimitiveGeometry Result;
	Result.Vertices.reserve(static_cast<usize>(Segments + 1U) * (Rings + 1U));
	for (uint32 Ring = 0; Ring <= Rings; ++Ring)
	{
		const float32 V = static_cast<float32>(Ring) / static_cast<float32>(Rings);
		const float32 Latitude = -std::numbers::pi_v<float32> * 0.5f + V * std::numbers::pi_v<float32>;
		const float32 CosLatitude = std::cos(Latitude);
		const float32 SinLatitude = std::sin(Latitude);
		const float32 CapsuleOffset = Capsule ? (SinLatitude < 0.0f ? -0.5f : 0.5f) : 0.0f;
		for (uint32 Segment = 0; Segment <= Segments; ++Segment)
		{
			const float32 U = static_cast<float32>(Segment) / static_cast<float32>(Segments);
			const float32 Longitude = U * std::numbers::pi_v<float32> * 2.0f;
			const glm::vec3 Normal{CosLatitude * std::cos(Longitude), SinLatitude, CosLatitude * std::sin(Longitude)};
			const glm::vec3 Position = Normal * 0.5f + glm::vec3(0.0f, CapsuleOffset, 0.0f);
			Result.Vertices.push_back({.Position = Position,
									   .Normal = Normal,
									   .Tangent = glm::vec4(-std::sin(Longitude), 0.0f, std::cos(Longitude), 1.0f),
									   .TextureCoordinate = {U, 1.0f - V}});
		}
	}
	for (uint32 Ring = 0; Ring < Rings; ++Ring)
	{
		for (uint32 Segment = 0; Segment < Segments; ++Segment)
		{
			const uint32 First = Ring * (Segments + 1U) + Segment;
			const uint32 Second = First + Segments + 1U;
			Result.Indices.insert(Result.Indices.end(), {First, Second, First + 1U, First + 1U, Second, Second + 1U});
		}
	}
	Result.Bounds = Capsule ? MakeBounds({-0.5f, -1.0f, -0.5f}, {0.5f, 1.0f, 0.5f}) : MakeBounds(glm::vec3(-0.5f), glm::vec3(0.5f));
	return Result;
}

[[nodiscard]] PrimitiveGeometry BuildCylinder(const uint32 Segments, const bool Cone)
{
	PrimitiveGeometry Result;
	const float32 TopRadius = Cone ? 0.0f : 0.5f;
	for (uint32 Segment = 0; Segment <= Segments; ++Segment)
	{
		const float32 U = static_cast<float32>(Segment) / static_cast<float32>(Segments);
		const float32 Angle = U * std::numbers::pi_v<float32> * 2.0f;
		const float32 Cosine = std::cos(Angle);
		const float32 Sine = std::sin(Angle);
		const glm::vec3 SideNormal = glm::normalize(glm::vec3(Cosine, Cone ? 0.5f : 0.0f, Sine));
		const glm::vec4 Tangent{-Sine, 0.0f, Cosine, 1.0f};
		Result.Vertices.push_back({{Cosine * 0.5f, -0.5f, Sine * 0.5f}, SideNormal, Tangent, {U, 1.0f}});
		Result.Vertices.push_back({{Cosine * TopRadius, 0.5f, Sine * TopRadius}, SideNormal, Tangent, {U, 0.0f}});
	}
	for (uint32 Segment = 0; Segment < Segments; ++Segment)
	{
		const uint32 Base = Segment * 2U;
		if (Cone)
			Result.Indices.insert(Result.Indices.end(), {Base, Base + 1U, Base + 2U});
		else
			Result.Indices.insert(Result.Indices.end(), {Base, Base + 1U, Base + 2U, Base + 1U, Base + 3U, Base + 2U});
	}

	const auto AddCap = [&Result, Segments](const float32 Y, const float32 Radius, const bool Top)
	{
		if (Radius == 0.0f)
			return;
		const uint32 Center = static_cast<uint32>(Result.Vertices.size());
		const glm::vec3 Normal{0.0f, Top ? 1.0f : -1.0f, 0.0f};
		Result.Vertices.push_back({{0.0f, Y, 0.0f}, Normal, {1.0f, 0.0f, 0.0f, 1.0f}, {0.5f, 0.5f}});
		for (uint32 Segment = 0; Segment <= Segments; ++Segment)
		{
			const float32 Angle = static_cast<float32>(Segment) / static_cast<float32>(Segments) * std::numbers::pi_v<float32> * 2.0f;
			const float32 Cosine = std::cos(Angle);
			const float32 Sine = std::sin(Angle);
			Result.Vertices.push_back(
				{{Cosine * Radius, Y, Sine * Radius}, Normal, {1.0f, 0.0f, 0.0f, 1.0f}, {Cosine * 0.5f + 0.5f, Sine * 0.5f + 0.5f}});
		}
		for (uint32 Segment = 0; Segment < Segments; ++Segment)
		{
			if (Top)
				Result.Indices.insert(Result.Indices.end(), {Center, Center + Segment + 2U, Center + Segment + 1U});
			else
				Result.Indices.insert(Result.Indices.end(), {Center, Center + Segment + 1U, Center + Segment + 2U});
		}
	};
	AddCap(-0.5f, 0.5f, false);
	AddCap(0.5f, TopRadius, true);
	Result.Bounds = MakeBounds({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
	return Result;
}

[[nodiscard]] resource::MeshLOD MakeLOD(const PrimitiveGeometry &Geometry, const uint32 Level, const float32 ScreenCoverage)
{
	std::vector<glm::vec3> Positions;
	std::vector<glm::vec3> Normals;
	std::vector<glm::vec4> Tangents;
	std::vector<glm::vec2> TextureCoordinates;
	std::vector<glm::vec2> DefaultTextureCoordinates(Geometry.Vertices.size(), glm::vec2(0.0f));
	Positions.reserve(Geometry.Vertices.size());
	Normals.reserve(Geometry.Vertices.size());
	Tangents.reserve(Geometry.Vertices.size());
	TextureCoordinates.reserve(Geometry.Vertices.size());
	for (const PrimitiveVertex &Vertex : Geometry.Vertices)
	{
		Positions.push_back(Vertex.Position);
		Normals.push_back(Vertex.Normal);
		Tangents.push_back(Vertex.Tangent);
		TextureCoordinates.push_back(Vertex.TextureCoordinate);
	}
	const uint32 VertexCount = static_cast<uint32>(Geometry.Vertices.size());
	const uint32 IndexCount = static_cast<uint32>(Geometry.Indices.size());
	return {.Level = Level,
			.ScreenCoverage = ScreenCoverage,
			.Hysteresis = 0.03f,
			.Bounds = Geometry.Bounds,
			.VertexStreams = {{.Semantic = resource::MeshVertexSemantic::Position,
							   .Format = resource::MeshVertexFormat::Float32x3,
							   .Stride = sizeof(glm::vec3),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec3>(Positions)},
							  {.Semantic = resource::MeshVertexSemantic::Normal,
							   .Format = resource::MeshVertexFormat::Float32x3,
							   .Stride = sizeof(glm::vec3),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec3>(Normals)},
							  {.Semantic = resource::MeshVertexSemantic::Tangent,
							   .Format = resource::MeshVertexFormat::Float32x4,
							   .Stride = sizeof(glm::vec4),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec4>(Tangents)},
							  {.Semantic = resource::MeshVertexSemantic::TextureCoordinate,
							   .Format = resource::MeshVertexFormat::Float32x2,
							   .SemanticIndex = 0,
							   .Stride = sizeof(glm::vec2),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec2>(TextureCoordinates)},
							  {.Semantic = resource::MeshVertexSemantic::TextureCoordinate,
							   .Format = resource::MeshVertexFormat::Float32x2,
							   .SemanticIndex = 1,
							   .Stride = sizeof(glm::vec2),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec2>(DefaultTextureCoordinates)},
							  {.Semantic = resource::MeshVertexSemantic::TextureCoordinate,
							   .Format = resource::MeshVertexFormat::Float32x2,
							   .SemanticIndex = 2,
							   .Stride = sizeof(glm::vec2),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec2>(DefaultTextureCoordinates)},
							  {.Semantic = resource::MeshVertexSemantic::TextureCoordinate,
							   .Format = resource::MeshVertexFormat::Float32x2,
							   .SemanticIndex = 3,
							   .Stride = sizeof(glm::vec2),
							   .ElementCount = VertexCount,
							   .Bytes = CopyBytes<glm::vec2>(DefaultTextureCoordinates)}},
			.IndexStream = {.Format = resource::MeshIndexFormat::UInt32,
							.IndexCount = IndexCount,
							.Bytes = CopyBytes<uint32>(Geometry.Indices)},
			.Sections = {{.ID = DefaultSection,
						  .MaterialSlot = DefaultMaterialSlot,
						  .FirstIndex = 0,
						  .IndexCount = IndexCount,
						  .BaseVertex = 0,
						  .LocalBounds = Geometry.Bounds}}};
}

[[nodiscard]] std::vector<resource::MeshLOD> BuildLODs(const PrimitiveShape Shape)
{
	std::vector<resource::MeshLOD> Result;
	if (Shape == PrimitiveShape::Box || Shape == PrimitiveShape::Plane)
	{
		const PrimitiveGeometry Geometry = Shape == PrimitiveShape::Box ? BuildBox() : BuildPlane();
		Result.push_back(MakeLOD(Geometry, 0, 1.0f));
		return Result;
	}
	// Curved primitives are routinely inspected close-up in the editor and cast
	// equally enlarged silhouettes into shadow maps. Keep enough angular samples
	// in the source LOD that a full-height primitive remains subpixel-smooth in a
	// 4K viewport; lower LODs retain the inexpensive distant representations.
	constexpr std::array Segments{1024U, 256U, 64U, 32U};
	constexpr std::array Coverage{1.0f, 0.35f, 0.1f, 0.03f};
	for (uint32 Level = 0; Level < Segments.size(); ++Level)
	{
		PrimitiveGeometry Geometry;
		switch (Shape)
		{
		case PrimitiveShape::Sphere:
			Geometry = BuildUVSurface(Segments[Level], Segments[Level] / 2U, false);
			break;
		case PrimitiveShape::Capsule:
			Geometry = BuildUVSurface(Segments[Level], Segments[Level] / 2U, true);
			break;
		case PrimitiveShape::Cylinder:
			Geometry = BuildCylinder(Segments[Level], false);
			break;
		case PrimitiveShape::Cone:
			Geometry = BuildCylinder(Segments[Level], true);
			break;
		default:
			throw std::logic_error("Primitive shape does not have a curved LOD generator");
		}
		Result.push_back(MakeLOD(Geometry, Level, Coverage[Level]));
	}
	return Result;
}

[[nodiscard]] resource::Bounds BoundsFor(const PrimitiveShape Shape)
{
	return Shape == PrimitiveShape::Capsule ? MakeBounds({-0.5f, -1.0f, -0.5f}, {0.5f, 1.0f, 0.5f})
		   : Shape == PrimitiveShape::Plane ? MakeBounds({-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f})
											: MakeBounds(glm::vec3(-0.5f), glm::vec3(0.5f));
}
} // namespace

PrimitiveMeshFactory::PrimitiveMeshFactory(resource::AssetManager &Assets) : Assets(&Assets)
{
	this->DefaultMaterial = Assets.PublishGeneratedAsset<resource::MaterialAsset>(
		string(DefaultMaterialID), "__Generated/Materials/Default.material",
		resource::AssetPtr<resource::MaterialAsset>::Make(
			"Default", resource::MaterialPipelineContract{},
			resource::PBRMaterialFactors{.BaseColor = glm::vec4(0.58f, 0.60f, 0.64f, 1.0f), .Metallic = 0.0f, .Roughness = 0.48f},
			std::vector<resource::MaterialTextureBinding>{}));
}

resource::AssetHandle<resource::ModelAsset> PrimitiveMeshFactory::GetModel(const PrimitiveShape Shape)
{
	const usize Index = static_cast<usize>(Shape);
	if (Index >= this->Models.size())
		throw std::out_of_range("Primitive shape is invalid");
	if (!this->Models[Index])
		this->Models[Index] = this->Build(Shape);
	return this->Models[Index];
}

string_view PrimitiveMeshFactory::GetName(const PrimitiveShape Shape)
{
	constexpr std::array<string_view, static_cast<usize>(PrimitiveShape::Count)> Names{"Box",	   "Sphere", "Capsule",
																					   "Cylinder", "Cone",	 "Plane"};
	const usize Index = static_cast<usize>(Shape);
	if (Index >= Names.size())
		throw std::out_of_range("Primitive shape is invalid");
	return Names[Index];
}

resource::AssetID PrimitiveMeshFactory::GetModelID(const PrimitiveShape Shape)
{
	const usize Index = static_cast<usize>(Shape);
	if (Index >= ModelIDs.size())
		throw std::out_of_range("Primitive shape is invalid");
	return string(ModelIDs[Index]);
}

resource::AssetHandle<resource::ModelAsset> PrimitiveMeshFactory::Build(const PrimitiveShape Shape)
{
	const usize Index = static_cast<usize>(Shape);
	const string Name(PrimitiveMeshFactory::GetName(Shape));
	const resource::Bounds Bounds = BoundsFor(Shape);
	resource::MeshAssetData MeshData{
		.Name = Name,
		.Bounds = Bounds,
		.CPURetention = resource::MeshCPURetentionPolicy::RetainAll,
		.MaterialSlots = {{.ID = DefaultMaterialSlot,
						   .Name = "Default",
						   .DefaultMaterial = resource::AssetHandle<resource::MaterialInterfaceAsset>(this->DefaultMaterial)}},
		.LODs = BuildLODs(Shape),
		.DerivedDataKey = "PrimitiveMesh:v2:" + Name};
	resource::AssetHandle<resource::StaticMeshAsset> Mesh = this->Assets->PublishGeneratedAsset<resource::StaticMeshAsset>(
		string(MeshIDs[Index]), std::filesystem::path("__Generated/Primitives") / (Name + ".mesh"),
		resource::AssetPtr<resource::StaticMeshAsset>::Make(std::move(MeshData)), {this->DefaultMaterial.GetID()});
	return this->Assets->PublishGeneratedAsset<resource::ModelAsset>(
		PrimitiveMeshFactory::GetModelID(Shape), std::filesystem::path("__Generated/Primitives") / (Name + ".model"),
		resource::AssetPtr<resource::ModelAsset>::Make(
			Name, Bounds,
			std::vector<resource::ModelNode>{
				{.ID = DefaultNode, .Name = Name, .ParentIndex = resource::InvalidModelNodeIndex, .LocalTransform = glm::mat4(1.0f)}},
			std::vector<resource::ModelMeshInstance>{
				{.ID = DefaultMeshInstance, .NodeIndex = 0, .Mesh = resource::AssetHandle<resource::MeshAsset>(Mesh)}}),
		{Mesh.GetID()});
}
} // namespace editor::asset
