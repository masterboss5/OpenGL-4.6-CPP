#include "VertexDescriptor.h"

#include "src/pipeline/device/Device.h"

#include <algorithm>
#include <limits>

namespace renderer
{
namespace
{
[[nodiscard]] GLenum GetOpenGlType(VertexAttributeDataType DataType)
{
	switch (DataType)
	{
	case VertexAttributeDataType::Float16:
		return GL_HALF_FLOAT;
	case VertexAttributeDataType::Float32:
		return GL_FLOAT;
	case VertexAttributeDataType::Float64:
		return GL_DOUBLE;
	case VertexAttributeDataType::Int8:
		return GL_BYTE;
	case VertexAttributeDataType::UInt8:
		return GL_UNSIGNED_BYTE;
	case VertexAttributeDataType::Int16:
		return GL_SHORT;
	case VertexAttributeDataType::UInt16:
		return GL_UNSIGNED_SHORT;
	case VertexAttributeDataType::Int32:
		return GL_INT;
	case VertexAttributeDataType::UInt32:
		return GL_UNSIGNED_INT;
	}

	throw VertexBufferError("VertexDescriptor contains an unknown attribute data type");
}

[[nodiscard]] usize GetTypeSize(VertexAttributeDataType DataType)
{
	switch (DataType)
	{
	case VertexAttributeDataType::Float16:
		return sizeof(uint16);
	case VertexAttributeDataType::Float32:
		return sizeof(float32);
	case VertexAttributeDataType::Float64:
		return sizeof(float64);
	case VertexAttributeDataType::Int8:
		return sizeof(int8);
	case VertexAttributeDataType::UInt8:
		return sizeof(uint8);
	case VertexAttributeDataType::Int16:
		return sizeof(int16);
	case VertexAttributeDataType::UInt16:
		return sizeof(uint16);
	case VertexAttributeDataType::Int32:
		return sizeof(int32);
	case VertexAttributeDataType::UInt32:
		return sizeof(uint32);
	}

	throw VertexBufferError("VertexDescriptor contains an unknown attribute data type");
}

[[nodiscard]] bool IsIntegerType(VertexAttributeDataType DataType)
{
	return DataType != VertexAttributeDataType::Float16 && DataType != VertexAttributeDataType::Float32 &&
		   DataType != VertexAttributeDataType::Float64;
}

void RequireVertexArray(GLuint VertexArrayID)
{
	if (VertexArrayID == 0 || glIsVertexArray(VertexArrayID) == GL_FALSE)
	{
		throw VertexBufferError("VertexDescriptor requires a created vertex array object");
	}
}
} // namespace

VertexDescriptor::VertexDescriptor(std::span<const VertexBindingDescriptor> BindingDescriptors,
								   std::span<const VertexAttributeDescriptor> AttributeDescriptors)
	: Bindings(BindingDescriptors.begin(), BindingDescriptors.end()), Attributes(AttributeDescriptors.begin(), AttributeDescriptors.end())
{
	this->Validate();
}

VertexDescriptor::VertexDescriptor(std::initializer_list<VertexBindingDescriptor> BindingDescriptors,
								   std::initializer_list<VertexAttributeDescriptor> AttributeDescriptors)
	: VertexDescriptor(std::span<const VertexBindingDescriptor>(BindingDescriptors.begin(), BindingDescriptors.size()),
					   std::span<const VertexAttributeDescriptor>(AttributeDescriptors.begin(), AttributeDescriptors.size()))
{
}

void VertexDescriptor::ApplyToVertexArray(pipeline::device::Device &Device, GLuint VertexArrayID) const
{
	(void)Device.RequireCurrentContext();
	RequireVertexArray(VertexArrayID);
	Device.CheckErrors("VertexDescriptor::ApplyToVertexArray precondition");
	const pipeline::device::DeviceCapabilities &Capabilities = Device.GetCapabilities();

	for (const VertexBindingDescriptor &Binding : this->Bindings)
	{
		if (Binding.BindingIndex >= Capabilities.MaximumVertexBindings)
		{
			throw VertexBufferError("VertexDescriptor binding index exceeds OpenGL limits");
		}
	}
	for (const VertexAttributeDescriptor &Attribute : this->Attributes)
	{
		if (Attribute.Location >= Capabilities.MaximumVertexAttributes)
		{
			throw VertexBufferError("VertexDescriptor attribute location exceeds OpenGL limits");
		}
	}

	for (uint32 Location = 0; Location < Capabilities.MaximumVertexAttributes; ++Location)
	{
		glDisableVertexArrayAttrib(VertexArrayID, static_cast<GLuint>(Location));
	}
	for (uint32 Binding = 0; Binding < Capabilities.MaximumVertexBindings; ++Binding)
	{
		glVertexArrayBindingDivisor(VertexArrayID, static_cast<GLuint>(Binding), 0);
	}

	for (const VertexBindingDescriptor &Binding : this->Bindings)
	{
		glVertexArrayBindingDivisor(VertexArrayID, Binding.BindingIndex, Binding.InstanceStepRate);
	}

	for (const VertexAttributeDescriptor &Attribute : this->Attributes)
	{
		glEnableVertexArrayAttrib(VertexArrayID, Attribute.Location);
		glVertexArrayAttribBinding(VertexArrayID, Attribute.Location, Attribute.BindingIndex);

		const GLenum DataType = GetOpenGlType(Attribute.DataType);
		const GLuint RelativeOffset = static_cast<GLuint>(Attribute.RelativeOffsetInBytes);
		if (Attribute.Input == VertexAttributeInput::Integer)
		{
			glVertexArrayAttribIFormat(VertexArrayID, Attribute.Location, static_cast<GLint>(Attribute.ComponentCount), DataType,
									   RelativeOffset);
		}
		else if (Attribute.DataType == VertexAttributeDataType::Float64)
		{
			glVertexArrayAttribLFormat(VertexArrayID, Attribute.Location, static_cast<GLint>(Attribute.ComponentCount), DataType,
									   RelativeOffset);
		}
		else
		{
			glVertexArrayAttribFormat(VertexArrayID, Attribute.Location, static_cast<GLint>(Attribute.ComponentCount), DataType,
									  Attribute.Normalized ? GL_TRUE : GL_FALSE, RelativeOffset);
		}
	}

	Device.CheckErrors("VertexDescriptor::ApplyToVertexArray");
}

void VertexDescriptor::BindVertexBuffer(pipeline::device::Device &Device, GLuint VertexArrayID, GLuint BindingIndex,
										const VertexBuffer &Buffer, usize OffsetInBytes) const
{
	(void)Device.RequireCurrentContext();
	RequireVertexArray(VertexArrayID);
	if (&Buffer.GetDevice() != &Device)
	{
		throw VertexBufferError("VertexDescriptor cannot bind a VertexBuffer owned by another Device");
	}
	const VertexBindingDescriptor &Binding = this->GetBinding(BindingIndex);
	if (Buffer.IsMapped())
	{
		throw VertexBufferError("VertexDescriptor cannot bind a mapped VertexBuffer");
	}
	if (Buffer.GetStrideInBytes() != Binding.StrideInBytes)
	{
		throw VertexBufferError("VertexDescriptor binding stride does not match the VertexBuffer stride");
	}
	if (OffsetInBytes >= Buffer.GetSizeInBytes())
	{
		throw VertexBufferError("VertexDescriptor binding offset exceeds VertexBuffer storage");
	}
	if (OffsetInBytes > static_cast<usize>(std::numeric_limits<GLintptr>::max()))
	{
		throw VertexBufferError("VertexDescriptor binding offset exceeds OpenGL limits");
	}
	if (Binding.StrideInBytes > static_cast<usize>(std::numeric_limits<GLsizei>::max()))
	{
		throw VertexBufferError("VertexDescriptor binding stride exceeds OpenGL limits");
	}

	Device.CheckErrors("VertexDescriptor::bindVertexBuffer precondition");
	glVertexArrayVertexBuffer(VertexArrayID, BindingIndex, Buffer.GetID(), static_cast<GLintptr>(OffsetInBytes),
							  static_cast<GLsizei>(Binding.StrideInBytes));
	Device.CheckErrors("glVertexArrayVertexBuffer");
}

const VertexBindingDescriptor &VertexDescriptor::GetBinding(GLuint BindingIndex) const
{
	const VertexBindingDescriptor *Binding = this->FindBinding(BindingIndex);
	if (Binding == nullptr)
	{
		throw VertexBufferError("VertexDescriptor does not contain the requested vertex binding");
	}

	return *Binding;
}

std::span<const VertexBindingDescriptor> VertexDescriptor::GetBindings() const noexcept
{
	return this->Bindings;
}

std::span<const VertexAttributeDescriptor> VertexDescriptor::GetAttributes() const noexcept
{
	return this->Attributes;
}

uint64 VertexDescriptor::GetLayoutHash() const noexcept
{
	constexpr uint64 OffsetBasis = 14695981039346656037ULL;
	constexpr uint64 Prime = 1099511628211ULL;
	uint64 Hash = OffsetBasis;
	const auto Append = [&Hash, Prime](uint64 Value)
	{
		for (uint32 Byte = 0; Byte < sizeof(Value); ++Byte)
		{
			Hash ^= (Value >> (Byte * 8U)) & 0xFFU;
			Hash *= Prime;
		}
	};
	for (const VertexBindingDescriptor &Binding : this->Bindings)
	{
		Append(Binding.BindingIndex);
		Append(Binding.StrideInBytes);
		Append(static_cast<uint64>(Binding.InputRate));
		Append(Binding.InstanceStepRate);
	}
	for (const VertexAttributeDescriptor &Attribute : this->Attributes)
	{
		for (const auto SemanticCharacter : Attribute.Semantic)
			Append(static_cast<uint8>(SemanticCharacter));
		Append(Attribute.SemanticIndex);
		Append(Attribute.Location);
		Append(Attribute.BindingIndex);
		Append(static_cast<uint64>(Attribute.DataType));
		Append(Attribute.ComponentCount);
		Append(Attribute.Normalized ? 1U : 0U);
		Append(static_cast<uint64>(Attribute.Input));
		Append(Attribute.RelativeOffsetInBytes);
	}
	return Hash;
}

void VertexDescriptor::Validate() const
{
	if (this->Bindings.empty() || this->Attributes.empty())
	{
		throw VertexBufferError("VertexDescriptor requires at least one binding and one attribute");
	}

	for (const VertexBindingDescriptor &Binding : this->Bindings)
	{
		if (Binding.StrideInBytes == 0)
		{
			throw VertexBufferError("VertexDescriptor binding stride must be greater than zero");
		}
		if ((Binding.InputRate == VertexInputRate::PerVertex && Binding.InstanceStepRate != 0) ||
			(Binding.InputRate == VertexInputRate::PerInstance && Binding.InstanceStepRate == 0))
		{
			throw VertexBufferError("VertexDescriptor binding input rate and instance step rate are inconsistent");
		}
		if (std::count_if(this->Bindings.begin(), this->Bindings.end(),
						  [&Binding](const VertexBindingDescriptor &Other) { return Other.BindingIndex == Binding.BindingIndex; }) != 1)
		{
			throw VertexBufferError("VertexDescriptor contains duplicate binding indices");
		}
	}

	for (const VertexAttributeDescriptor &Attribute : this->Attributes)
	{
		const VertexBindingDescriptor *Binding = this->FindBinding(Attribute.BindingIndex);
		if (Binding == nullptr)
		{
			throw VertexBufferError("VertexDescriptor attribute references an unknown binding");
		}
		if (Attribute.ComponentCount == 0 || Attribute.ComponentCount > 4)
		{
			throw VertexBufferError("VertexDescriptor attribute component count must be between one and four");
		}
		if (Attribute.Input == VertexAttributeInput::Integer && (!IsIntegerType(Attribute.DataType) || Attribute.Normalized))
		{
			throw VertexBufferError("Integer VertexDescriptor attributes require an integer type and cannot be normalized");
		}
		if (Attribute.Normalized && !IsIntegerType(Attribute.DataType))
		{
			throw VertexBufferError("Only integer VertexDescriptor attributes can be normalized");
		}

		const usize AttributeSize = GetTypeSize(Attribute.DataType) * Attribute.ComponentCount;
		if (Attribute.RelativeOffsetInBytes > Binding->StrideInBytes ||
			AttributeSize > Binding->StrideInBytes - Attribute.RelativeOffsetInBytes)
		{
			throw VertexBufferError("VertexDescriptor attribute exceeds its binding stride");
		}
		if (Attribute.RelativeOffsetInBytes > static_cast<usize>(std::numeric_limits<GLuint>::max()))
		{
			throw VertexBufferError("VertexDescriptor attribute offset exceeds OpenGL limits");
		}
		if (std::count_if(this->Attributes.begin(), this->Attributes.end(),
						  [&Attribute](const VertexAttributeDescriptor &Other) { return Other.Location == Attribute.Location; }) != 1)
		{
			throw VertexBufferError("VertexDescriptor contains duplicate attribute locations");
		}
	}
}

const VertexBindingDescriptor *VertexDescriptor::FindBinding(GLuint BindingIndex) const noexcept
{
	const auto Iterator = std::find_if(this->Bindings.begin(), this->Bindings.end(), [BindingIndex](const VertexBindingDescriptor &Binding)
									   { return Binding.BindingIndex == BindingIndex; });
	return Iterator == this->Bindings.end() ? nullptr : &*Iterator;
}
} // namespace renderer
