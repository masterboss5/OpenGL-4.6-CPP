#pragma once
#include <GL/glew.h>
#include <unordered_map>
#include "src/resource/Asset.h"
#include <filesystem>
#include "src/util/logger.h"

namespace pipeline::shader
{
	struct VertexShaderSource final
	{
		std::filesystem::path sourcePath;
		std::string source;
	};

	class VertexShader final : public resource::Asset
	{
	private:
		GLuint ID;
		bool compiled = false;
		VertexShaderSource source;
	public:
		explicit VertexShader(VertexShaderSource& source);
		~VertexShader();

		std::string_view getCompileLog() const;
		void compile();
		bool isCompiled() const;
		const VertexShaderSource& getSource() const;
	};
}