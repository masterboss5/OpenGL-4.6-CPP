#pragma once

#include "src/types.h"

#include <array>
#include <atomic>
#include <compare>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core
{
class Context;
class WindowManager;
} // namespace core

namespace pipeline::shader
{
struct GraphicsPipelineState;
}

namespace pipeline::device
{
class RenderStateCache;

class ENGINE_API DeviceError final : public std::runtime_error
{
  public:
	explicit DeviceError(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};

enum class DeviceStatus : uint8
{
	Ready,
	Reset,
	Failed
};

enum class DiagnosticSeverity : uint8
{
	Notification,
	Low,
	Medium,
	High
};

struct DeviceDiagnostic final
{
	uint32 ID = 0;
	DiagnosticSeverity Severity = DiagnosticSeverity::Notification;
	std::string Message;
};

struct DeviceCapabilities final
{
	uint32 MajorVersion = 0;
	uint32 MinorVersion = 0;
	uint32 MaximumVertexAttributes = 0;
	uint32 MaximumVertexBindings = 0;
	uint32 MaximumTextureSize = 0;
	uint32 MaximumTextureUnits = 0;
	uint32 MaximumTextureArrayLayers = 0;
	uint32 MaximumCubeMapTextureSize = 0;
	uint32 MaximumColorAttachments = 0;
	uint32 MaximumDrawBuffers = 0;
	uint32 MaximumSamples = 0;
	uint32 MaximumComputeWorkGroupInvocations = 0;
	std::array<uint32, 3> MaximumComputeWorkGroupCount{};
	std::array<uint32, 3> MaximumComputeWorkGroupSize{};
	uint32 MaximumUniformBufferBindings = 0;
	uint32 MaximumShaderStorageBufferBindings = 0;
	uint32 UniformBufferOffsetAlignment = 0;
	uint32 ShaderStorageBufferOffsetAlignment = 0;
	uint64 MaximumUniformBlockSize = 0;
	uint64 MaximumShaderStorageBlockSize = 0;
	float32 MaximumAnisotropy = 1.0f;
	bool DebugOutput = false;
	bool Robustness = false;
	bool BindlessTextures = false;
	bool SparseTextures = false;
	bool IndirectParameters = false;
	bool ParallelShaderCompile = false;
};

enum class DeviceFormat : uint8
{
	Depth32Float,
	RGBA8SRGB,
	RGBA16Float,
	RG16Float,
	R32UnsignedInteger,
	R32Float,
	R8Unorm,
	Count
};

struct DeviceFormatCapabilities final
{
	bool Supported = false;
	bool ColorRenderable = false;
	bool DepthRenderable = false;
	bool Filterable = false;
	bool ShaderImageLoad = false;
	bool ShaderImageStore = false;
	std::vector<uint32> SampleCounts;
};

struct DeviceSync final
{
	uint64 Value = 0;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Value != 0;
	}
	auto operator<=>(const DeviceSync &) const = default;
};

enum class SyncWaitResult : uint8
{
	Signaled,
	Timeout
};

struct DeviceDebugCallback;
struct DeviceDebugCallbackState;
class Device;

struct DeviceLifetimeState final
{
	std::atomic<Device *> Owner = nullptr;
};

class DeviceHandle final
{
  public:
	DeviceHandle() = default;
	explicit DeviceHandle(Device &DeviceInstance) noexcept;
	[[nodiscard]] Device *TryGet() const noexcept;
	[[nodiscard]] Device &Get() const;
	[[nodiscard]] Device *operator->() const;
	[[nodiscard]] explicit operator bool() const noexcept
	{
		return this->TryGet() != nullptr;
	}

  private:
	std::shared_ptr<DeviceLifetimeState> State;
};

enum class GPUObjectType : uint8
{
	Buffer,
	Texture,
	VertexArray,
	Framebuffer,
	Program,
	ProgramPipeline
};

class ENGINE_API Device final
{
  public:
	explicit Device(core::Context &AnchorContext);
	~Device();

	Device(const Device &) = delete;
	Device &operator=(const Device &) = delete;
	Device(Device &&) = delete;
	Device &operator=(Device &&) = delete;

	[[nodiscard]] const DeviceCapabilities &GetCapabilities() const noexcept;
	[[nodiscard]] const DeviceFormatCapabilities &GetFormatCapabilities(DeviceFormat Format) const;
	[[nodiscard]] DeviceStatus GetStatus() const noexcept;
	[[nodiscard]] bool SupportsExtension(std::string_view Extension) const;
	[[nodiscard]] std::vector<DeviceDiagnostic> ConsumeDiagnostics();
	[[nodiscard]] core::Context &RequireCurrentContext() const;
	[[nodiscard]] bool CanIssueCommands() const noexcept;
	void CheckErrors(std::string_view Operation) const;
	void ValidateStatus();
	void ApplyGraphicsPipelineState(const pipeline::shader::GraphicsPipelineState &State) const;
	void InvalidateGraphicsPipelineStateCache() const noexcept;
	[[nodiscard]] DeviceSync CreateSync();
	[[nodiscard]] SyncWaitResult WaitSync(DeviceSync Sync, uint64 TimeoutNanoseconds, bool FlushCommands = true);
	void DestroySync(DeviceSync &Sync);
	[[nodiscard]] std::shared_ptr<DeviceLifetimeState> GetLifetimeState() const noexcept;
	void RetireGPUObject(GPUObjectType Type, uint32 Object, uint64 BindlessHandle = 0) noexcept;
	void NotifyFrameSubmitted(uint64 FrameNumber) noexcept;
	void CollectRetiredGPUObjects(uint64 CompletedFrameNumber);

  private:
	friend class core::WindowManager;
	friend struct DeviceDebugCallback;

	void RegisterContext(core::Context &Context);
	void UnregisterContext(core::Context &Context);
	void RecordDiagnostic(DeviceDiagnostic Diagnostic);
	void QueryCapabilities();
	void QueryFormatCapabilities();
	void ConfigureDiagnostics(core::Context &Context);
	void DisableDiagnostics(core::Context &Context) noexcept;

	core::Context *AnchorContext = nullptr;
	mutable std::mutex ContextMutex;
	std::vector<core::Context *> Contexts;
	DeviceCapabilities Capabilities;
	std::array<DeviceFormatCapabilities, static_cast<usize>(DeviceFormat::Count)> FormatCapabilities;
	std::unordered_set<std::string> Extensions;
	mutable std::mutex DiagnosticsMutex;
	std::vector<DeviceDiagnostic> Diagnostics;
	std::atomic<DeviceStatus> Status = DeviceStatus::Ready;
	std::mutex SyncMutex;
	std::unordered_map<uint64, void *> SyncObjects;
	uint64 NextSyncID = 1;
	mutable std::unordered_map<core::Context *, std::unique_ptr<RenderStateCache>> StateCaches;
	std::unique_ptr<DeviceDebugCallbackState> DebugCallbackState;
	std::shared_ptr<DeviceLifetimeState> LifetimeState;
	struct RetiredGPUObject final
	{
		GPUObjectType Type = GPUObjectType::Buffer;
		uint32 Object = 0;
		uint64 BindlessHandle = 0;
		uint64 RetireAfterFrame = 0;
	};
	std::mutex RetirementMutex;
	std::vector<RetiredGPUObject> RetiredGPUObjects;
	std::atomic<uint64> LatestSubmittedFrame = 0;
};

inline DeviceHandle::DeviceHandle(Device &DeviceInstance) noexcept : State(DeviceInstance.GetLifetimeState())
{
}

inline Device *DeviceHandle::TryGet() const noexcept
{
	return this->State != nullptr ? this->State->Owner.load(std::memory_order_acquire) : nullptr;
}

inline Device &DeviceHandle::Get() const
{
	Device *Owner = this->TryGet();
	if (Owner == nullptr)
		throw DeviceError("GPU resource outlived its Device");
	return *Owner;
}

inline Device *DeviceHandle::operator->() const
{
	return &this->Get();
}
} // namespace pipeline::device
