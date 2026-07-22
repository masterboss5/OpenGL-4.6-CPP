#include "Window.h"
#include "WindowManager.h"
#include "src/concepts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <Ole2.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincred.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
namespace
{
[[nodiscard]] std::wstring ToWideDialog(const std::string_view Source)
{
	if (Source.empty())
		return {};
	if (Source.size() > static_cast<usize>(std::numeric_limits<int32>::max()))
		throw WindowException("Dialog text exceeds native conversion limits");
	const int32 Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0);
	if (Count <= 0)
		throw WindowException("Dialog text is not valid UTF-8");
	std::wstring Result(static_cast<usize>(Count), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Count) != Count)
		throw WindowException("Dialog text conversion failed");
	return Result;
}

[[nodiscard]] std::string ToUtf8Dialog(const std::wstring_view Source)
{
	if (Source.empty())
		return {};
	if (Source.size() > static_cast<usize>(std::numeric_limits<int32>::max()))
		throw WindowException("Dialog text exceeds native conversion limits");
	const int32 Count =
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0, nullptr, nullptr);
	if (Count <= 0)
		throw WindowException("Dialog Unicode conversion failed");
	std::string Result(static_cast<usize>(Count), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Count, nullptr,
							nullptr) != Count)
		throw WindowException("Dialog Unicode conversion failed");
	return Result;
}

class DialogApartment final
{
  public:
	DialogApartment()
	{
		const HRESULT Result = OleInitialize(nullptr);
		if (FAILED(Result))
			throw WindowException("Dialog thread OLE initialization failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
		this->Initialized = true;
	}
	~DialogApartment()
	{
		if (this->Initialized)
			OleUninitialize();
	}

  private:
	bool Initialized = false;
};

template <COMInterface Interface> class COMPointer final
{
  public:
	COMPointer() = default;
	~COMPointer()
	{
		this->Reset();
	}
	COMPointer(const COMPointer &) = delete;
	COMPointer &operator=(const COMPointer &) = delete;
	COMPointer(COMPointer &&Other) noexcept : Pointer(std::exchange(Other.Pointer, nullptr))
	{
	}
	COMPointer &operator=(COMPointer &&Other) noexcept
	{
		if (this != &Other)
		{
			this->Reset();
			this->Pointer = std::exchange(Other.Pointer, nullptr);
		}
		return *this;
	}
	[[nodiscard]] Interface *Get() const noexcept
	{
		return this->Pointer;
	}
	[[nodiscard]] Interface **Put() noexcept
	{
		this->Reset();
		return &this->Pointer;
	}
	[[nodiscard]] Interface *operator->() const noexcept
	{
		return this->Pointer;
	}
	void Reset() noexcept
	{
		if (this->Pointer != nullptr)
		{
			this->Pointer->Release();
			this->Pointer = nullptr;
		}
	}

  private:
	Interface *Pointer = nullptr;
};

void ApplyDarkDialogTheme(HWND Window) noexcept
{
	if (Window == nullptr)
		return;
	const BOOL Enabled = TRUE;
	(void)DwmSetWindowAttribute(Window, 20, &Enabled, sizeof(Enabled));
	(void)DwmSetWindowAttribute(Window, 19, &Enabled, sizeof(Enabled));
	(void)SetWindowTheme(Window, L"DarkMode_Explorer", nullptr);
}

LRESULT CALLBACK DarkDialogHook(const int Code, WPARAM Parameter, LPARAM Data)
{
	if (Code == HCBT_CREATEWND)
		ApplyDarkDialogTheme(reinterpret_cast<HWND>(Parameter));
	return CallNextHookEx(nullptr, Code, Parameter, Data);
}

class DarkDialogScope final
{
  public:
	DarkDialogScope() : Hook(SetWindowsHookExW(WH_CBT, DarkDialogHook, nullptr, GetCurrentThreadId()))
	{
		if (this->Hook == nullptr)
			throw WindowException("Dark dialog hook installation failed");
	}
	~DarkDialogScope()
	{
		if (this->Hook != nullptr)
			UnhookWindowsHookEx(this->Hook);
	}

  private:
	HHOOK Hook = nullptr;
};

[[nodiscard]] std::filesystem::path ShellItemPath(IShellItem &Item)
{
	PWSTR Value = nullptr;
	const HRESULT Result = Item.GetDisplayName(SIGDN_FILESYSPATH, &Value);
	if (FAILED(Result) || Value == nullptr)
		throw WindowException("Dialog selection is not a filesystem path");
	std::filesystem::path Path(Value);
	CoTaskMemFree(Value);
	return Path;
}

void ConfigureInitialFolder(IFileDialog &Dialog, const std::filesystem::path &Path)
{
	if (Path.empty())
		return;
	COMPointer<IShellItem> Folder;
	const HRESULT Result = SHCreateItemFromParsingName(Path.c_str(), nullptr, IID_PPV_ARGS(Folder.Put()));
	if (FAILED(Result))
		throw WindowException("Dialog initial directory is unavailable: " + Path.string());
	if (FAILED(Dialog.SetFolder(Folder.Get())))
		throw WindowException("Dialog initial directory could not be selected");
}

[[nodiscard]] DialogResult<FileDialogSelection> RunFileDialog(HWND Owner, const FileDialogSpecification &Specification)
{
	DialogApartment Apartment;
	DarkDialogScope Dark;
	if (Specification.Operation == FileDialogOperation::SelectFolder && !Specification.Filters.empty())
		throw WindowException("Folder dialogs cannot use file filters");
	COMPointer<IFileDialog> Dialog;
	const CLSID ClassID = Specification.Operation == FileDialogOperation::SaveFile ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
	HRESULT Result = CoCreateInstance(ClassID, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(Dialog.Put()));
	if (FAILED(Result))
		throw WindowException("File dialog creation failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));

	FILEOPENDIALOGOPTIONS Options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
	if (Specification.Operation == FileDialogOperation::OpenFiles)
		Options |= FOS_ALLOWMULTISELECT;
	if (Specification.Operation == FileDialogOperation::SelectFolder)
		Options |= FOS_PICKFOLDERS;
	if (Specification.ShowHidden)
		Options |= FOS_FORCESHOWHIDDEN;
	if (!Specification.AddToRecent)
		Options |= FOS_DONTADDTORECENT;
	if (Specification.RequireExistingPath && Specification.Operation != FileDialogOperation::SaveFile)
		Options |= FOS_PATHMUSTEXIST;
	if (Specification.RequireExistingPath &&
		(Specification.Operation == FileDialogOperation::OpenFile || Specification.Operation == FileDialogOperation::OpenFiles))
		Options |= FOS_FILEMUSTEXIST;
	if (Specification.ConfirmOverwrite && Specification.Operation == FileDialogOperation::SaveFile)
		Options |= FOS_OVERWRITEPROMPT;
	if (FAILED(Dialog->SetOptions(Options)))
		throw WindowException("File dialog options are invalid");

	const std::wstring Title = ToWideDialog(Specification.Title);
	const std::wstring InitialName = ToWideDialog(Specification.InitialName);
	const std::wstring DefaultExtension = ToWideDialog(Specification.DefaultExtension);
	if (!Title.empty() && FAILED(Dialog->SetTitle(Title.c_str())))
		throw WindowException("File dialog title could not be applied");
	if (!InitialName.empty() && FAILED(Dialog->SetFileName(InitialName.c_str())))
		throw WindowException("File dialog initial name could not be applied");
	if (!DefaultExtension.empty() && FAILED(Dialog->SetDefaultExtension(DefaultExtension.c_str())))
		throw WindowException("File dialog default extension could not be applied");
	ConfigureInitialFolder(*Dialog.Get(), Specification.InitialDirectory);

	std::vector<std::wstring> FilterNames;
	std::vector<std::wstring> FilterPatterns;
	FilterNames.reserve(Specification.Filters.size());
	FilterPatterns.reserve(Specification.Filters.size());
	for (const FileDialogFilter &Filter : Specification.Filters)
	{
		if (Filter.Name.empty() || Filter.Patterns.empty())
			throw WindowException("Every file-dialog filter requires a name and at least one pattern");
		FilterNames.push_back(ToWideDialog(Filter.Name));
		std::wstring Combined;
		for (const std::string &Pattern : Filter.Patterns)
		{
			if (Pattern.empty())
				throw WindowException("File-dialog filter patterns cannot be empty");
			if (!Combined.empty())
				Combined += L';';
			Combined += ToWideDialog(Pattern);
		}
		FilterPatterns.push_back(std::move(Combined));
	}
	std::vector<COMDLG_FILTERSPEC> NativeFilters;
	NativeFilters.reserve(Specification.Filters.size());
	for (usize Index = 0; Index < FilterNames.size(); ++Index)
		NativeFilters.push_back({FilterNames[Index].c_str(), FilterPatterns[Index].c_str()});
	if (!NativeFilters.empty() && FAILED(Dialog->SetFileTypes(static_cast<UINT>(NativeFilters.size()), NativeFilters.data())))
		throw WindowException("File-dialog filters could not be applied");

	Result = Dialog->Show(Owner);
	if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		return {};
	if (FAILED(Result))
		throw WindowException("File dialog failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));

	FileDialogSelection Selection;
	if (Specification.Operation == FileDialogOperation::OpenFiles)
	{
		COMPointer<IFileOpenDialog> OpenDialog;
		if (FAILED(Dialog->QueryInterface(IID_PPV_ARGS(OpenDialog.Put()))))
			throw WindowException("Multiple-selection dialog interface is unavailable");
		COMPointer<IShellItemArray> Items;
		if (FAILED(OpenDialog->GetResults(Items.Put())))
			throw WindowException("File dialog selections could not be read");
		DWORD Count = 0;
		if (FAILED(Items->GetCount(&Count)))
			throw WindowException("File dialog selection count could not be read");
		Selection.Paths.reserve(Count);
		for (DWORD Index = 0; Index < Count; ++Index)
		{
			COMPointer<IShellItem> Item;
			if (FAILED(Items->GetItemAt(Index, Item.Put())))
				throw WindowException("File dialog selection could not be read");
			Selection.Paths.push_back(ShellItemPath(*Item.Get()));
		}
	}
	else
	{
		COMPointer<IShellItem> Item;
		if (FAILED(Dialog->GetResult(Item.Put())))
			throw WindowException("File dialog selection could not be read");
		Selection.Paths.push_back(ShellItemPath(*Item.Get()));
	}
	if (Selection.Paths.empty())
		throw WindowException("An accepted file dialog returned no selections");
	return {.Status = DialogStatus::Accepted, .Value = std::move(Selection)};
}

[[nodiscard]] PCWSTR TaskIcon(const TaskDialogSeverity Severity) noexcept
{
	switch (Severity)
	{
	case TaskDialogSeverity::Warning:
		return TD_WARNING_ICON;
	case TaskDialogSeverity::Error:
		return TD_ERROR_ICON;
	case TaskDialogSeverity::Shield:
		return TD_SHIELD_ICON;
	default:
		return TD_INFORMATION_ICON;
	}
}

HRESULT CALLBACK TaskDialogCallback(HWND Window, UINT Notification, WPARAM, LPARAM, LONG_PTR)
{
	if (Notification == TDN_CREATED)
		ApplyDarkDialogTheme(Window);
	return S_OK;
}

[[nodiscard]] DialogResult<TaskDialogSelection> RunTaskDialog(HWND Owner, const TaskDialogSpecification &Specification)
{
	DialogApartment Apartment;
	DarkDialogScope Dark;
	const std::wstring Title = ToWideDialog(Specification.Title);
	const std::wstring Instruction = ToWideDialog(Specification.Instruction);
	const std::wstring Content = ToWideDialog(Specification.Content);
	const std::wstring Expanded = ToWideDialog(Specification.ExpandedInformation);
	const std::wstring Verification = ToWideDialog(Specification.VerificationText);
	std::vector<std::wstring> Labels;
	Labels.reserve(Specification.Buttons.size());
	for (const TaskDialogButton &Button : Specification.Buttons)
	{
		if (Button.ID == 0 || Button.ID > static_cast<uint32>(std::numeric_limits<int32>::max()) || Button.Text.empty())
			throw WindowException("Task-dialog buttons require a non-zero native-range ID and text");
		Labels.push_back(ToWideDialog(Button.Text));
	}
	std::vector<TASKDIALOG_BUTTON> Buttons;
	Buttons.reserve(Labels.size());
	int32 DefaultButton = 0;
	for (usize Index = 0; Index < Labels.size(); ++Index)
	{
		Buttons.push_back({static_cast<int32>(Specification.Buttons[Index].ID), Labels[Index].c_str()});
		if (Specification.Buttons[Index].IsDefault)
		{
			if (DefaultButton != 0)
				throw WindowException("Task dialog cannot have more than one default button");
			DefaultButton = static_cast<int32>(Specification.Buttons[Index].ID);
		}
	}
	TASKDIALOGCONFIG Config{};
	Config.cbSize = sizeof(Config);
	Config.hwndParent = Owner;
	Config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
	if (Specification.AllowCancellation)
		Config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;
	if (Specification.VerificationChecked)
		Config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
	Config.dwCommonButtons = Buttons.empty() ? TDCBF_OK_BUTTON : 0;
	Config.pszWindowTitle = Title.empty() ? nullptr : Title.c_str();
	Config.pszMainInstruction = Instruction.empty() ? nullptr : Instruction.c_str();
	Config.pszContent = Content.empty() ? nullptr : Content.c_str();
	Config.pszExpandedInformation = Expanded.empty() ? nullptr : Expanded.c_str();
	Config.pszVerificationText = Verification.empty() ? nullptr : Verification.c_str();
	Config.pszMainIcon = TaskIcon(Specification.Severity);
	Config.cButtons = static_cast<UINT>(Buttons.size());
	Config.pButtons = Buttons.empty() ? nullptr : Buttons.data();
	Config.nDefaultButton = DefaultButton;
	Config.pfCallback = TaskDialogCallback;
	int32 SelectedButton = 0;
	BOOL VerificationChecked = FALSE;
	const HRESULT Result = TaskDialogIndirect(&Config, &SelectedButton, nullptr, &VerificationChecked);
	if (FAILED(Result))
		throw WindowException("Task dialog failed with HRESULT " + std::to_string(static_cast<uint32>(Result)));
	if (SelectedButton == IDCANCEL)
		return {};
	return {.Status = DialogStatus::Accepted,
			.Value =
				TaskDialogSelection{.ButtonID = static_cast<uint32>(SelectedButton), .VerificationChecked = VerificationChecked != FALSE}};
}

[[nodiscard]] COLORREF ToColorRef(const DialogColor Color) noexcept
{
	return RGB(Color.Red, Color.Green, Color.Blue);
}
[[nodiscard]] DialogColor FromColorRef(const COLORREF Color) noexcept
{
	return {GetRValue(Color), GetGValue(Color), GetBValue(Color), 255};
}

UINT_PTR CALLBACK CommonDialogHook(HWND Window, UINT Message, WPARAM, LPARAM)
{
	if (Message == WM_INITDIALOG)
		ApplyDarkDialogTheme(Window);
	return 0;
}

[[nodiscard]] DialogResult<DialogColor> RunColorDialog(HWND Owner, const ColorDialogSpecification &Specification)
{
	DialogApartment Apartment;
	DarkDialogScope Dark;
	if (Specification.CustomColors.size() > 16)
		throw WindowException("Color dialogs support at most 16 custom colors");
	std::array<COLORREF, 16> Custom{};
	for (usize Index = 0; Index < Specification.CustomColors.size(); ++Index)
		Custom[Index] = ToColorRef(Specification.CustomColors[Index]);
	CHOOSECOLORW Config{};
	Config.lStructSize = sizeof(Config);
	Config.hwndOwner = Owner;
	Config.rgbResult = ToColorRef(Specification.Initial);
	Config.lpCustColors = Custom.data();
	Config.Flags = CC_RGBINIT | CC_ENABLEHOOK;
	if (Specification.FullOpen)
		Config.Flags |= CC_FULLOPEN;
	if (Specification.AllowSolidOnly)
		Config.Flags |= CC_SOLIDCOLOR;
	Config.lpfnHook = CommonDialogHook;
	if (ChooseColorW(&Config) == FALSE)
	{
		const DWORD Error = CommDlgExtendedError();
		if (Error == 0)
			return {};
		throw WindowException("Color dialog failed with native error " + std::to_string(static_cast<uint32>(Error)));
	}
	DialogColor Result = FromColorRef(Config.rgbResult);
	Result.Alpha = Specification.Initial.Alpha;
	return {.Status = DialogStatus::Accepted, .Value = Result};
}

[[nodiscard]] DialogResult<FontSelection> RunFontDialog(HWND Owner, const FontDialogSpecification &Specification)
{
	DialogApartment Apartment;
	DarkDialogScope Dark;
	if (Specification.MinimumPointSize <= 0.0f || Specification.MaximumPointSize < Specification.MinimumPointSize)
		throw WindowException("Font dialog point-size limits are invalid");
	LOGFONTW Font{};
	const std::wstring Family = ToWideDialog(Specification.Initial.Family);
	if (Family.size() >= LF_FACESIZE)
		throw WindowException("Initial font family name exceeds the native limit");
	if (!Family.empty())
		std::copy(Family.begin(), Family.end(), Font.lfFaceName);
	const uint32 DPI = Owner == nullptr ? 96U : GetDpiForWindow(Owner);
	Font.lfHeight = -static_cast<LONG>(std::lround(Specification.Initial.PointSize * static_cast<float32>(DPI) / 72.0f));
	Font.lfWeight = Specification.Initial.Weight;
	Font.lfItalic = Specification.Initial.Italic ? TRUE : FALSE;
	Font.lfUnderline = Specification.Initial.Underline ? TRUE : FALSE;
	Font.lfStrikeOut = Specification.Initial.Strikeout ? TRUE : FALSE;
	CHOOSEFONTW Config{};
	Config.lStructSize = sizeof(Config);
	Config.hwndOwner = Owner;
	Config.lpLogFont = &Font;
	Config.iPointSize = static_cast<int32>(std::lround(Specification.Initial.PointSize * 10.0f));
	Config.rgbColors = ToColorRef(Specification.Initial.Color);
	Config.Flags = CF_INITTOLOGFONTSTRUCT | CF_ENABLEHOOK | CF_LIMITSIZE;
	if (Specification.ScreenFontsOnly)
		Config.Flags |= CF_SCREENFONTS;
	if (Specification.Effects)
		Config.Flags |= CF_EFFECTS;
	Config.nSizeMin = static_cast<int32>(std::floor(Specification.MinimumPointSize));
	Config.nSizeMax = static_cast<int32>(std::ceil(Specification.MaximumPointSize));
	Config.lpfnHook = CommonDialogHook;
	if (ChooseFontW(&Config) == FALSE)
	{
		const DWORD Error = CommDlgExtendedError();
		if (Error == 0)
			return {};
		throw WindowException("Font dialog failed with native error " + std::to_string(static_cast<uint32>(Error)));
	}
	FontSelection Selection;
	Selection.Family = ToUtf8Dialog(Font.lfFaceName);
	Selection.PointSize = static_cast<float32>(Config.iPointSize) / 10.0f;
	Selection.Weight = static_cast<uint16>(std::clamp<LONG>(Font.lfWeight, 0, std::numeric_limits<uint16>::max()));
	Selection.Italic = Font.lfItalic != FALSE;
	Selection.Underline = Font.lfUnderline != FALSE;
	Selection.Strikeout = Font.lfStrikeOut != FALSE;
	Selection.Color = FromColorRef(Config.rgbColors);
	return {.Status = DialogStatus::Accepted, .Value = std::move(Selection)};
}

[[nodiscard]] DialogResult<CredentialSelection> RunCredentialDialog(HWND Owner, const CredentialDialogSpecification &Specification)
{
	DialogApartment Apartment;
	DarkDialogScope Dark;
	if (Specification.TargetName.empty())
		throw WindowException("Credential dialogs require a non-empty target name");
	const std::wstring Target = ToWideDialog(Specification.TargetName);
	const std::wstring Title = ToWideDialog(Specification.Title);
	const std::wstring Message = ToWideDialog(Specification.Message);
	const std::wstring InitialUser = ToWideDialog(Specification.InitialUserName);
	CREDUI_INFOW Info{};
	Info.cbSize = sizeof(Info);
	Info.hwndParent = Owner;
	Info.pszCaptionText = Title.empty() ? nullptr : Title.c_str();
	Info.pszMessageText = Message.empty() ? nullptr : Message.c_str();
	ULONG Package = 0;
	std::vector<uint8> InitialAuthentication;
	if (!InitialUser.empty())
	{
		DWORD RequiredBytes = 0;
		wchar_t EmptyPassword[]{L'\0'};
		(void)CredPackAuthenticationBufferW(CRED_PACK_GENERIC_CREDENTIALS, const_cast<wchar_t *>(InitialUser.c_str()), EmptyPassword,
											nullptr, &RequiredBytes);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || RequiredBytes == 0)
			throw WindowException("Initial credential identity could not be encoded");
		InitialAuthentication.resize(RequiredBytes);
		if (CredPackAuthenticationBufferW(CRED_PACK_GENERIC_CREDENTIALS, const_cast<wchar_t *>(InitialUser.c_str()), EmptyPassword,
										  InitialAuthentication.data(), &RequiredBytes) == FALSE)
			throw WindowException("Initial credential identity encoding failed");
	}
	struct ByteScrubber final
	{
		std::vector<uint8> &Value;
		~ByteScrubber()
		{
			if (!Value.empty())
				SecureZeroMemory(Value.data(), Value.size());
		}
	} InitialAuthenticationScrubber{InitialAuthentication};
	void *Authentication = nullptr;
	ULONG AuthenticationBytes = 0;
	BOOL Save = Specification.SaveChecked ? TRUE : FALSE;
	DWORD Flags = CREDUIWIN_GENERIC;
	if (Specification.AllowSave)
		Flags |= CREDUIWIN_CHECKBOX;
	const DWORD Result = CredUIPromptForWindowsCredentialsW(
		&Info, 0, &Package, InitialAuthentication.empty() ? nullptr : InitialAuthentication.data(),
		static_cast<ULONG>(InitialAuthentication.size()), &Authentication, &AuthenticationBytes, &Save, Flags);
	if (Result == ERROR_CANCELLED)
		return {};
	if (Result != ERROR_SUCCESS)
		throw WindowException("Credential dialog failed with native error " + std::to_string(static_cast<uint32>(Result)));

	struct AuthenticationGuard final
	{
		void *Memory;
		ULONG Size;
		~AuthenticationGuard()
		{
			if (Memory != nullptr)
			{
				SecureZeroMemory(Memory, Size);
				CoTaskMemFree(Memory);
			}
		}
	} Guard{Authentication, AuthenticationBytes};
	DWORD UserCount = 0;
	DWORD DomainCount = 0;
	DWORD PasswordCount = 0;
	(void)CredUnPackAuthenticationBufferW(0, Authentication, AuthenticationBytes, nullptr, &UserCount, nullptr, &DomainCount, nullptr,
										  &PasswordCount);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		throw WindowException("Credential result size query failed");
	std::vector<wchar_t> User(UserCount);
	std::vector<wchar_t> Domain(DomainCount);
	std::vector<wchar_t> Password(PasswordCount);
	struct PasswordScrubber final
	{
		std::vector<wchar_t> &Value;
		~PasswordScrubber()
		{
			if (!Value.empty())
				SecureZeroMemory(Value.data(), Value.size() * sizeof(wchar_t));
		}
	} PasswordScrubber{Password};
	if (CredUnPackAuthenticationBufferW(0, Authentication, AuthenticationBytes, User.data(), &UserCount, Domain.data(), &DomainCount,
										Password.data(), &PasswordCount) == FALSE)
	{
		throw WindowException("Credential result decoding failed with native error " + std::to_string(static_cast<uint32>(GetLastError())));
	}
	CredentialSelection Selection;
	Selection.UserName = ToUtf8Dialog(User.data());
	Selection.Domain = ToUtf8Dialog(Domain.data());
	std::string SecretText = ToUtf8Dialog(Password.data());
	struct TextScrubber final
	{
		std::string &Value;
		~TextScrubber()
		{
			if (!Value.empty())
				SecureZeroMemory(Value.data(), Value.size());
		}
	} SecretTextScrubber{SecretText};
	Selection.Secret = SecureBuffer(std::span<const uint8>(reinterpret_cast<const uint8 *>(SecretText.data()), SecretText.size()));
	Selection.SaveChecked = Specification.AllowSave && Save != FALSE;
	return {.Status = DialogStatus::Accepted, .Value = std::move(Selection)};
}

} // namespace

SecureBuffer::SecureBuffer(const std::span<const uint8> Bytes)
	: Memory(Bytes.empty() ? nullptr : std::make_unique<uint8[]>(Bytes.size())), ByteCount(Bytes.size())
{
	if (!Bytes.empty())
		std::memcpy(this->Memory.get(), Bytes.data(), Bytes.size());
}

SecureBuffer::~SecureBuffer()
{
	this->Clear();
}
SecureBuffer::SecureBuffer(SecureBuffer &&Other) noexcept : Memory(std::move(Other.Memory)), ByteCount(std::exchange(Other.ByteCount, 0))
{
}
SecureBuffer &SecureBuffer::operator=(SecureBuffer &&Other) noexcept
{
	if (this != &Other)
	{
		this->Clear();
		this->Memory = std::move(Other.Memory);
		this->ByteCount = std::exchange(Other.ByteCount, 0);
	}
	return *this;
}
std::span<const uint8> SecureBuffer::Bytes() const noexcept
{
	return {this->Memory.get(), this->ByteCount};
}
bool SecureBuffer::Empty() const noexcept
{
	return this->ByteCount == 0;
}
void SecureBuffer::Clear() noexcept
{
	if (this->Memory != nullptr && this->ByteCount != 0)
		SecureZeroMemory(this->Memory.get(), this->ByteCount);
	this->Memory.reset();
	this->ByteCount = 0;
}

DialogResult<FileDialogSelection> Window::ShowFileDialog(const FileDialogSpecification &Specification) const
{
	this->RequireOwnerThread();
	return RunFileDialog(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), Specification);
}
DialogResult<TaskDialogSelection> Window::ShowTaskDialog(const TaskDialogSpecification &Specification) const
{
	this->RequireOwnerThread();
	return RunTaskDialog(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), Specification);
}
DialogResult<DialogColor> Window::ShowColorDialog(const ColorDialogSpecification &Specification) const
{
	this->RequireOwnerThread();
	return RunColorDialog(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), Specification);
}
DialogResult<FontSelection> Window::ShowFontDialog(const FontDialogSpecification &Specification) const
{
	this->RequireOwnerThread();
	return RunFontDialog(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), Specification);
}
DialogResult<CredentialSelection> Window::ShowCredentialDialog(const CredentialDialogSpecification &Specification) const
{
	this->RequireOwnerThread();
	return RunCredentialDialog(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), Specification);
}

DialogFuture<FileDialogSelection> Window::BeginFileDialog(FileDialogSpecification Specification)
{
	this->RequireOwnerThread();
	HWND Owner = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	auto Operation = detail::DialogOperation<FileDialogSelection>::Start([Owner, Specification = std::move(Specification)]
																		 { return RunFileDialog(Owner, Specification); },
																		 [this] { this->GetManager().WakeEventLoop(); });
	this->TrackDialog(Operation);
	return DialogFuture<FileDialogSelection>(std::move(Operation));
}
DialogFuture<TaskDialogSelection> Window::BeginTaskDialog(TaskDialogSpecification Specification)
{
	this->RequireOwnerThread();
	HWND Owner = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	auto Operation = detail::DialogOperation<TaskDialogSelection>::Start([Owner, Specification = std::move(Specification)]
																		 { return RunTaskDialog(Owner, Specification); },
																		 [this] { this->GetManager().WakeEventLoop(); });
	this->TrackDialog(Operation);
	return DialogFuture<TaskDialogSelection>(std::move(Operation));
}
DialogFuture<DialogColor> Window::BeginColorDialog(ColorDialogSpecification Specification)
{
	this->RequireOwnerThread();
	HWND Owner = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	auto Operation = detail::DialogOperation<DialogColor>::Start([Owner, Specification = std::move(Specification)]
																 { return RunColorDialog(Owner, Specification); },
																 [this] { this->GetManager().WakeEventLoop(); });
	this->TrackDialog(Operation);
	return DialogFuture<DialogColor>(std::move(Operation));
}
DialogFuture<FontSelection> Window::BeginFontDialog(FontDialogSpecification Specification)
{
	this->RequireOwnerThread();
	HWND Owner = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	auto Operation = detail::DialogOperation<FontSelection>::Start([Owner, Specification = std::move(Specification)]
																   { return RunFontDialog(Owner, Specification); },
																   [this] { this->GetManager().WakeEventLoop(); });
	this->TrackDialog(Operation);
	return DialogFuture<FontSelection>(std::move(Operation));
}
DialogFuture<CredentialSelection> Window::BeginCredentialDialog(CredentialDialogSpecification Specification)
{
	this->RequireOwnerThread();
	HWND Owner = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	auto Operation = detail::DialogOperation<CredentialSelection>::Start([Owner, Specification = std::move(Specification)]
																		 { return RunCredentialDialog(Owner, Specification); },
																		 [this] { this->GetManager().WakeEventLoop(); });
	this->TrackDialog(Operation);
	return DialogFuture<CredentialSelection>(std::move(Operation));
}
} // namespace core
