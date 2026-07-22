#pragma once

#include "src/types.h"

#include <compare>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core
{
struct WindowID final
{
	uint64 Value = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Value != 0;
	}
	auto operator<=>(const WindowID &) const = default;
};

struct MonitorID final
{
	std::string Value;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return !this->Value.empty();
	}
	auto operator<=>(const MonitorID &) const = default;
};

struct WindowExtent final
{
	uint32 Width = 0;
	uint32 Height = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Width != 0 && this->Height != 0;
	}
	[[nodiscard]] float32 AspectRatio() const noexcept
	{
		return this->Height == 0 ? 0.0f : static_cast<float32>(this->Width) / static_cast<float32>(this->Height);
	}
	auto operator<=>(const WindowExtent &) const = default;
};

struct WindowPosition final
{
	int32 X = 0;
	int32 Y = 0;
	auto operator<=>(const WindowPosition &) const = default;
};

struct WindowSizeConstraints final
{
	std::optional<WindowExtent> Minimum;
	std::optional<WindowExtent> Maximum;
	std::optional<WindowExtent> AspectRatio;
};

struct MonitorVideoMode final
{
	WindowExtent Extent;
	uint32 RedBits = 8;
	uint32 GreenBits = 8;
	uint32 BlueBits = 8;
	uint32 RefreshRate = 60;
	auto operator<=>(const MonitorVideoMode &) const = default;
};

struct MonitorInfo final
{
	MonitorID ID;
	std::string DisplayName;
	WindowPosition Position;
	WindowPosition WorkAreaPosition;
	WindowExtent WorkAreaExtent;
	uint32 PhysicalWidthMillimeters = 0;
	uint32 PhysicalHeightMillimeters = 0;
	float32 ContentScaleX = 1.0f;
	float32 ContentScaleY = 1.0f;
	MonitorVideoMode CurrentMode;
	std::vector<MonitorVideoMode> Modes;
	bool Primary = false;
};

enum class WindowMode : uint8
{
	Windowed,
	Borderless,
	ExclusiveFullscreen
};

enum class PresentationMode : uint8
{
	Off,
	On,
	Adaptive
};

enum class PresentationResult : uint8
{
	Applied,
	AppliedWithFallback,
	Unsupported
};

enum class WindowFeatureResult : uint8
{
	Applied,
	Unsupported
};

enum class CursorMode : uint8
{
	Visible,
	Hidden,
	Captured,
	Confined,
	Relative
};

enum class CursorShape : uint8
{
	Arrow,
	Text,
	Crosshair,
	PointingHand,
	ResizeHorizontal,
	ResizeVertical,
	ResizeNorthwestSoutheast,
	ResizeNortheastSouthwest,
	ResizeAll,
	NotAllowed
};

enum class WindowCornerPreference : uint8
{
	SystemDefault,
	Square,
	Rounded,
	RoundedSmall
};
enum class WindowBackdrop : uint8
{
	None,
	Mica,
	Acrylic,
	Tabbed
};
enum class WindowHitRegion : uint8
{
	Client,
	Caption,
	SystemMenu,
	Minimize,
	Maximize,
	Close,
	ResizeLeft,
	ResizeRight,
	ResizeTop,
	ResizeBottom,
	ResizeTopLeft,
	ResizeTopRight,
	ResizeBottomLeft,
	ResizeBottomRight
};
enum class WindowZOrder : uint8
{
	Bottom,
	Normal,
	Top,
	Topmost
};
enum class WindowSystemCommand : uint8
{
	Restore,
	Move,
	Size,
	Minimize,
	Maximize,
	Close
};
enum class TaskbarProgressState : uint8
{
	None,
	Indeterminate,
	Normal,
	Error,
	Paused
};
enum class NotificationSeverity : uint8
{
	Information,
	Warning,
	Error
};

struct TitleBarSpecification final
{
	bool Custom = false;
	uint32 DraggableHeight = 32;
	bool PreserveResizeBorder = true;
};

using WindowHitTest = std::function<WindowHitRegion(WindowPosition)>;

struct WindowImageView final
{
	const uint8 *Pixels = nullptr;
	WindowExtent Extent;
	uint32 BytesPerPixel = 4;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Pixels != nullptr && this->Extent.IsValid() && this->BytesPerPixel == 4;
	}
};

struct TransferredImage final
{
	WindowExtent Extent;
	std::vector<uint8> Rgba8;

	[[nodiscard]] bool IsValid() const noexcept
	{
		if (!this->Extent.IsValid())
			return false;
		const usize Width = this->Extent.Width;
		const usize Height = this->Extent.Height;
		if (Height > std::numeric_limits<usize>::max() / Width)
			return false;
		const usize Pixels = Width * Height;
		return Pixels <= std::numeric_limits<usize>::max() / 4 && this->Rgba8.size() == Pixels * 4;
	}
};

struct CustomDataPayload final
{
	std::string Format;
	std::vector<uint8> Bytes;
};

struct DataPayload final
{
	std::optional<std::string> Text;
	std::vector<std::filesystem::path> Files;
	std::optional<TransferredImage> Image;
	std::vector<CustomDataPayload> Custom;

	[[nodiscard]] bool Empty() const noexcept
	{
		return !this->Text && this->Files.empty() && !this->Image && this->Custom.empty();
	}
};

struct DataReadRequest final
{
	bool Text = true;
	bool Files = true;
	bool Image = true;
	bool AllNamedFormats = false;
	std::vector<std::string> NamedFormats;
	usize MaximumPayloadBytes = 64U * 1024U * 1024U;
};

enum class DataTransferOperation : uint8
{
	None,
	Copy,
	Move,
	Link
};

struct ThumbnailAction final
{
	uint16 ID = 0;
	std::string Title;
	std::optional<WindowImageView> Icon;
	bool Enabled = true;
	bool DismissOnClick = true;
	bool NoBackground = false;
};

struct WindowNotification final
{
	std::string Title;
	std::string Message;
	NotificationSeverity Severity = NotificationSeverity::Information;
};

struct WindowOperationResult final
{
	bool Succeeded = false;
	uint32 NativeCode = 0;
	std::string Diagnostic;
};

struct ApplicationIdentity final
{
	std::string ApplicationID;
	std::string DisplayName;
	std::string RelaunchCommand;
	std::string RelaunchDisplayName;
	std::filesystem::path IconResource;
};

struct JumpListItem final
{
	std::filesystem::path Executable;
	std::string Arguments;
	std::string Title;
	std::filesystem::path Icon;
	int32 IconIndex = 0;
};

struct JumpListCategory final
{
	std::string Name;
	std::vector<JumpListItem> Items;
};

struct JumpList final
{
	std::string ApplicationID;
	std::vector<JumpListCategory> Categories;
	bool IncludeRecent = true;
	bool IncludeFrequent = false;
};

struct FileAssociationVerb final
{
	std::string Name = "open";
	std::string DisplayName;
	std::string Command;
};

struct FileAssociation final
{
	std::string Extension;
	std::string ProgrammaticID;
	std::string Description;
	std::filesystem::path Icon;
	std::vector<FileAssociationVerb> Verbs;
};

struct WindowSpecification final
{
	std::string Title = "Engine";
	WindowExtent Extent{1280, 720};
	std::optional<WindowPosition> Position;
	WindowSizeConstraints Constraints;
	WindowMode Mode = WindowMode::Windowed;
	std::string ContextGroup = "Primary";
	bool Visible = true;
	bool Focused = true;
	bool Decorated = true;
	bool Resizable = true;
	bool Floating = false;
	bool TransparentFramebuffer = false;
	bool HeadlessValidation = false;
};

enum class WindowEventType : uint8
{
	CloseRequested,
	Closed,
	Moved,
	Resized,
	FramebufferResized,
	ContentScaleChanged,
	FocusChanged,
	VisibilityChanged,
	Minimized,
	Maximized,
	Restored,
	MonitorChanged,
	DisplayChanged,
	DPIChanged,
	ThemeChanged,
	PowerChanged,
	SessionChanged,
	DeviceChanged,
	AccessibilityChanged,
	MemoryPressure,
	ThumbnailActionInvoked,
	DataDropped
};

struct WindowEvent final
{
	WindowEventType Type = WindowEventType::Resized;
	WindowID Window;
	WindowExtent Extent;
	WindowPosition Position;
	float32 ContentScaleX = 1.0f;
	float32 ContentScaleY = 1.0f;
	bool State = false;
	uint32 CommandID = 0;
	WindowPosition DataPosition;
	DataTransferOperation DataOperation = DataTransferOperation::None;
	std::shared_ptr<const DataPayload> Data;
};
} // namespace core

template <> struct std::hash<core::WindowID>
{
	usize operator()(const core::WindowID ID) const noexcept
	{
		return std::hash<uint64>{}(ID.Value);
	}
};
