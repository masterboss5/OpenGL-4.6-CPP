#include "VertexDescriptor.h"
#include "src/pipeline/device/OpenGLRuntime.h"

#include <algorithm>
#include <limits>

namespace renderer
{
	namespace
	{
		[[nodiscard]] GLenum getOpenGLType(VertexAttributeDataType dataType)
		{
			switch (dataType)
			{
			case VertexAttributeDataType::Float16: return GL_HALF_FLOAT;
			case VertexAttributeDataType::Float32: return GL_FLOAT;
			case VertexAttributeDataType::Float64: return GL_DOUBLE;
			case VertexAttributeDataType::Int8: return GL_BYTE;
			case VertexAttributeDataType::UInt8: return GL_UNSIGNED_BYTE;
			case VertexAttributeDataType::Int16: return GL_SHORT;
			case VertexAttributeDataType::UInt16: return GL_UNSIGNED_SHORT;
			case VertexAttributeDataType::Int32: return GL_INT;
			case VertexAttributeDataType::UInt32: return GL_UNSIGNED_INT;
			}

			throw VertexBufferError("VertexDescriptor contains an unknown attribute data type");
		}

		[[nodiscard]] std::size_t getTypeSize(VertexAttributeDataType dataType)
		{
			switch (dataType)
			{
			case VertexAttributeDataType::Float16: return sizeof(std::uint16_t);
			case VertexAttributeDataType::Float32: return sizeof(float);
			case VertexAttributeDataType::Float64: return sizeof(double);
			case VertexAttributeDataType::Int8: return sizeof(std::int8_t);
			case VertexAttributeDataType::UInt8: return sizeof(std::uint8_t);
			case VertexAttributeDataType::Int16: return sizeof(std::int16_t);
			case VertexAttributeDataType::UInt16: return sizeof(std::uint16_t);
			case VertexAttributeDataType::Int32: return sizeof(std::int32_t);
			case VertexAttributeDataType::UInt32: return sizeof(std::uint32_t);
			}

			throw VertexBufferError("VertexDescriptor contains an unknown attribute data type");
		}

		[[nodiscard]] bool isIntegerType(VertexAttributeDataType dataType)
		{
			return dataType != VertexAttributeDataType::Float16 &&
				dataType != VertexAttributeDataType::Float32 &&
				dataType != VertexAttributeDataType::Float64;
		}

		void requireVertexArray(GLuint vertexArrayID)
		{
			if (vertexArrayID == 0 || glIsVertexArray(vertexArrayID) == GL_FALSE)
			{
				throw VertexBufferError("VertexDescriptor requires a created vertex array object");
			}
		}
	}

	VertexDescriptor::VertexDescriptor(
		std::span<const VertexBindingDescriptor> bindingDescriptors,
		std::span<const VertexAttributeDescriptor> attributeDescriptors)
		: bindings(bindingDescriptors.begin(), bindingDescriptors.end()),
		attributes(attributeDescriptors.begin(), attributeDescriptors.end())
	{
		this->validate();
	}

	VertexDescriptor::VertexDescriptor(
		std::initializer_list<VertexBindingDescriptor> bindingDescriptors,
		std::initializer_list<VertexAttributeDescriptor> attributeDescriptors)
		: VertexDescriptor(
			std::span<const VertexBindingDescriptor>(bindingDescriptors.begin(), bindingDescriptors.size()),
			std::span<const VertexAttributeDescriptor>(attributeDescriptors.begin(), attributeDescriptors.size()))
	{
	}

	void VertexDescriptor::applyToVertexArray(GLuint vertexArrayID) const
	{
		pipeline::device::requireOpenGL46Context();
		requireVertexArray(vertexArrayID);
		pipeline::device::throwPendingOpenGLErrors("VertexDescriptor::applyToVertexArray precondition");

		GLint maxAttributes = 0;
		GLint maxBindings = 0;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttributes);
		glGetIntegerv(GL_MAX_VERTEX_ATTRIB_BINDINGS, &maxBindings);
		pipeline::device::throwPendingOpenGLErrors("VertexDescriptor capability query");

		for (const VertexBindingDescriptor& binding : this->bindings)
		{
			if (binding.bindingIndex >= static_cast<GLuint>(maxBindings))
			{
				throw VertexBufferError("VertexDescriptor binding index exceeds OpenGL limits");
			}
		}
		for (const VertexAttributeDescriptor& attribute : this->attributes)
		{
			if (attribute.location >= static_cast<GLuint>(maxAttributes))
			{
				throw VertexBufferError("VertexDescriptor attribute location exceeds OpenGL limits");
			}
		}

		for (GLint location = 0; location < maxAttributes; ++location)
		{
			glDisableVertexArrayAttrib(vertexArrayID, static_cast<GLuint>(location));
		}
		for (GLint binding = 0; binding < maxBindings; ++binding)
		{
			glVertexArrayBindingDivisor(vertexArrayID, static_cast<GLuint>(binding), 0);
		}

		for (const VertexBindingDescriptor& binding : this->bindings)
		{
			glVertexArrayBindingDivisor(vertexArrayID, binding.bindingIndex, binding.instanceStepRate);
		}

		for (const VertexAttributeDescriptor& attribute : this->attributes)
		{
			glEnableVertexArrayAttrib(vertexArrayID, attribute.location);
			glVertexArrayAttribBinding(vertexArrayID, attribute.location, attribute.bindingIndex);

			const GLenum dataType = getOpenGLType(attribute.dataType);
			const GLuint relativeOffset = static_cast<GLuint>(attribute.relativeOffsetInBytes);
			if (attribute.input == VertexAttributeInput::Integer)
			{
				glVertexArrayAttribIFormat(vertexArrayID, attribute.location, static_cast<GLint>(attribute.componentCount), dataType, relativeOffset);
			}
			else if (attribute.dataType == VertexAttributeDataType::Float64)
			{
				glVertexArrayAttribLFormat(vertexArrayID, attribute.location, static_cast<GLint>(attribute.componentCount), dataType, relativeOffset);
			}
			else
			{
				glVertexArrayAttribFormat(vertexArrayID, attribute.location, static_cast<GLint>(attribute.componentCount), dataType, attribute.normalized ? GL_TRUE : GL_FALSE, relativeOffset);
			}
		}

		pipeline::device::throwPendingOpenGLErrors("VertexDescriptor::applyToVertexArray");
	}

	void VertexDescriptor::bindVertexBuffer(GLuint vertexArrayID, GLuint bindingIndex, const VertexBuffer& buffer, std::size_t offsetInBytes) const
	{
		pipeline::device::requireOpenGL46Context();
		requireVertexArray(vertexArrayID);
		const VertexBindingDescriptor& binding = this->getBinding(bindingIndex);
		if (buffer.isMapped())
		{
			throw VertexBufferError("VertexDescriptor cannot bind a mapped VertexBuffer");
		}
		if (buffer.getStrideInBytes() != binding.strideInBytes)
		{
			throw VertexBufferError("VertexDescriptor binding stride does not match the VertexBuffer stride");
		}
		if (offsetInBytes >= buffer.getSizeInBytes())
		{
			throw VertexBufferError("VertexDescriptor binding offset exceeds VertexBuffer storage");
		}
		if (offsetInBytes > static_cast<std::size_t>(std::numeric_limits<GLintptr>::max()))
		{
			throw VertexBufferError("VertexDescriptor binding offset exceeds OpenGL limits");
		}
		if (binding.strideInBytes > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
		{
			throw VertexBufferError("VertexDescriptor binding stride exceeds OpenGL limits");
		}

		pipeline::device::throwPendingOpenGLErrors("VertexDescriptor::bindVertexBuffer precondition");
		glVertexArrayVertexBuffer(vertexArrayID, bindingIndex, buffer.getID(), static_cast<GLintptr>(offsetInBytes), static_cast<GLsizei>(binding.strideInBytes));
		pipeline::device::throwPendingOpenGLErrors("glVertexArrayVertexBuffer");
	}

	const VertexBindingDescriptor& VertexDescriptor::getBinding(GLuint bindingIndex) const
	{
		const VertexBindingDescriptor* binding = this->findBinding(bindingIndex);
		if (binding == nullptr)
		{
			throw VertexBufferError("VertexDescriptor does not contain the requested vertex binding");
		}

		return *binding;
	}

	std::span<const VertexBindingDescriptor> VertexDescriptor::getBindings() const noexcept
	{
		return this->bindings;
	}

	std::span<const VertexAttributeDescriptor> VertexDescriptor::getAttributes() const noexcept
	{
		return this->attributes;
	}

	uint64 VertexDescriptor::getLayoutHash() const noexcept
	{
		constexpr uint64 offsetBasis = 14695981039346656037ULL;
		constexpr uint64 prime = 1099511628211ULL;
		uint64 hash = offsetBasis;
		const auto append = [&hash, prime](uint64 value)
		{
			for (uint32 byte = 0; byte < sizeof(value); ++byte)
			{
				hash ^= (value >> (byte * 8U)) & 0xFFU;
				hash *= prime;
			}
		};
		for (const VertexBindingDescriptor& binding : this->bindings)
		{
			append(binding.bindingIndex);
			append(binding.strideInBytes);
			append(static_cast<uint64>(binding.inputRate));
			append(binding.instanceStepRate);
		}
		for (const VertexAttributeDescriptor& attribute : this->attributes)
		{
			for (const auto semanticCharacter : attribute.semantic) append(static_cast<uint8>(semanticCharacter));
			append(attribute.semanticIndex);
			append(attribute.location);
			append(attribute.bindingIndex);
			append(static_cast<uint64>(attribute.dataType));
			append(attribute.componentCount);
			append(attribute.normalized ? 1U : 0U);
			append(static_cast<uint64>(attribute.input));
			append(attribute.relativeOffsetInBytes);
		}
		return hash;
	}

	void VertexDescriptor::validate() const
	{
		if (this->bindings.empty() || this->attributes.empty())
		{
			throw VertexBufferError("VertexDescriptor requires at least one binding and one attribute");
		}

		for (const VertexBindingDescriptor& binding : this->bindings)
		{
			if (binding.strideInBytes == 0)
			{
				throw VertexBufferError("VertexDescriptor binding stride must be greater than zero");
			}
			if ((binding.inputRate == VertexInputRate::PerVertex && binding.instanceStepRate != 0) ||
				(binding.inputRate == VertexInputRate::PerInstance && binding.instanceStepRate == 0))
			{
				throw VertexBufferError("VertexDescriptor binding input rate and instance step rate are inconsistent");
			}
			if (std::count_if(this->bindings.begin(), this->bindings.end(), [&binding](const VertexBindingDescriptor& other) { return other.bindingIndex == binding.bindingIndex; }) != 1)
			{
				throw VertexBufferError("VertexDescriptor contains duplicate binding indices");
			}
		}

		for (const VertexAttributeDescriptor& attribute : this->attributes)
		{
			const VertexBindingDescriptor* binding = this->findBinding(attribute.bindingIndex);
			if (binding == nullptr)
			{
				throw VertexBufferError("VertexDescriptor attribute references an unknown binding");
			}
			if (attribute.componentCount == 0 || attribute.componentCount > 4)
			{
				throw VertexBufferError("VertexDescriptor attribute component count must be between one and four");
			}
			if (attribute.input == VertexAttributeInput::Integer && (!isIntegerType(attribute.dataType) || attribute.normalized))
			{
				throw VertexBufferError("Integer VertexDescriptor attributes require an integer type and cannot be normalized");
			}
			if (attribute.normalized && !isIntegerType(attribute.dataType))
			{
				throw VertexBufferError("Only integer VertexDescriptor attributes can be normalized");
			}

			const std::size_t attributeSize = getTypeSize(attribute.dataType) * attribute.componentCount;
			if (attribute.relativeOffsetInBytes > binding->strideInBytes || attributeSize > binding->strideInBytes - attribute.relativeOffsetInBytes)
			{
				throw VertexBufferError("VertexDescriptor attribute exceeds its binding stride");
			}
			if (attribute.relativeOffsetInBytes > static_cast<std::size_t>(std::numeric_limits<GLuint>::max()))
			{
				throw VertexBufferError("VertexDescriptor attribute offset exceeds OpenGL limits");
			}
			if (std::count_if(this->attributes.begin(), this->attributes.end(), [&attribute](const VertexAttributeDescriptor& other) { return other.location == attribute.location; }) != 1)
			{
				throw VertexBufferError("VertexDescriptor contains duplicate attribute locations");
			}
		}
	}

	const VertexBindingDescriptor* VertexDescriptor::findBinding(GLuint bindingIndex) const noexcept
	{
		const auto iterator = std::find_if(this->bindings.begin(), this->bindings.end(), [bindingIndex](const VertexBindingDescriptor& binding)
		{
			return binding.bindingIndex == bindingIndex;
		});
		return iterator == this->bindings.end() ? nullptr : &*iterator;
	}
}
