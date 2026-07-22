#include "Device.h"

#include "src/core/window/Context.h"

#include <GL/glew.h>
#include <algorithm>
#include <sstream>

namespace pipeline::device
{
namespace
{
struct NativeFormat final
{
	DeviceFormat Format;
	GLenum InternalFormat;
	bool Depth;
};
constexpr std::array<NativeFormat, static_cast<usize>(DeviceFormat::Count)> NativeFormats{
	NativeFormat{DeviceFormat::Depth32Float, GL_DEPTH_COMPONENT32F, true},
	NativeFormat{DeviceFormat::RGBA16Float, GL_RGBA16F, false},
	NativeFormat{DeviceFormat::RG16Float, GL_RG16F, false},
	NativeFormat{DeviceFormat::R32UnsignedInteger, GL_R32UI, false},
	NativeFormat{DeviceFormat::R32Float, GL_R32F, false},
	NativeFormat{DeviceFormat::R8Unorm, GL_R8, false}};

[[nodiscard]] const char *GetErrorName(const GLenum Error) noexcept
{
	switch (Error)
	{
	case GL_INVALID_ENUM:
		return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE:
		return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION:
		return "GL_INVALID_OPERATION";
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		return "GL_INVALID_FRAMEBUFFER_OPERATION";
	case GL_OUT_OF_MEMORY:
		return "GL_OUT_OF_MEMORY";
	case GL_STACK_UNDERFLOW:
		return "GL_STACK_UNDERFLOW";
	case GL_STACK_OVERFLOW:
		return "GL_STACK_OVERFLOW";
	default:
		return "GL_UNKNOWN_ERROR";
	}
}

[[nodiscard]] DiagnosticSeverity ToSeverity(const GLenum Severity) noexcept
{
	if (Severity == GL_DEBUG_SEVERITY_HIGH)
		return DiagnosticSeverity::High;
	if (Severity == GL_DEBUG_SEVERITY_MEDIUM)
		return DiagnosticSeverity::Medium;
	if (Severity == GL_DEBUG_SEVERITY_LOW)
		return DiagnosticSeverity::Low;
	return DiagnosticSeverity::Notification;
}
} // namespace

struct DeviceDebugCallback final
{
	static void GLAPIENTRY Receive(GLenum, GLenum, GLuint ID, GLenum Severity, GLsizei, const GLchar *Message,
								   const void *UserData) noexcept
	{
		if (UserData == nullptr || Message == nullptr)
			return;
		try
		{
			auto *DeviceInstance = const_cast<Device *>(static_cast<const Device *>(UserData));
			DeviceInstance->RecordDiagnostic({.ID = static_cast<uint32>(ID), .Severity = ToSeverity(Severity), .Message = Message});
		}
		catch (...)
		{
			// Driver callbacks are native ABI boundaries. Diagnostics may be dropped
			// under allocation failure, but exceptions must never cross this boundary.
		}
	}
};

Device::Device(core::Context &AnchorContext) : AnchorContext(&AnchorContext)
{
	AnchorContext.RequireCurrentThread();
	glewExperimental = GL_TRUE;
	const GLenum Initialization = glewInit();
	if (Initialization != GLEW_OK)
	{
		this->Status = DeviceStatus::Failed;
		throw DeviceError("Failed to initialize OpenGL function dispatch: " +
						  std::string(reinterpret_cast<const char *>(glewGetErrorString(Initialization))));
	}
	(void)glGetError();
	if (GLEW_VERSION_4_6 == GL_FALSE)
	{
		this->Status = DeviceStatus::Failed;
		throw DeviceError("This engine requires OpenGL 4.6 core support");
	}
	this->RegisterContext(AnchorContext);
	this->QueryCapabilities();
	this->QueryFormatCapabilities();
	this->ConfigureDiagnostics();
	this->CheckErrors("Device initialization");
}

Device::~Device()
{
	if (this->AnchorContext != nullptr && this->AnchorContext->IsCurrent())
	{
		std::scoped_lock SyncLock(this->SyncMutex);
		for (const auto &[id, object] : this->SyncObjects)
			if (object != nullptr)
				glDeleteSync(static_cast<GLsync>(object));
		this->SyncObjects.clear();
		if (GLEW_VERSION_4_3 != GL_FALSE)
			glDebugMessageCallback(nullptr, nullptr);
	}
	else
		this->SyncObjects.clear();
	this->Contexts.clear();
	this->AnchorContext = nullptr;
}

const DeviceCapabilities &Device::GetCapabilities() const noexcept
{
	return this->Capabilities;
}
const DeviceFormatCapabilities &Device::GetFormatCapabilities(const DeviceFormat Format) const
{
	const usize Index = static_cast<usize>(Format);
	if (Index >= this->FormatCapabilities.size())
		throw DeviceError("Device format is out of range");
	return this->FormatCapabilities[Index];
}
DeviceStatus Device::GetStatus() const noexcept
{
	return this->Status;
}
bool Device::SupportsExtension(const std::string_view Extension) const
{
	return this->Extensions.contains(std::string(Extension));
}

std::vector<DeviceDiagnostic> Device::ConsumeDiagnostics()
{
	std::scoped_lock Lock(this->DiagnosticsMutex);
	std::vector<DeviceDiagnostic> Result;
	Result.swap(this->Diagnostics);
	return Result;
}

core::Context &Device::RequireCurrentContext() const
{
	if (this->Status != DeviceStatus::Ready)
		throw DeviceError("GPU operation attempted after Device reset or failure");
	const auto Iterator = std::find_if(this->Contexts.begin(), this->Contexts.end(),
									   [](const core::Context *Context) { return Context != nullptr && Context->IsCurrent(); });
	if (Iterator == this->Contexts.end())
		throw DeviceError("GPU operation requires a current Context owned by this Device");
	(*Iterator)->RequireCurrentThread();
	return **Iterator;
}

bool Device::CanIssueCommands() const noexcept
{
	if (this->Status != DeviceStatus::Ready)
		return false;
	for (const core::Context *Context : this->Contexts)
	{
		if (Context == nullptr || !Context->IsCurrent())
			continue;
		try
		{
			Context->RequireCurrentThread();
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	return false;
}

void Device::CheckErrors(const std::string_view Operation) const
{
	(void)this->RequireCurrentContext();
	std::ostringstream Diagnostic;
	bool Failed = false;
	for (GLenum Error = glGetError(); Error != GL_NO_ERROR; Error = glGetError())
	{
		if (!Failed)
			Diagnostic << Operation << " failed: ";
		else
			Diagnostic << ", ";
		Diagnostic << GetErrorName(Error);
		Failed = true;
	}
	if (Failed)
		throw DeviceError(Diagnostic.str());
}

void Device::ValidateStatus()
{
	(void)this->RequireCurrentContext();
	if (!this->Capabilities.Robustness)
		return;
	const GLenum Reset = glGetGraphicsResetStatus();
	if (Reset == GL_NO_ERROR)
		return;
	this->Status = DeviceStatus::Reset;
	for (core::Context *Context : this->Contexts)
		if (Context != nullptr)
			Context->MarkReset();
	throw DeviceError("OpenGL Context reset detected; orderly shutdown is required");
}

DeviceSync Device::CreateSync()
{
	(void)this->RequireCurrentContext();
	GLsync Object = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (Object == nullptr)
		throw DeviceError("GPU synchronization object creation failed");
	std::scoped_lock Lock(this->SyncMutex);
	if (this->NextSyncID == 0)
	{
		glDeleteSync(Object);
		throw DeviceError("GPU synchronization identifier space is exhausted");
	}
	const DeviceSync Sync{this->NextSyncID++};
	this->SyncObjects.emplace(Sync.Value, static_cast<void *>(Object));
	return Sync;
}

SyncWaitResult Device::WaitSync(const DeviceSync Sync, const uint64 TimeoutNanoseconds, const bool FlushCommands)
{
	(void)this->RequireCurrentContext();
	if (!Sync.IsValid())
		throw DeviceError("Cannot wait on an empty GPU synchronization token");
	std::scoped_lock Lock(this->SyncMutex);
	const auto Object = this->SyncObjects.find(Sync.Value);
	if (Object == this->SyncObjects.end())
		throw DeviceError("GPU synchronization token is not owned by this Device");
	const GLbitfield Flags = FlushCommands ? GL_SYNC_FLUSH_COMMANDS_BIT : 0;
	const GLenum Result = glClientWaitSync(static_cast<GLsync>(Object->second), Flags, TimeoutNanoseconds);
	if (Result == GL_ALREADY_SIGNALED || Result == GL_CONDITION_SATISFIED)
		return SyncWaitResult::Signaled;
	if (Result == GL_TIMEOUT_EXPIRED)
		return SyncWaitResult::Timeout;
	throw DeviceError("GPU synchronization wait failed");
}

void Device::DestroySync(DeviceSync &Sync)
{
	if (!Sync.IsValid())
		return;
	(void)this->RequireCurrentContext();
	std::scoped_lock Lock(this->SyncMutex);
	const auto Object = this->SyncObjects.find(Sync.Value);
	if (Object == this->SyncObjects.end())
		throw DeviceError("GPU synchronization token is not owned by this Device");
	glDeleteSync(static_cast<GLsync>(Object->second));
	this->SyncObjects.erase(Object);
	Sync = {};
}

void Device::RegisterContext(core::Context &Context)
{
	if (Context.GetShareGroupName() != this->AnchorContext->GetShareGroupName())
		throw DeviceError("Cannot register Context from a different share group");
	if (std::find(this->Contexts.begin(), this->Contexts.end(), &Context) == this->Contexts.end())
		this->Contexts.push_back(&Context);
	if (this->Capabilities.DebugOutput && Context.IsCurrent())
		this->ConfigureDiagnostics();
}

void Device::UnregisterContext(core::Context &Context) noexcept
{
	const auto Iterator = std::remove(this->Contexts.begin(), this->Contexts.end(), &Context);
	this->Contexts.erase(Iterator, this->Contexts.end());
}

void Device::RecordDiagnostic(DeviceDiagnostic Diagnostic)
{
	std::scoped_lock Lock(this->DiagnosticsMutex);
	this->Diagnostics.push_back(std::move(Diagnostic));
}

void Device::QueryCapabilities()
{
	GLint Value = 0;
	GLint64 WideValue = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &Value);
	this->Capabilities.MajorVersion = static_cast<uint32>(Value);
	glGetIntegerv(GL_MINOR_VERSION, &Value);
	this->Capabilities.MinorVersion = static_cast<uint32>(Value);
	if (this->Capabilities.MajorVersion != 4 || this->Capabilities.MinorVersion < 6)
		throw DeviceError("Device did not create an OpenGL 4.6 context");
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &Value);
	this->Capabilities.MaximumVertexAttributes = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_VERTEX_ATTRIB_BINDINGS, &Value);
	this->Capabilities.MaximumVertexBindings = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &Value);
	this->Capabilities.MaximumTextureSize = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &Value);
	this->Capabilities.MaximumTextureUnits = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &Value);
	this->Capabilities.MaximumTextureArrayLayers = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &Value);
	this->Capabilities.MaximumCubeMapTextureSize = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &Value);
	this->Capabilities.MaximumColorAttachments = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_DRAW_BUFFERS, &Value);
	this->Capabilities.MaximumDrawBuffers = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_SAMPLES, &Value);
	this->Capabilities.MaximumSamples = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &Value);
	this->Capabilities.MaximumComputeWorkGroupInvocations = static_cast<uint32>(Value);
	for (GLuint Dimension = 0; Dimension < 3; ++Dimension)
	{
		glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, Dimension, &Value);
		this->Capabilities.MaximumComputeWorkGroupCount[Dimension] = static_cast<uint32>(Value);
		glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, Dimension, &Value);
		this->Capabilities.MaximumComputeWorkGroupSize[Dimension] = static_cast<uint32>(Value);
	}
	glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &Value);
	this->Capabilities.MaximumUniformBufferBindings = static_cast<uint32>(Value);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &Value);
	this->Capabilities.MaximumShaderStorageBufferBindings = static_cast<uint32>(Value);
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &Value);
	this->Capabilities.UniformBufferOffsetAlignment = static_cast<uint32>(Value);
	glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &Value);
	this->Capabilities.ShaderStorageBufferOffsetAlignment = static_cast<uint32>(Value);
	glGetInteger64v(GL_MAX_UNIFORM_BLOCK_SIZE, &WideValue);
	this->Capabilities.MaximumUniformBlockSize = static_cast<uint64>(WideValue);
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &WideValue);
	this->Capabilities.MaximumShaderStorageBlockSize = static_cast<uint64>(WideValue);
	glGetIntegerv(GL_NUM_EXTENSIONS, &Value);
	for (GLint Index = 0; Index < Value; ++Index)
	{
		const GLubyte *Name = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(Index));
		if (Name != nullptr)
			this->Extensions.emplace(reinterpret_cast<const char *>(Name));
	}
	this->Capabilities.DebugOutput = this->SupportsExtension("GL_KHR_debug");
	this->Capabilities.Robustness = this->SupportsExtension("GL_KHR_robustness") || this->SupportsExtension("GL_ARB_robustness");
	this->Capabilities.BindlessTextures = this->SupportsExtension("GL_ARB_bindless_texture");
	this->Capabilities.SparseTextures = this->SupportsExtension("GL_ARB_sparse_texture");
	this->Capabilities.IndirectParameters = this->SupportsExtension("GL_ARB_indirect_parameters");
	this->Capabilities.ParallelShaderCompile = this->SupportsExtension("GL_ARB_parallel_shader_compile");
	if (this->SupportsExtension("GL_EXT_texture_filter_anisotropic"))
	{
		GLfloat Anisotropy = 1.0f;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &Anisotropy);
		this->Capabilities.MaximumAnisotropy = Anisotropy;
	}
}

void Device::QueryFormatCapabilities()
{
	for (const NativeFormat &Native : NativeFormats)
	{
		DeviceFormatCapabilities &Result = this->FormatCapabilities[static_cast<usize>(Native.Format)];
		GLint Value = 0;
		glGetInternalformativ(GL_TEXTURE_2D, Native.InternalFormat, GL_INTERNALFORMAT_SUPPORTED, 1, &Value);
		Result.Supported = Value == GL_TRUE;
		if (!Result.Supported)
			continue;
		glGetInternalformativ(GL_TEXTURE_2D, Native.InternalFormat, GL_FRAMEBUFFER_RENDERABLE, 1, &Value);
		Result.ColorRenderable = !Native.Depth && Value != GL_NONE;
		Result.DepthRenderable = Native.Depth && Value != GL_NONE;
		glGetInternalformativ(GL_TEXTURE_2D, Native.InternalFormat, GL_FILTER, 1, &Value);
		Result.Filterable = Value == GL_TRUE;
		glGetInternalformativ(GL_TEXTURE_2D, Native.InternalFormat, GL_SHADER_IMAGE_LOAD, 1, &Value);
		Result.ShaderImageLoad = Value != GL_NONE;
		glGetInternalformativ(GL_TEXTURE_2D, Native.InternalFormat, GL_SHADER_IMAGE_STORE, 1, &Value);
		Result.ShaderImageStore = Value != GL_NONE;
		glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, Native.InternalFormat, GL_NUM_SAMPLE_COUNTS, 1, &Value);
		if (Value > 0)
		{
			std::vector<GLint> Samples(static_cast<usize>(Value));
			glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, Native.InternalFormat, GL_SAMPLES, Value, Samples.data());
			Result.SampleCounts.reserve(Samples.size());
			for (const GLint Count : Samples)
				if (Count > 0)
					Result.SampleCounts.push_back(static_cast<uint32>(Count));
		}
	}
}

void Device::ConfigureDiagnostics()
{
	if (!this->Capabilities.DebugOutput)
		return;
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDebugMessageCallback(DeviceDebugCallback::Receive, this);
	glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
}
} // namespace pipeline::device
