#pragma once

#include "src/types.h"

#include <array>
#include <compare>
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

namespace pipeline::device
{
class DeviceError final : public std::runtime_error
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

class Device final
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
	[[nodiscard]] DeviceSync CreateSync();
	[[nodiscard]] SyncWaitResult WaitSync(DeviceSync Sync, uint64 TimeoutNanoseconds, bool FlushCommands = true);
	void DestroySync(DeviceSync &Sync);

  private:
	friend class core::WindowManager;
	friend struct DeviceDebugCallback;

	void RegisterContext(core::Context &Context);
	void UnregisterContext(core::Context &Context) noexcept;
	void RecordDiagnostic(DeviceDiagnostic Diagnostic);
	void QueryCapabilities();
	void QueryFormatCapabilities();
	void ConfigureDiagnostics();

	core::Context *AnchorContext = nullptr;
	std::vector<core::Context *> Contexts;
	DeviceCapabilities Capabilities;
	std::array<DeviceFormatCapabilities, static_cast<usize>(DeviceFormat::Count)> FormatCapabilities;
	std::unordered_set<std::string> Extensions;
	mutable std::mutex DiagnosticsMutex;
	std::vector<DeviceDiagnostic> Diagnostics;
	DeviceStatus Status = DeviceStatus::Ready;
	std::mutex SyncMutex;
	std::unordered_map<uint64, void *> SyncObjects;
	uint64 NextSyncID = 1;
};
} // namespace pipeline::device
#include <unordered_map>
