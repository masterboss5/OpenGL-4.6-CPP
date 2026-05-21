#pragma once
#include <GL/glew.h>
#include <unordered_map>
#include "src/resource/Asset.h"
#include <filesystem>
#include "src/util/logger.h"
#include "src/util/UUID.h"

namespace pipeline::shader
{
	struct FragmentShaderSource final
	{
		std::filesystem::path sourcePath;
		std::string source;
	};

	class FragmentShader final : public resource::Asset
	{
	private:
		GLuint ID;
		bool compiled = false;
		FragmentShaderSource source;
	public:
		explicit FragmentShader(FragmentShaderSource& source);
		~FragmentShader();

		std::string_view getCompileLog() const;
		void compile();
		bool isCompiled() const;
		const FragmentShaderSource& getSource() const;
	};
}