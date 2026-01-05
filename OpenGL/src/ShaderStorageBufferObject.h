#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <type_traits>

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
	T* bufferPointer;
public:

	ShaderSorageBufferObject(size_t maxElements);
	~ShaderSorageBufferObject();

	void uploadData(const T* data, size_t count) const;
	GLuint getBufferID() const;
	void bindBuffer() const;
	size_t getBytesSize() const;
	size_t getMaxElements() const;
	T* getBufferPointer() const;

	static_assert(std::is_trivially_copyable_v<T>, "SSBO element type must be trivially copyable");
};
