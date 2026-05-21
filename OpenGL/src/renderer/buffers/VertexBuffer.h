#pragma once
#include <GL/glew.h>

namespace renderer
{
	class VertexBuffer final
	{
	private:
		GLuint ID;
	public:
		VertexBuffer();
		~VertexBuffer();
		VertexBuffer(const VertexBuffer&);
		VertexBuffer(VertexBuffer&&);
		renderer::VertexBuffer& operator=(const VertexBuffer&);
		renderer::VertexBuffer& operator=(VertexBuffer&&);

		void bind();
		GLuint getID();
	};
}