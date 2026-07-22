#pragma once

#include "src/pipeline/buffer/VertexBuffer.h"
#include "src/types.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace renderer
{
enum class VertexAttributeDataType : uint8
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

enum class VertexAttributeInput : uint8
{
	FloatingPoint,
	Integer
};

enum class VertexInputRate : uint8
{
	PerVertex,
	PerInstance
};

struct VertexBindingDescriptor final
{
	GLuint BindingIndex = 0;
	usize StrideInBytes = 0;
	VertexInputRate InputRate = VertexInputRate::PerVertex;
	GLuint InstanceStepRate = 0;
};

struct VertexAttributeDescriptor final
{
	std::string Semantic;
	GLuint SemanticIndex = 0;
	GLuint Location = 0;
	GLuint BindingIndex = 0;
	VertexAttributeDataType DataType = VertexAttributeDataType::Float32;
	GLuint ComponentCount = 0;
	bool Normalized = false;
	VertexAttributeInput Input = VertexAttributeInput::FloatingPoint;
	usize RelativeOffsetInBytes = 0;
};

class VertexDescriptor final
{
  public:
	VertexDescriptor(std::span<const VertexBindingDescriptor> Bindings, std::span<const VertexAttributeDescriptor> Attributes);
	VertexDescriptor(std::initializer_list<VertexBindingDescriptor> Bindings, std::initializer_list<VertexAttributeDescriptor> Attributes);

	void ApplyToVertexArray(pipeline::device::Device &Device, GLuint VertexArrayID) const;
	void BindVertexBuffer(pipeline::device::Device &Device, GLuint VertexArrayID, GLuint BindingIndex, const VertexBuffer &Buffer,
						  usize OffsetInBytes = 0) const;

	[[nodiscard]] const VertexBindingDescriptor &GetBinding(GLuint BindingIndex) const;
	[[nodiscard]] std::span<const VertexBindingDescriptor> GetBindings() const noexcept;
	[[nodiscard]] std::span<const VertexAttributeDescriptor> GetAttributes() const noexcept;
	[[nodiscard]] uint64 GetLayoutHash() const noexcept;

  private:
	std::vector<VertexBindingDescriptor> Bindings;
	std::vector<VertexAttributeDescriptor> Attributes;

	void Validate() const;
	[[nodiscard]] const VertexBindingDescriptor *FindBinding(GLuint BindingIndex) const noexcept;
};
} // namespace renderer
