#include "ShaderModule.h"

#include "ShaderException.h"
#include "src/pipeline/device/Device.h"
#include "src/renderer/RendererGpuTypes.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace pipeline::shader
{
namespace
{
GLenum ToGLStage(ShaderStage Stage)
{
	return Stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : Stage == ShaderStage::Fragment ? GL_FRAGMENT_SHADER : GL_COMPUTE_SHADER;
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
	MEMBER_OFFSET("InstanceData", "transform", renderer::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("VisibleInstances", "transform", renderer::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("CandidateInstances", "transform", renderer::GPUInstanceRecord, Transform);
	MEMBER_OFFSET("InstanceData", "previousTransform", renderer::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("VisibleInstances", "previousTransform", renderer::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("CandidateInstances", "previousTransform", renderer::GPUInstanceRecord, PreviousTransform);
	MEMBER_OFFSET("InstanceData", "worldBounds", renderer::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("VisibleInstances", "worldBounds", renderer::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("CandidateInstances", "worldBounds", renderer::GPUInstanceRecord, WorldBounds);
	MEMBER_OFFSET("InstanceData", "materialIndex", renderer::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("VisibleInstances", "materialIndex", renderer::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("CandidateInstances", "materialIndex", renderer::GPUInstanceRecord, MaterialIndex);
	MEMBER_OFFSET("InstanceData", "objectID", renderer::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("VisibleInstances", "objectID", renderer::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("CandidateInstances", "objectID", renderer::GPUInstanceRecord, ObjectID);
	MEMBER_OFFSET("InstanceData", "batchIndex", renderer::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("VisibleInstances", "batchIndex", renderer::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("CandidateInstances", "batchIndex", renderer::GPUInstanceRecord, BatchIndex);
	MEMBER_OFFSET("InstanceData", "skinPaletteOffset", renderer::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("VisibleInstances", "skinPaletteOffset", renderer::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("CandidateInstances", "skinPaletteOffset", renderer::GPUInstanceRecord, SkinPaletteOffset);
	MEMBER_OFFSET("InstanceData", "previousSkinPaletteOffset", renderer::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("VisibleInstances", "previousSkinPaletteOffset", renderer::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("CandidateInstances", "previousSkinPaletteOffset", renderer::GPUInstanceRecord, PreviousSkinPaletteOffset);
	MEMBER_OFFSET("InstanceData", "flags", renderer::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("VisibleInstances", "flags", renderer::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("CandidateInstances", "flags", renderer::GPUInstanceRecord, Flags);
	MEMBER_OFFSET("InstanceData", "morphWeightOffset", renderer::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("VisibleInstances", "morphWeightOffset", renderer::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("CandidateInstances", "morphWeightOffset", renderer::GPUInstanceRecord, MorphWeightOffset);
	MEMBER_OFFSET("InstanceData", "morphWeightCount", renderer::GPUInstanceRecord, MorphWeightCount);
	MEMBER_OFFSET("VisibleInstances", "morphWeightCount", renderer::GPUInstanceRecord, MorphWeightCount);
	MEMBER_OFFSET("CandidateInstances", "morphWeightCount", renderer::GPUInstanceRecord, MorphWeightCount);

	MEMBER_OFFSET("Materials", "baseColorTexture", renderer::GPUMaterialRecord, BaseColorTexture);
	MEMBER_OFFSET("Materials", "normalTexture", renderer::GPUMaterialRecord, NormalTexture);
	MEMBER_OFFSET("Materials", "metallicRoughnessTexture", renderer::GPUMaterialRecord, MetallicRoughnessTexture);
	MEMBER_OFFSET("Materials", "occlusionTexture", renderer::GPUMaterialRecord, OcclusionTexture);
	MEMBER_OFFSET("Materials", "emissiveTexture", renderer::GPUMaterialRecord, EmissiveTexture);
	MEMBER_OFFSET("Materials", "specularTexture", renderer::GPUMaterialRecord, SpecularTexture);
	MEMBER_OFFSET("Materials", "transmissionTexture", renderer::GPUMaterialRecord, TransmissionTexture);
	MEMBER_OFFSET("Materials", "textureCoordinateSelectors", renderer::GPUMaterialRecord, TextureCoordinateSelectors);
	MEMBER_OFFSET("Materials", "baseColorFactor", renderer::GPUMaterialRecord, BaseColorFactor);
	MEMBER_OFFSET("Materials", "emissiveAndMetallic", renderer::GPUMaterialRecord, EmissiveAndMetallic);
	MEMBER_OFFSET("Materials", "roughnessTransmissionIor", renderer::GPUMaterialRecord, RoughnessTransmissionIOR);
	MEMBER_OFFSET("Materials", "textureControls", renderer::GPUMaterialRecord, TextureControls);

	MEMBER_OFFSET("Lights", "positionAndRange", renderer::GPULightRecord, PositionAndRange);
	MEMBER_OFFSET("Lights", "directionAndType", renderer::GPULightRecord, DirectionAndType);
	MEMBER_OFFSET("Lights", "colorAndIntensity", renderer::GPULightRecord, ColorAndIntensity);
	MEMBER_OFFSET("Lights", "spotAnglesAndShadow", renderer::GPULightRecord, SpotAnglesAndShadow);
	MEMBER_OFFSET("ClusterHeaders", "offset", renderer::GPUClusterHeader, Offset);
	MEMBER_OFFSET("ClusterHeaders", "count", renderer::GPUClusterHeader, Count);
	MEMBER_OFFSET("ClusterHeaders", "pad0", renderer::GPUClusterHeader, Pad0);
	MEMBER_OFFSET("ClusterHeaders", "pad1", renderer::GPUClusterHeader, Pad1);
	MEMBER_OFFSET("ShadowData", "viewProjection", renderer::GPUShadowRecord, ViewProjection);
	MEMBER_OFFSET("ShadowData", "atlasScaleBias", renderer::GPUShadowRecord, AtlasScaleBias);
	MEMBER_OFFSET("ShadowData", "depthBiasAndFilter", renderer::GPUShadowRecord, DepthBiasAndFilter);
	MEMBER_OFFSET("SkinMatrices", "current", renderer::GPUSkinMatrixRecord, Current);
	MEMBER_OFFSET("SkinMatrices", "previous", renderer::GPUSkinMatrixRecord, Previous);
	MEMBER_OFFSET("MorphDeltas", "positionDelta", renderer::GPUMorphDeltaRecord, PositionDelta);
	MEMBER_OFFSET("MorphDeltas", "normalDelta", renderer::GPUMorphDeltaRecord, NormalDelta);
	MEMBER_OFFSET("MorphWeights", "deltaOffset", renderer::GPUMorphWeightRecord, DeltaOffset);
	MEMBER_OFFSET("MorphWeights", "currentWeight", renderer::GPUMorphWeightRecord, CurrentWeight);
	MEMBER_OFFSET("MorphWeights", "previousWeight", renderer::GPUMorphWeightRecord, PreviousWeight);
	MEMBER_OFFSET("MorphWeights", "padding", renderer::GPUMorphWeightRecord, Padding);
#undef MEMBER_OFFSET
	if ((Block == "ClusterIndices" && IsDirectArray(Name, "indices")) || (Block == "VisibilityScratch" && IsDirectArray(Name, "scratch")))
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

void ValidateFrameConstants(const GLuint Program, const ShaderStage Stage, const std::filesystem::path &Path,
							const ShaderPermutationKey &Permutation)
{
	const GLuint Block = glGetProgramResourceIndex(Program, GL_UNIFORM_BLOCK, "FrameConstants");
	if (Block == GL_INVALID_INDEX)
		return;
	constexpr std::array<GLenum, 3> Properties{GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES};
	std::array<GLint, 3> Values{};
	glGetProgramResourceiv(Program, GL_UNIFORM_BLOCK, Block, static_cast<GLsizei>(Properties.size()), Properties.data(),
						   static_cast<GLsizei>(Values.size()), nullptr, Values.data());
	if (Values[0] != 0 || Values[1] != static_cast<GLint>(sizeof(renderer::GPUFrameConstants)))
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
	Expected = static_cast<GLint>(offsetof(renderer::GPUFrameConstants, Field))
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
}

void ValidateEngineBindings(GLuint Program, ShaderStage Stage, const std::filesystem::path &Path, const ShaderPermutationKey &Permutation)
{
	ValidateFrameConstants(Program, Stage, Path, Permutation);
	constexpr std::array StorageContracts{
		StorageBlockContract{"InstanceData", 0, static_cast<GLint>(sizeof(renderer::GPUInstanceRecord))},
		StorageBlockContract{"VisibleInstances", 0, static_cast<GLint>(sizeof(renderer::GPUInstanceRecord))},
		StorageBlockContract{"Materials", 1, static_cast<GLint>(sizeof(renderer::GPUMaterialRecord))},
		StorageBlockContract{"Lights", 2, static_cast<GLint>(sizeof(renderer::GPULightRecord))},
		StorageBlockContract{"ClusterHeaders", 3, static_cast<GLint>(sizeof(renderer::GPUClusterHeader))},
		StorageBlockContract{"ClusterIndices", 4, static_cast<GLint>(sizeof(uint32))},
		StorageBlockContract{"CandidateInstances", 5, static_cast<GLint>(sizeof(renderer::GPUInstanceRecord))},
		StorageBlockContract{"VisibilityScratch", 6, static_cast<GLint>(sizeof(uint32))},
		StorageBlockContract{"IndirectCommands", 7, static_cast<GLint>(sizeof(uint32) * 5U)},
		StorageBlockContract{"ShadowData", 8, static_cast<GLint>(sizeof(renderer::GPUShadowRecord))},
		StorageBlockContract{"SkinMatrices", 9, static_cast<GLint>(sizeof(renderer::GPUSkinMatrixRecord))},
		StorageBlockContract{"MorphDeltas", 10, static_cast<GLint>(sizeof(renderer::GPUMorphDeltaRecord))},
		StorageBlockContract{"MorphWeights", 11, static_cast<GLint>(sizeof(renderer::GPUMorphWeightRecord))}};
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
	}
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

[[nodiscard]] std::unordered_map<std::string, GLint> ReflectUniformLocations(const GLuint Program)
{
	std::unordered_map<std::string, GLint> Locations;
	GLint ResourceCount = 0;
	glGetProgramInterfaceiv(Program, GL_UNIFORM, GL_ACTIVE_RESOURCES, &ResourceCount);
	if (ResourceCount <= 0)
		return Locations;
	Locations.reserve(static_cast<usize>(ResourceCount));
	constexpr std::array<GLenum, 3> Properties{GL_NAME_LENGTH, GL_LOCATION, GL_BLOCK_INDEX};
	for (uint32 ResourceIndex = 0; ResourceIndex < static_cast<uint32>(ResourceCount); ++ResourceIndex)
	{
		std::array<GLint, 3> Values{};
		glGetProgramResourceiv(Program, GL_UNIFORM, ResourceIndex, static_cast<GLsizei>(Properties.size()), Properties.data(),
							   static_cast<GLsizei>(Values.size()), nullptr, Values.data());
		if (Values[1] < 0 || Values[2] != -1 || Values[0] <= 1)
			continue;
		std::string Name(static_cast<usize>(Values[0]), '\0');
		GLsizei Written = 0;
		glGetProgramResourceName(Program, GL_UNIFORM, ResourceIndex, Values[0], &Written, Name.data());
		Name.resize(static_cast<usize>(Written));
		Locations.emplace(std::move(Name), Values[1]);
	}
	return Locations;
}
} // namespace
ShaderModule::ShaderModule(device::Device &Device, const ShaderSourceAsset &Source, ShaderPermutationKey Permutation,
						   const ShaderPreprocessResult &Preprocessed)
	: Device(&Device), Stage(Source.GetStage())
{
	(void)this->Device->RequireCurrentContext();
	const GLuint Shader = glCreateShader(ToGLStage(this->Stage));
	const GLchar *Text = Preprocessed.Source.c_str();
	glShaderSource(Shader, 1, &Text, nullptr);
	glCompileShader(Shader);
	GLint Compiled = GL_FALSE;
	glGetShaderiv(Shader, GL_COMPILE_STATUS, &Compiled);
	if (Compiled != GL_TRUE)
	{
		const std::string Diagnostic = Log(Shader, false);
		glDeleteShader(Shader);
		throw ShaderCompilationException(this->Stage, Source.GetSourcePath(), Permutation, Diagnostic);
	}
	this->ProgramID = glCreateProgram();
	glProgramParameteri(this->ProgramID, GL_PROGRAM_SEPARABLE, GL_TRUE);
	glAttachShader(this->ProgramID, Shader);
	glLinkProgram(this->ProgramID);
	glDeleteShader(Shader);
	GLint Linked = GL_FALSE;
	glGetProgramiv(this->ProgramID, GL_LINK_STATUS, &Linked);
	if (Linked != GL_TRUE)
	{
		const std::string Diagnostic = Log(this->ProgramID, true);
		glDeleteProgram(this->ProgramID);
		this->ProgramID = 0;
		throw ShaderLinkException(this->Stage, Source.GetSourcePath(), Permutation, Diagnostic);
	}
	try
	{
		ValidateEngineBindings(this->ProgramID, this->Stage, Source.GetSourcePath(), Permutation);
		this->VertexInputs = ReflectVertexInputs(this->ProgramID, this->Stage);
		this->UniformLocations = ReflectUniformLocations(this->ProgramID);
	}
	catch (...)
	{
		glDeleteProgram(this->ProgramID);
		this->ProgramID = 0;
		throw;
	}
	this->Device->CheckErrors("ShaderModule creation");
}
ShaderModule::~ShaderModule()
{
	if (this->ProgramID != 0)
	{
		if (this->Device != nullptr && this->Device->CanIssueCommands())
			glDeleteProgram(this->ProgramID);
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
} // namespace pipeline::shader
