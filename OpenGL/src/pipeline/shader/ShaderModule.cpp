#include "ShaderModule.h"

#include "ShaderException.h"
#include "src/pipeline/device/Device.h"
#include "src/pipeline/render/RenderData.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace pipeline::shader
{
namespace
{
std::atomic<uint64> InterfaceQueryCount{0};
GLenum ToGLStage(ShaderStage Stage)
{
	switch (Stage)
	{
	case ShaderStage::Vertex:
		return GL_VERTEX_SHADER;
	case ShaderStage::Fragment:
		return GL_FRAGMENT_SHADER;
	case ShaderStage::Compute:
		return GL_COMPUTE_SHADER;
	case ShaderStage::Include:
		throw std::invalid_argument("Shader include sources cannot be compiled as standalone stages");
	}
	throw std::invalid_argument("Shader source has an invalid stage");
}
std::string Log(GLuint Object, bool Program)
{
	GLint Length = 0;
	if (Program)
		glGetProgramiv(Object, GL_INFO_LOG_LENGTH, &Length);
	else
		glGetShaderiv(Object, GL_INFO_LOG_LENGTH, &Length);
	std::vector<GLchar> Buffer(static_cast<usize>(Length > 1 ? Length : 1));
	if (Program)
		glGetProgramInfoLog(Object, Length, nullptr, Buffer.data());
	else
		glGetShaderInfoLog(Object, Length, nullptr, Buffer.data());
	return std::string(Buffer.data());
}

struct StorageBlockContract final
{
	const char *Name;
	GLuint Binding;
	GLint RecordStride;
};

[[nodiscard]] bool IsMember(const std::string_view Name, const std::string_view Member) noexcept
{
	return Name == Member || (Name.size() > Member.size() && Name.ends_with(Member) && Name[Name.size() - Member.size() - 1U] == '.');
}

[[nodiscard]] bool IsDirectArray(const std::string_view Name, const std::string_view Member) noexcept
{
	return Name == Member || (Name.size() == Member.size() + 3U && Name.starts_with(Member) && Name.substr(Member.size()) == "[0]");
}

[[nodiscard]] std::optional<GLint> ExpectedStorageOffset(const std::string_view Block, const std::string_view Name)
{
#define MEMBER_OFFSET(BlockName, MemberName, Type, Field)                                                                                  \
	if (Block == BlockName && IsMember(Name, MemberName))                                                                                  \
	return static_cast<GLint>(offsetof(Type, Field))
	MEMBER_OFFSET("InstanceData", "transform", pipeline::render::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("VisibleInstances", "transform", pipeline::render::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("CandidateInstances", "transform", pipeline::render::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("InstanceData", "previousTransform", pipeline::render::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("VisibleInstances", "previousTransform", pipeline::render::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("CandidateInstances", "previousTransform", pipeline::render::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("InstanceData", "worldBounds", pipeline::render::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("VisibleInstances", "worldBounds", pipeline::render::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("CandidateInstances", "worldBounds", pipeline::render::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("InstanceData", "materialIndex", pipeline::render::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("VisibleInstances", "materialIndex", pipeline::render::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("CandidateInstances", "materialIndex", pipeline::render::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("InstanceData", "objectID", pipeline::render::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("VisibleInstances", "objectID", pipeline::render::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("CandidateInstances", "objectID", pipeline::render::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("InstanceData", "batchIndex", pipeline::render::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("VisibleInstances", "batchIndex", pipeline::render::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("CandidateInstances", "batchIndex", pipeline::render::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("InstanceData", "skinPaletteOffset", pipeline::render::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("VisibleInstances", "skinPaletteOffset", pipeline::render::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("CandidateInstances", "skinPaletteOffset", pipeline::render::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("InstanceData", "previousSkinPaletteOffset", pipeline::render::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("VisibleInstances", "previousSkinPaletteOffset", pipeline::render::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("CandidateInstances", "previousSkinPaletteOffset", pipeline::render::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("InstanceData", "flags", pipeline::render::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("VisibleInstances", "flags", pipeline::render::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("CandidateInstances", "flags", pipeline::render::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("InstanceData", "morphWeightOffset", pipeline::render::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("VisibleInstances", "morphWeightOffset", pipeline::render::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("CandidateInstances", "morphWeightOffset", pipeline::render::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("InstanceData", "morphWeightCount", pipeline::render::GPUInstanceRecord, MorphWeightCount);
	MEMBER_OFFSET("VisibleInstances", "morphWeightCount", pipeline::render::GPUInstanceRecord, MorphWeightCount);
	MEMBER_OFFSET("CandidateInstances", "morphWeightCount", pipeline::render::GPUInstanceRecord, MorphWeightCount);

	MEMBER_OFFSET("Materials", "baseColorTexture", pipeline::render::GPUMaterialRecord, BaseColorTexture);
	MEMBER_OFFSET("Materials", "normalTexture", pipeline::render::GPUMaterialRecord, NormalTexture);
	MEMBER_OFFSET("Materials", "metallicRoughnessTexture", pipeline::render::GPUMaterialRecord, MetallicRoughnessTexture);
	MEMBER_OFFSET("Materials", "occlusionTexture", pipeline::render::GPUMaterialRecord, OcclusionTexture);
	MEMBER_OFFSET("Materials", "emissiveTexture", pipeline::render::GPUMaterialRecord, EmissiveTexture);
	MEMBER_OFFSET("Materials", "specularTexture", pipeline::render::GPUMaterialRecord, SpecularTexture);
	MEMBER_OFFSET("Materials", "transmissionTexture", pipeline::render::GPUMaterialRecord, TransmissionTexture);
	MEMBER_OFFSET("Materials", "textureCoordinateSelectors", pipeline::render::GPUMaterialRecord, TextureCoordinateSelectors);
	MEMBER_OFFSET("Materials", "baseColorFactor", pipeline::render::GPUMaterialRecord, BaseColorFactor);
	MEMBER_OFFSET("Materials", "emissiveAndMetallic", pipeline::render::GPUMaterialRecord, EmissiveAndMetallic);
	MEMBER_OFFSET("Materials", "roughnessTransmissionIor", pipeline::render::GPUMaterialRecord, RoughnessTransmissionIOR);
	MEMBER_OFFSET("Materials", "textureControls", pipeline::render::GPUMaterialRecord, TextureControls);

	MEMBER_OFFSET("Lights", "positionAndRange", pipeline::render::GPULightRecord, PositionAndRange);
	MEMBER_OFFSET("Lights", "directionAndType", pipeline::render::GPULightRecord, DirectionAndType);
	MEMBER_OFFSET("Lights", "colorAndIntensity", pipeline::render::GPULightRecord, ColorAndIntensity);
	MEMBER_OFFSET("Lights", "spotAnglesAndShadow", pipeline::render::GPULightRecord, SpotAnglesAndShadow);
	MEMBER_OFFSET("ClusterHeaders", "offset", pipeline::render::GPUClusterHeader, Offset);
	MEMBER_OFFSET("ClusterHeaders", "count", pipeline::render::GPUClusterHeader, Count);
	MEMBER_OFFSET("ClusterHeaders", "pad0", pipeline::render::GPUClusterHeader, Pad0);
	MEMBER_OFFSET("ClusterHeaders", "pad1", pipeline::render::GPUClusterHeader, Pad1);
	MEMBER_OFFSET("ShadowData", "viewProjection", pipeline::render::GPUShadowRecord, ViewProjection);
	MEMBER_OFFSET("ShadowData", "atlasScaleBias", pipeline::render::GPUShadowRecord, AtlasScaleBias);
	MEMBER_OFFSET("ShadowData", "depthBiasAndFilter", pipeline::render::GPUShadowRecord, DepthBiasAndFilter);
	MEMBER_OFFSET("SkinMatrices", "current", pipeline::render::GPUSkinMatrixRecord, Current);
	MEMBER_OFFSET("SkinMatrices", "previous", pipeline::render::GPUSkinMatrixRecord, Previous);
	MEMBER_OFFSET("MorphDeltas", "positionDelta", pipeline::render::GPUMorphDeltaRecord, PositionDelta);
	MEMBER_OFFSET("MorphDeltas", "normalDelta", pipeline::render::GPUMorphDeltaRecord, NormalDelta);
	MEMBER_OFFSET("MorphWeights", "deltaOffset", pipeline::render::GPUMorphWeightRecord, DeltaOffset);
	MEMBER_OFFSET("MorphWeights", "currentWeight", pipeline::render::GPUMorphWeightRecord, CurrentWeight);
	MEMBER_OFFSET("MorphWeights", "previousWeight", pipeline::render::GPUMorphWeightRecord, PreviousWeight);
	MEMBER_OFFSET("MorphWeights", "padding", pipeline::render::GPUMorphWeightRecord, Padding);
	if (Block == "DebugLines")
	{
		if (IsMember(Name, "startAndWidth"))
			return 0;
		if (IsMember(Name, "end"))
			return static_cast<GLint>(sizeof(glm::vec4));
		if (IsMember(Name, "color"))
			return static_cast<GLint>(sizeof(glm::vec4) * 2U);
	}
#undef MEMBER_OFFSET
	if ((Block == "ClusterIndices" && IsDirectArray(Name, "indices")) || (Block == "VisibilityScratch" && IsDirectArray(Name, "scratch")) ||
		(Block == "SelectionMask" && IsDirectArray(Name, "selectionWords")))
		return 0;
	if (Block == "IndirectCommands")
	{
		if (IsMember(Name, "indexCount"))
			return 0;
		if (IsMember(Name, "instanceCount"))
			return 4;
		if (IsMember(Name, "firstIndex"))
			return 8;
		if (IsMember(Name, "baseVertex"))
			return 12;
		if (IsMember(Name, "baseInstance"))
			return 16;
	}
	return std::nullopt;
}

void ValidateStorageLayout(const GLuint Program, const StorageBlockContract &Contract, const GLuint Block, const ShaderStage Stage,
						   const std::filesystem::path &Path, const ShaderPermutationKey &Permutation)
{
	constexpr std::array<GLenum, 2> BlockProperties{GL_NUM_ACTIVE_VARIABLES, GL_BUFFER_DATA_SIZE};
	std::array<GLint, 2> BlockValues{};
	glGetProgramResourceiv(Program, GL_SHADER_STORAGE_BLOCK, Block, static_cast<GLsizei>(BlockProperties.size()), BlockProperties.data(),
						   static_cast<GLsizei>(BlockValues.size()), nullptr, BlockValues.data());
	if (BlockValues[0] <= 0)
		throw ShaderInterfaceException(Stage, Path, Permutation, std::string(Contract.Name) + " exposes no active buffer variables");
	if (BlockValues[1] != 0 && BlockValues[1] < Contract.RecordStride)
		throw ShaderInterfaceException(Stage, Path, Permutation, std::string(Contract.Name) + " has an undersized storage layout");

	constexpr GLenum ActiveVariablesProperty = GL_ACTIVE_VARIABLES;
	std::vector<GLint> Variables(static_cast<usize>(BlockValues[0]));
	glGetProgramResourceiv(Program, GL_SHADER_STORAGE_BLOCK, Block, 1, &ActiveVariablesProperty, BlockValues[0], nullptr, Variables.data());
	constexpr std::array<GLenum, 5> VariableProperties{GL_NAME_LENGTH, GL_OFFSET, GL_ARRAY_STRIDE, GL_MATRIX_STRIDE,
													   GL_TOP_LEVEL_ARRAY_STRIDE};
	for (const GLint Variable : Variables)
	{
		std::array<GLint, 5> Values{};
		glGetProgramResourceiv(Program, GL_BUFFER_VARIABLE, static_cast<GLuint>(Variable), static_cast<GLsizei>(VariableProperties.size()),
							   VariableProperties.data(), static_cast<GLsizei>(Values.size()), nullptr, Values.data());
		std::string Name(static_cast<usize>(std::max(Values[0], 1)), '\0');
		GLsizei Written = 0;
		glGetProgramResourceName(Program, GL_BUFFER_VARIABLE, static_cast<GLuint>(Variable), Values[0], &Written, Name.data());
		Name.resize(static_cast<usize>(Written));
		const auto ExpectedOffset = ExpectedStorageOffset(Contract.Name, Name);
		if (!ExpectedOffset || Values[1] != *ExpectedOffset)
			throw ShaderInterfaceException(Stage, Path, Permutation, std::string(Contract.Name) + " has incompatible member " + Name);
		const GLint ReflectedStride = Values[4] > 0 ? Values[4] : Values[2];
		if (ReflectedStride > 0 && ReflectedStride != Contract.RecordStride)
			throw ShaderInterfaceException(Stage, Path, Permutation, std::string(Contract.Name) + " has an incompatible record stride");
		if (Values[3] > 0 && Values[3] != static_cast<GLint>(sizeof(glm::vec4)))
			throw ShaderInterfaceException(Stage, Path, Permutation, std::string(Contract.Name) + " has an incompatible matrix stride");
	}
}

[[nodiscard]] bool ValidateFrameConstants(const GLuint Program, const ShaderStage Stage, const std::filesystem::path &Path,
										  const ShaderPermutationKey &Permutation)
{
	const GLuint Block = glGetProgramResourceIndex(Program, GL_UNIFORM_BLOCK, "FrameConstants");
	if (Block == GL_INVALID_INDEX)
		return false;
	constexpr std::array<GLenum, 3> Properties{GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES};
	std::array<GLint, 3> Values{};
	glGetProgramResourceiv(Program, GL_UNIFORM_BLOCK, Block, static_cast<GLsizei>(Properties.size()), Properties.data(),
						   static_cast<GLsizei>(Values.size()), nullptr, Values.data());
	if (Values[0] != static_cast<GLint>(pipeline::render::RendererBinding::FrameConstants) ||
		Values[1] != static_cast<GLint>(sizeof(pipeline::render::GPUFrameConstants)))
		throw ShaderInterfaceException(Stage, Path, Permutation, "FrameConstants binding or std140 size is incompatible");
	constexpr GLenum ActiveVariablesProperty = GL_ACTIVE_VARIABLES;
	std::vector<GLint> Variables(static_cast<usize>(Values[2]));
	glGetProgramResourceiv(Program, GL_UNIFORM_BLOCK, Block, 1, &ActiveVariablesProperty, Values[2], nullptr, Variables.data());
	constexpr std::array<GLenum, 3> VariableProperties{GL_NAME_LENGTH, GL_OFFSET, GL_MATRIX_STRIDE};
	for (const GLint Variable : Variables)
	{
		std::array<GLint, 3> MemberValues{};
		glGetProgramResourceiv(Program, GL_UNIFORM, static_cast<GLuint>(Variable), static_cast<GLsizei>(VariableProperties.size()),
							   VariableProperties.data(), static_cast<GLsizei>(MemberValues.size()), nullptr, MemberValues.data());
		std::string Name(static_cast<usize>(std::max(MemberValues[0], 1)), '\0');
		GLsizei Written = 0;
		glGetProgramResourceName(Program, GL_UNIFORM, static_cast<GLuint>(Variable), MemberValues[0], &Written, Name.data());
		Name.resize(static_cast<usize>(Written));
		std::optional<GLint> Expected;
#define FRAME_OFFSET(MemberName, Field)                                                                                                    \
	if (IsMember(Name, MemberName))                                                                                                        \
	Expected = static_cast<GLint>(offsetof(pipeline::render::GPUFrameConstants, Field))
		FRAME_OFFSET("projection", Projection);
		FRAME_OFFSET("view", View);
		FRAME_OFFSET("viewProjection", ViewProjection);
		FRAME_OFFSET("previousViewProjection", PreviousViewProjection);
		FRAME_OFFSET("inverseViewProjection", InverseViewProjection);
		FRAME_OFFSET("cameraPositionAndNear", CameraPositionAndNear);
		FRAME_OFFSET("renderExtentAndFar", RenderExtentAndFar);
		FRAME_OFFSET("countsAndFrame", CountsAndFrame);
		FRAME_OFFSET("backgroundColor", BackgroundColor);
#undef FRAME_OFFSET
		if (!Expected || MemberValues[1] != *Expected || (MemberValues[2] > 0 && MemberValues[2] != static_cast<GLint>(sizeof(glm::vec4))))
			throw ShaderInterfaceException(Stage, Path, Permutation, "FrameConstants has incompatible member " + Name);
	}
	return true;
}

[[nodiscard]] uint64 ValidateEngineBindings(GLuint Program, ShaderStage Stage, const std::filesystem::path &Path,
											const ShaderPermutationKey &Permutation)
{
	uint64 InterfaceMask = 0;
	if (ValidateFrameConstants(Program, Stage, Path, Permutation))
		InterfaceMask |= UniformBlockBindingBit(static_cast<uint32>(pipeline::render::RendererBinding::FrameConstants));
	constexpr std::array StorageContracts{
		StorageBlockContract{"InstanceData", static_cast<GLuint>(pipeline::render::RendererBinding::Instances),
							 static_cast<GLint>(sizeof(pipeline::render::GPUInstanceRecord))},
		StorageBlockContract{"VisibleInstances", static_cast<GLuint>(pipeline::render::RendererBinding::Instances),
							 static_cast<GLint>(sizeof(pipeline::render::GPUInstanceRecord))},
		StorageBlockContract{"Materials", static_cast<GLuint>(pipeline::render::RendererBinding::Materials),
							 static_cast<GLint>(sizeof(pipeline::render::GPUMaterialRecord))},
		StorageBlockContract{"Lights", static_cast<GLuint>(pipeline::render::RendererBinding::Lights),
							 static_cast<GLint>(sizeof(pipeline::render::GPULightRecord))},
		StorageBlockContract{"ClusterHeaders", static_cast<GLuint>(pipeline::render::RendererBinding::ClusterHeaders),
							 static_cast<GLint>(sizeof(pipeline::render::GPUClusterHeader))},
		StorageBlockContract{"ClusterIndices", static_cast<GLuint>(pipeline::render::RendererBinding::ClusterIndices),
							 static_cast<GLint>(sizeof(uint32))},
		StorageBlockContract{"CandidateInstances", static_cast<GLuint>(pipeline::render::RendererBinding::Candidates),
							 static_cast<GLint>(sizeof(pipeline::render::GPUInstanceRecord))},
		StorageBlockContract{"VisibilityScratch", static_cast<GLuint>(pipeline::render::RendererBinding::VisibilityScratch),
							 static_cast<GLint>(sizeof(uint32))},
		StorageBlockContract{"IndirectCommands", static_cast<GLuint>(pipeline::render::RendererBinding::IndirectCommands),
							 static_cast<GLint>(sizeof(uint32) * 5U)},
		StorageBlockContract{"ShadowData", static_cast<GLuint>(pipeline::render::RendererBinding::ShadowData),
							 static_cast<GLint>(sizeof(pipeline::render::GPUShadowRecord))},
		StorageBlockContract{"SkinMatrices", static_cast<GLuint>(pipeline::render::RendererBinding::SkinMatrices),
							 static_cast<GLint>(sizeof(pipeline::render::GPUSkinMatrixRecord))},
		StorageBlockContract{"MorphDeltas", static_cast<GLuint>(pipeline::render::RendererBinding::MorphDeltas),
							 static_cast<GLint>(sizeof(pipeline::render::GPUMorphDeltaRecord))},
		StorageBlockContract{"MorphWeights", static_cast<GLuint>(pipeline::render::RendererBinding::MorphWeights),
							 static_cast<GLint>(sizeof(pipeline::render::GPUMorphWeightRecord))},
		StorageBlockContract{"SelectionMask", static_cast<GLuint>(pipeline::render::RendererBinding::SelectionMask),
							 static_cast<GLint>(sizeof(uint32))},
		StorageBlockContract{"DebugLines", static_cast<GLuint>(pipeline::render::RendererBinding::DebugLines),
							 static_cast<GLint>(sizeof(glm::vec4) * 3U)}};
	for (const StorageBlockContract &Contract : StorageContracts)
	{
		const GLuint Block = glGetProgramResourceIndex(Program, GL_SHADER_STORAGE_BLOCK, Contract.Name);
		if (Block == GL_INVALID_INDEX)
			continue;
		constexpr GLenum Property = GL_BUFFER_BINDING;
		GLint Binding = -1;
		glGetProgramResourceiv(Program, GL_SHADER_STORAGE_BLOCK, Block, 1, &Property, 1, nullptr, &Binding);
		if (Binding != static_cast<GLint>(Contract.Binding))
			throw ShaderInterfaceException(Stage, Path, Permutation,
										   std::string(Contract.Name) + " must use SSBO binding " + std::to_string(Contract.Binding));
		ValidateStorageLayout(Program, Contract, Block, Stage, Path, Permutation);
		InterfaceMask |= StorageBlockBindingBit(Contract.Binding);
	}
	return InterfaceMask;
}

[[nodiscard]] std::vector<ShaderModule::VertexInput> ReflectVertexInputs(GLuint Program, ShaderStage Stage)
{
	std::vector<ShaderModule::VertexInput> Inputs;
	if (Stage != ShaderStage::Vertex)
	{
		return Inputs;
	}

	GLint ResourceCount = 0;
	glGetProgramInterfaceiv(Program, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES, &ResourceCount);
	if (ResourceCount <= 0)
	{
		return Inputs;
	}

	Inputs.reserve(static_cast<usize>(ResourceCount));
	constexpr std::array<GLenum, 2> Properties{GL_LOCATION, GL_TYPE};
	for (uint32 ResourceIndex = 0; ResourceIndex < static_cast<uint32>(ResourceCount); ++ResourceIndex)
	{
		std::array<GLint, 2> Values{};
		glGetProgramResourceiv(Program, GL_PROGRAM_INPUT, ResourceIndex, static_cast<GLsizei>(Properties.size()), Properties.data(),
							   static_cast<GLsizei>(Values.size()), nullptr, Values.data());
		if (Values[0] >= 0)
		{
			Inputs.push_back(ShaderModule::VertexInput{Values[0], static_cast<GLenum>(Values[1])});
		}
	}
	return Inputs;
}

[[nodiscard]] bool IsOpaqueUniformType(const GLenum Type) noexcept
{
	switch (Type)
	{
	case GL_SAMPLER_1D:
	case GL_SAMPLER_2D:
	case GL_SAMPLER_3D:
	case GL_SAMPLER_CUBE:
	case GL_SAMPLER_1D_SHADOW:
	case GL_SAMPLER_2D_SHADOW:
	case GL_SAMPLER_1D_ARRAY:
	case GL_SAMPLER_2D_ARRAY:
	case GL_SAMPLER_1D_ARRAY_SHADOW:
	case GL_SAMPLER_2D_ARRAY_SHADOW:
	case GL_SAMPLER_2D_MULTISAMPLE:
	case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_SAMPLER_CUBE_SHADOW:
	case GL_SAMPLER_CUBE_MAP_ARRAY:
	case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
	case GL_INT_SAMPLER_1D:
	case GL_INT_SAMPLER_2D:
	case GL_INT_SAMPLER_3D:
	case GL_INT_SAMPLER_CUBE:
	case GL_INT_SAMPLER_1D_ARRAY:
	case GL_INT_SAMPLER_2D_ARRAY:
	case GL_INT_SAMPLER_2D_MULTISAMPLE:
	case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_1D:
	case GL_UNSIGNED_INT_SAMPLER_2D:
	case GL_UNSIGNED_INT_SAMPLER_3D:
	case GL_UNSIGNED_INT_SAMPLER_CUBE:
	case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
	case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
	case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
	case GL_IMAGE_1D:
	case GL_IMAGE_2D:
	case GL_IMAGE_3D:
	case GL_IMAGE_2D_RECT:
	case GL_IMAGE_CUBE:
	case GL_IMAGE_BUFFER:
	case GL_IMAGE_1D_ARRAY:
	case GL_IMAGE_2D_ARRAY:
	case GL_IMAGE_CUBE_MAP_ARRAY:
	case GL_IMAGE_2D_MULTISAMPLE:
	case GL_IMAGE_2D_MULTISAMPLE_ARRAY:
	case GL_INT_IMAGE_1D:
	case GL_INT_IMAGE_2D:
	case GL_INT_IMAGE_3D:
	case GL_INT_IMAGE_2D_RECT:
	case GL_INT_IMAGE_CUBE:
	case GL_INT_IMAGE_BUFFER:
	case GL_INT_IMAGE_1D_ARRAY:
	case GL_INT_IMAGE_2D_ARRAY:
	case GL_INT_IMAGE_CUBE_MAP_ARRAY:
	case GL_INT_IMAGE_2D_MULTISAMPLE:
	case GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
	case GL_UNSIGNED_INT_IMAGE_1D:
	case GL_UNSIGNED_INT_IMAGE_2D:
	case GL_UNSIGNED_INT_IMAGE_3D:
	case GL_UNSIGNED_INT_IMAGE_2D_RECT:
	case GL_UNSIGNED_INT_IMAGE_CUBE:
	case GL_UNSIGNED_INT_IMAGE_BUFFER:
	case GL_UNSIGNED_INT_IMAGE_1D_ARRAY:
	case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
	case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
	case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE:
	case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] std::vector<ShaderModule::UniformResource> ReflectUniformResources(const GLuint Program)
{
	std::vector<ShaderModule::UniformResource> Resources;
	GLint ResourceCount = 0;
	glGetProgramInterfaceiv(Program, GL_UNIFORM, GL_ACTIVE_RESOURCES, &ResourceCount);
	if (ResourceCount <= 0)
		return Resources;
	Resources.reserve(static_cast<usize>(ResourceCount));
	constexpr std::array<GLenum, 5> Properties{GL_NAME_LENGTH, GL_LOCATION, GL_BLOCK_INDEX, GL_TYPE, GL_ARRAY_SIZE};
	for (uint32 ResourceIndex = 0; ResourceIndex < static_cast<uint32>(ResourceCount); ++ResourceIndex)
	{
		std::array<GLint, 5> Values{};
		glGetProgramResourceiv(Program, GL_UNIFORM, ResourceIndex, static_cast<GLsizei>(Properties.size()), Properties.data(),
							   static_cast<GLsizei>(Values.size()), nullptr, Values.data());
		if (Values[1] < 0 || Values[2] != -1 || Values[0] <= 1)
			continue;
		std::string Name(static_cast<usize>(Values[0]), '\0');
		GLsizei Written = 0;
		glGetProgramResourceName(Program, GL_UNIFORM, ResourceIndex, Values[0], &Written, Name.data());
		Name.resize(static_cast<usize>(Written));
		GLint Binding = -1;
		if (IsOpaqueUniformType(static_cast<GLenum>(Values[3])))
			glGetUniformiv(Program, Values[1], &Binding);
		Resources.push_back({.Name = std::move(Name),
							 .Location = Values[1],
							 .Type = static_cast<GLenum>(Values[3]),
							 .ArraySize = Values[4],
							 .Binding = Binding});
	}
	return Resources;
}
} // namespace
ShaderModule::ShaderModule(device::Device &Device, const ShaderSourceAsset &Source, ShaderPermutationKey Permutation,
						   const ShaderPreprocessResult &Preprocessed)
	: Device(Device), Stage(Source.GetStage())
{
	(void)this->Device->RequireCurrentContext();
	GLuint Shader = glCreateShader(ToGLStage(this->Stage));
	if (Shader == 0)
		throw ShaderCompilationException(this->Stage, Source.GetSourcePath(), Permutation, "OpenGL could not allocate the shader object");
	try
	{
		const GLchar *Text = Preprocessed.Source.c_str();
		glShaderSource(Shader, 1, &Text, nullptr);
		glCompileShader(Shader);
		GLint Compiled = GL_FALSE;
		glGetShaderiv(Shader, GL_COMPILE_STATUS, &Compiled);
		if (Compiled != GL_TRUE)
			throw ShaderCompilationException(this->Stage, Source.GetSourcePath(), Permutation, Log(Shader, false));
		this->ProgramID = glCreateProgram();
		if (this->ProgramID == 0)
			throw ShaderLinkException(this->Stage, Source.GetSourcePath(), Permutation, "OpenGL could not allocate the shader program");
		glProgramParameteri(this->ProgramID, GL_PROGRAM_SEPARABLE, GL_TRUE);
		glAttachShader(this->ProgramID, Shader);
		glLinkProgram(this->ProgramID);
		glDeleteShader(Shader);
		Shader = 0;
		GLint Linked = GL_FALSE;
		glGetProgramiv(this->ProgramID, GL_LINK_STATUS, &Linked);
		if (Linked != GL_TRUE)
			throw ShaderLinkException(this->Stage, Source.GetSourcePath(), Permutation, Log(this->ProgramID, true));
		InterfaceQueryCount.fetch_add(1, std::memory_order_relaxed);
		this->EngineInterfaceMask = ValidateEngineBindings(this->ProgramID, this->Stage, Source.GetSourcePath(), Permutation);
		this->VertexInputs = ReflectVertexInputs(this->ProgramID, this->Stage);
		this->UniformResources = ReflectUniformResources(this->ProgramID);
		this->UniformLocations.reserve(this->UniformResources.size());
		for (const UniformResource &Resource : this->UniformResources)
			this->UniformLocations.emplace(Resource.Name, Resource.Location);
		this->Device->CheckErrors("ShaderModule creation");
	}
	catch (...)
	{
		if (Shader != 0)
			glDeleteShader(Shader);
		if (this->ProgramID != 0)
			glDeleteProgram(this->ProgramID);
		this->ProgramID = 0;
		throw;
	}
}
ShaderModule::~ShaderModule()
{
	if (this->ProgramID != 0)
	{
		if (device::Device *LiveDevice = this->Device.TryGet(); LiveDevice != nullptr)
			LiveDevice->RetireGPUObject(device::GPUObjectType::Program, this->ProgramID);
		this->ProgramID = 0;
	}
}
GLuint ShaderModule::GetProgramID() const noexcept
{
	return this->ProgramID;
}
ShaderStage ShaderModule::GetStage() const noexcept
{
	return this->Stage;
}
const std::vector<ShaderModule::VertexInput> &ShaderModule::GetVertexInputs() const noexcept
{
	return this->VertexInputs;
}
const std::unordered_map<std::string, GLint> &ShaderModule::GetUniformLocations() const noexcept
{
	return this->UniformLocations;
}
const std::vector<ShaderModule::UniformResource> &ShaderModule::GetUniformResources() const noexcept
{
	return this->UniformResources;
}
uint64 ShaderModule::GetEngineInterfaceMask() const noexcept
{
	return this->EngineInterfaceMask;
}
uint64 ShaderModule::GetInterfaceQueryCountForValidation() noexcept
{
	return InterfaceQueryCount.load(std::memory_order_relaxed);
}
} // namespace pipeline::shader
