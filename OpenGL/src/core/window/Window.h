#pragma once

#include "WindowDialogs.h"
#include "WindowException.h"
#include "WindowTypes.h"
#include "src/resource/asset/AssetHandle.h"
#include "src/resource/asset/Texture2DAsset.h"

#include <exception>
#include <memory>
#include <string>

namespace core
{
class Context;
class WindowManager;
struct WindowCallbacks;

class Window final
{
  public:
	~Window();

	Window(const Window &) = delete;
	Window &operator=(const Window &) = delete;
	Window(Window &&) = delete;
	Window &operator=(Window &&) = delete;

	[[nodiscard]] WindowID GetID() const noexcept;
	[[nodiscard]] const std::string &GetTitle() const noexcept;
	[[nodiscard]] WindowExtent GetExtent() const noexcept;
	[[nodiscard]] WindowExtent GetFramebufferExtent() const noexcept;
	[[nodiscard]] WindowPosition GetPosition() const noexcept;
	[[nodiscard]] float32 GetContentScale() const noexcept;
	[[nodiscard]] WindowMode GetMode() const noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	[[nodiscard]] bool IsFocused() const noexcept;
	[[nodiscard]] bool IsMinimized() const noexcept;
	[[nodiscard]] bool IsMaximized() const noexcept;
	[[nodiscard]] bool IsSRGBPresentationCapable() const noexcept;
	[[nodiscard]] bool ShouldClose() const;
	[[nodiscard]] Context &GetContext() const;

	void SetTitle(std::string Title);
	void SetExtent(WindowExtent Extent);
	void SetPosition(WindowPosition Position);
	void SetSizeConstraints(const WindowSizeConstraints &Constraints);
	void SetMode(WindowMode Mode);
	void SetMonitor(const MonitorID &Monitor, WindowMode Mode, std::optional<MonitorVideoMode> VideoMode = std::nullopt);
	void SetVisible(bool Visible);
	void SetEnabled(bool Enabled);
	void SetDecorated(bool Decorated);
	void SetResizable(bool Resizable);
	void SetTopmost(bool Topmost);
	void SetTaskbarVisible(bool Visible);
	void SetToolWindow(bool Enabled);
	void SetOwner(Window *Owner);
	void SetModal(bool Modal);
	void SetZOrder(WindowZOrder Order, Window *RelativeTo = nullptr);
	void SetOpacity(float32 Opacity);
	void SetMousePassthrough(bool Enabled);
	void SetCursorMode(CursorMode Mode);
	void SetCursorShape(CursorShape Shape);
	void SetCustomCursor(const WindowImageView &Image, WindowPosition HotSpot = {});
	void SetCustomCursor(const resource::AssetHandle<resource::Texture2DAsset> &Image, WindowPosition HotSpot = {});
	void SetIcon(const WindowImageView &Image);
	void SetIcon(const resource::AssetHandle<resource::Texture2DAsset> &Image);
	[[nodiscard]] WindowFeatureResult SetDarkTheme(bool Enabled);
	[[nodiscard]] WindowFeatureResult SetCornerPreference(WindowCornerPreference Preference);
	[[nodiscard]] WindowFeatureResult SetBackdrop(WindowBackdrop Backdrop);
	void SetTitleBar(const TitleBarSpecification &Specification, WindowHitTest HitTest = {});
	void SetSystemMenuCommandEnabled(WindowSystemCommand Command, bool Enabled);
	void ShowSystemMenu(WindowPosition ScreenPosition);
	void SetTaskbarProgress(TaskbarProgressState State, uint64 Completed = 0, uint64 Total = 0);
	void SetTaskbarOverlay(const WindowImageView &Image, std::string Description = {});
	void ClearTaskbarOverlay();
	void SetThumbnailActions(std::span<const ThumbnailAction> Actions);
	void ShowNotification(const WindowNotification &Notification);
	[[nodiscard]] WindowOperationResult SetApplicationIdentity(const ApplicationIdentity &Identity);
	[[nodiscard]] WindowOperationResult SetJumpList(const JumpList &JumpList);
	[[nodiscard]] WindowOperationResult AddRecentDocument(const std::filesystem::path &Path);
	[[nodiscard]] WindowOperationResult RegisterFileAssociation(const FileAssociation &Association);
	[[nodiscard]] WindowOperationResult UnregisterFileAssociation(const FileAssociation &Association);
	void SetClipboardData(const DataPayload &Payload);
	[[nodiscard]] DataPayload ReadClipboardData(const DataReadRequest &Request = {}) const;
	[[nodiscard]] std::vector<std::string> GetClipboardFormats() const;
	void ClearClipboard();
	[[nodiscard]] DialogResult<FileDialogSelection> ShowFileDialog(const FileDialogSpecification &Specification) const;
	[[nodiscard]] DialogResult<TaskDialogSelection> ShowTaskDialog(const TaskDialogSpecification &Specification) const;
	[[nodiscard]] DialogResult<DialogColor> ShowColorDialog(const ColorDialogSpecification &Specification) const;
	[[nodiscard]] DialogResult<FontSelection> ShowFontDialog(const FontDialogSpecification &Specification) const;
	[[nodiscard]] DialogResult<CredentialSelection> ShowCredentialDialog(const CredentialDialogSpecification &Specification) const;
	[[nodiscard]] DialogFuture<FileDialogSelection> BeginFileDialog(FileDialogSpecification Specification);
	[[nodiscard]] DialogFuture<TaskDialogSelection> BeginTaskDialog(TaskDialogSpecification Specification);
	[[nodiscard]] DialogFuture<DialogColor> BeginColorDialog(ColorDialogSpecification Specification);
	[[nodiscard]] DialogFuture<FontSelection> BeginFontDialog(FontDialogSpecification Specification);
	[[nodiscard]] DialogFuture<CredentialSelection> BeginCredentialDialog(CredentialDialogSpecification Specification);

	template <typename Value> DialogResult<Value> WaitForDialog(DialogFuture<Value> &Future)
	{
		if (!Future.IsValid())
			throw WindowException("Cannot wait for an empty DialogFuture");
		while (!Future.IsReady())
			this->PumpDialogEvents();
		return Future.Take();
	}
	void RequestFocus();
	void RequestAttention();
	void RequestClose();
	void CancelClose();
	void Minimize();
	void Maximize();
	void Restore();
	[[nodiscard]] PresentationResult SetPresentationMode(PresentationMode Mode);
	void Present();

  private:
	struct State;
	friend class Context;
	friend class DropTarget;
	friend class WindowManager;
	friend struct WindowCallbacks;

	Window(WindowManager &Manager, WindowID ID, const WindowSpecification &Specification, void *NativeWindow);
	[[nodiscard]] void *GetNativeWindow() const noexcept;
	void SetContext(Context &Context) noexcept;
	void Publish(WindowEvent Event);
	void PublishDrop(DataPayload Payload, WindowPosition Position, DataTransferOperation Operation);
	void InitializeDataTransfer();
	void ShutdownDataTransfer() noexcept;
	void RetargetDataTransfer() noexcept;
	void RebindNativeRouting();
	void DetachFromOwner(WindowID Owner) noexcept;
	void RecordNativeFailure(std::exception_ptr Exception) noexcept;
	void TrackDialog(std::shared_ptr<detail::DialogOperationBase> Operation);
	void WaitForTrackedDialogs() noexcept;
	void PumpDialogEvents();
	void RequireOwnerThread() const;
	[[nodiscard]] WindowManager &GetManager() const noexcept;
	void SetDataDropTarget(void *Target) noexcept;
	[[nodiscard]] void *GetDataDropTarget() const noexcept;
	[[nodiscard]] void *GetTaskbarService();

	std::unique_ptr<State> StateData;
};
} // namespace core
