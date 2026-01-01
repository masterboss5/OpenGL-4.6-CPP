#include <GL/glew.h>
#include <GLFW/glfw3.h>

enum class BindingPoint : GLuint
{
	SPOT_LIGHT_SOURCES = 1,
	POINT_LIGHT_SOURCE = 2,
	DIRECTIONAL_LIGHT_SOURCES = 3,
	//Add more as needed
};

template<typename T, BindingPoint BINDING>
class ShaderSorageBufferObject final
{
private:
	GLuint bufferID;
	GLuint bytesSize;
	size_t maxElements;
public:

	ShaderSorageBufferObject(size_t maxElements);
	~ShaderSorageBufferObject();

	GLuint getBufferID() const;
	void bindBuffer() const;
	GLuint getBytesSize() const;
	GLuint getMaxElements() const;
};
