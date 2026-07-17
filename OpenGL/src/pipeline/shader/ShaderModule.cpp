#include "ShaderModule.h"

#include <array>
#include <string_view>
#include <vector>

#include "ShaderException.h"
#include "src/pipeline/device/OpenGLRuntime.h"

namespace pipeline::shader
{
	namespace
	{
		GLenum toGLStage(ShaderStage stage) { return stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : stage == ShaderStage::Fragment ? GL_FRAGMENT_SHADER : GL_COMPUTE_SHADER; }
		std::string log(GLuint object, bool program)
		{
			GLint length = 0; if (program) glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length); else glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
			std::vector<GLchar> buffer(static_cast<size_t>(length > 1 ? length : 1));
			if (program) glGetProgramInfoLog(object, length, nullptr, buffer.data()); else glGetShaderInfoLog(object, length, nullptr, buffer.data());
			return std::string(buffer.data());
		}

		void validateEngineBindings(GLuint program, ShaderStage stage, const std::filesystem::path& path, const ShaderPermutationKey& permutation)
		{
			constexpr GLuint frameConstantsBinding = 0;
			const GLuint frameConstants = glGetUniformBlockIndex(program, "FrameConstants");
			if (frameConstants != GL_INVALID_INDEX)
			{
				GLint binding = -1;
				glGetActiveUniformBlockiv(program, frameConstants, GL_UNIFORM_BLOCK_BINDING, &binding);
				if (binding != static_cast<GLint>(frameConstantsBinding)) throw ShaderInterfaceException(stage, path, permutation, "FrameConstants must use UBO binding 0");
			}

			struct StorageBlockContract final { const char* name; GLuint binding; };
			constexpr std::array storageContracts {
				StorageBlockContract { "InstanceData", 0 }, StorageBlockContract { "VisibleInstances", 0 },
				StorageBlockContract { "Materials", 1 }, StorageBlockContract { "MaterialData", 1 },
				StorageBlockContract { "Lights", 2 }, StorageBlockContract { "ClusterHeaders", 3 },
				StorageBlockContract { "ClusterIndices", 4 }, StorageBlockContract { "CandidateInstances", 5 },
				StorageBlockContract { "VisibilityScratch", 6 }, StorageBlockContract { "IndirectCommands", 7 },
				StorageBlockContract { "ShadowData", 8 }
			};
			for (const StorageBlockContract& contract : storageContracts)
			{
				const GLuint block = glGetProgramResourceIndex(program, GL_SHADER_STORAGE_BLOCK, contract.name);
				if (block == GL_INVALID_INDEX) continue;
				constexpr GLenum property = GL_BUFFER_BINDING;
				GLint binding = -1;
				glGetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, block, 1, &property, 1, nullptr, &binding);
				if (binding != static_cast<GLint>(contract.binding)) throw ShaderInterfaceException(stage, path, permutation, std::string(contract.name) + " must use SSBO binding " + std::to_string(contract.binding));
			}
		}

		[[nodiscard]] std::vector<ShaderModule::VertexInput> reflectVertexInputs(GLuint program, ShaderStage stage)
		{
			std::vector<ShaderModule::VertexInput> inputs;
			if (stage != ShaderStage::Vertex)
			{
				return inputs;
			}

			GLint resourceCount = 0;
			glGetProgramInterfaceiv(program, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES, &resourceCount);
			if (resourceCount <= 0)
			{
				return inputs;
			}

			inputs.reserve(static_cast<size_t>(resourceCount));
			constexpr std::array<GLenum, 2> properties { GL_LOCATION, GL_TYPE };
			for (uint32 resourceIndex = 0; resourceIndex < static_cast<uint32>(resourceCount); ++resourceIndex)
			{
				std::array<GLint, 2> values {};
				glGetProgramResourceiv(program, GL_PROGRAM_INPUT, resourceIndex, static_cast<GLsizei>(properties.size()), properties.data(), static_cast<GLsizei>(values.size()), nullptr, values.data());
				if (values[0] >= 0)
				{
					inputs.push_back(ShaderModule::VertexInput { values[0], static_cast<GLenum>(values[1]) });
				}
			}
			return inputs;
		}
	}
	ShaderModule::ShaderModule(const ShaderSourceAsset& source, ShaderPermutationKey permutation, const ShaderPreprocessResult& preprocessed) : stage(source.getStage())
	{
		device::requireOpenGL46Context();
		const GLuint shader = glCreateShader(toGLStage(this->stage));
		const GLchar* text = preprocessed.source.c_str(); glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
		GLint compiled = GL_FALSE; glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (compiled != GL_TRUE) { const std::string diagnostic = log(shader, false); glDeleteShader(shader); throw ShaderCompilationException(this->stage, source.getSourcePath(), permutation, diagnostic); }
		this->programID = glCreateProgram(); glProgramParameteri(this->programID, GL_PROGRAM_SEPARABLE, GL_TRUE); glAttachShader(this->programID, shader); glLinkProgram(this->programID); glDeleteShader(shader);
		GLint linked = GL_FALSE; glGetProgramiv(this->programID, GL_LINK_STATUS, &linked);
		if (linked != GL_TRUE) { const std::string diagnostic = log(this->programID, true); glDeleteProgram(this->programID); this->programID = 0; throw ShaderLinkException(this->stage, source.getSourcePath(), permutation, diagnostic); }
		try
		{
			validateEngineBindings(this->programID, this->stage, source.getSourcePath(), permutation);
			this->vertexInputs = reflectVertexInputs(this->programID, this->stage);
		}
		catch (...) { glDeleteProgram(this->programID); this->programID = 0; throw; }
	}
	ShaderModule::~ShaderModule() { if (this->programID != 0) glDeleteProgram(this->programID); }
	GLuint ShaderModule::getProgramID() const noexcept { return this->programID; }
	ShaderStage ShaderModule::getStage() const noexcept { return this->stage; }
	const std::vector<ShaderModule::VertexInput>& ShaderModule::getVertexInputs() const noexcept { return this->vertexInputs; }
}
