#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "src/pipeline/buffer/VertexBuffer.h"
#include "src/types.h"

namespace renderer
{
	enum class VertexAttributeDataType : std::uint8_t
	{
		Float16,
		Float32,
		Float64,
		Int8,
		UInt8,
		Int16,
		UInt16,
		Int32,
		UInt32
	};

	enum class VertexAttributeInput : std::uint8_t
	{
		FloatingPoint,
		Integer
	};

	enum class VertexInputRate : std::uint8_t
	{
		PerVertex,
		PerInstance
	};

	struct VertexBindingDescriptor final
	{
		GLuint bindingIndex = 0;
		std::size_t strideInBytes = 0;
		VertexInputRate inputRate = VertexInputRate::PerVertex;
		GLuint instanceStepRate = 0;
	};

	struct VertexAttributeDescriptor final
	{
		std::string semantic;
		GLuint semanticIndex = 0;
		GLuint location = 0;
		GLuint bindingIndex = 0;
		VertexAttributeDataType dataType = VertexAttributeDataType::Float32;
		GLuint componentCount = 0;
		bool normalized = false;
		VertexAttributeInput input = VertexAttributeInput::FloatingPoint;
		std::size_t relativeOffsetInBytes = 0;
	};

	class VertexDescriptor final
	{
	public:
		VertexDescriptor(
			std::span<const VertexBindingDescriptor> bindings,
			std::span<const VertexAttributeDescriptor> attributes);
		VertexDescriptor(
			std::initializer_list<VertexBindingDescriptor> bindings,
			std::initializer_list<VertexAttributeDescriptor> attributes);

		void applyToVertexArray(GLuint vertexArrayID) const;
		void bindVertexBuffer(GLuint vertexArrayID, GLuint bindingIndex, const VertexBuffer& buffer, std::size_t offsetInBytes = 0) const;

		[[nodiscard]] const VertexBindingDescriptor& getBinding(GLuint bindingIndex) const;
		[[nodiscard]] std::span<const VertexBindingDescriptor> getBindings() const noexcept;
		[[nodiscard]] std::span<const VertexAttributeDescriptor> getAttributes() const noexcept;
		[[nodiscard]] uint64 getLayoutHash() const noexcept;

	private:
		std::vector<VertexBindingDescriptor> bindings;
		std::vector<VertexAttributeDescriptor> attributes;

		void validate() const;
		[[nodiscard]] const VertexBindingDescriptor* findBinding(GLuint bindingIndex) const noexcept;
	};
}
