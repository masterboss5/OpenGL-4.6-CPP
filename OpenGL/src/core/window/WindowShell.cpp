#include "Window.h"
#include "src/concepts.h"

#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
namespace
{
template <COMInterface Interface> class COMPointer final
{
  public:
	~COMPointer()
	{
		if (this->Pointer != nullptr)
			this->Pointer->Release();
	}
	COMPointer() = default;
	COMPointer(const COMPointer &) = delete;
	COMPointer &operator=(const COMPointer &) = delete;
	[[nodiscard]] Interface *Get() const noexcept
	{
		return this->Pointer;
	}
	[[nodiscard]] Interface **Put() noexcept
	{
		if (this->Pointer != nullptr)
		{
			this->Pointer->Release();
			this->Pointer = nullptr;
		}
		return &this->Pointer;
	}
	[[nodiscard]] Interface *operator->() const noexcept
	{
		return this->Pointer;
	}

  private:
	Interface *Pointer = nullptr;
};

[[nodiscard]] std::wstring ToWideShell(const std::string_view Source)
{
	if (Source.empty())
		return {};
	const int32 Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0);
	if (Count <= 0)
		return {};
	std::wstring Result(static_cast<usize>(Count), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Count) != Count)
		return {};
	return Result;
}

[[nodiscard]] WindowOperationResult HresultFailure(const std::string_view Operation, const HRESULT Result)
{
	return {.NativeCode = static_cast<uint32>(Result),
			.Diagnostic = std::string(Operation) + " failed with HRESULT " + std::to_string(static_cast<uint32>(Result))};
}

[[nodiscard]] WindowOperationResult NativeFailure(const std::string_view Operation, const LSTATUS Result)
{
	return {.NativeCode = static_cast<uint32>(Result),
			.Diagnostic = std::string(Operation) + " failed with native code " + std::to_string(static_cast<uint32>(Result))};
}

[[nodiscard]] HRESULT SetStringProperty(IPropertyStore &Store, const PROPERTYKEY &Key, const std::wstring &Value)
{
	PROPVARIANT Property;
	PropVariantInit(&Property);
	HRESULT Result = InitPropVariantFromString(Value.c_str(), &Property);
	if (SUCCEEDED(Result))
		Result = Store.SetValue(Key, Property);
	PropVariantClear(&Property);
	return Result;
}

[[nodiscard]] LSTATUS SetRegistryString(HKEY Root, const std::wstring &Subkey, const wchar_t *ValueName, const std::wstring &Value)
{
	HKEY Key = nullptr;
	LSTATUS Result = RegCreateKeyExW(Root, Subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &Key, nullptr);
	if (Result == ERROR_SUCCESS)
	{
		Result = RegSetValueExW(Key, ValueName, 0, REG_SZ, reinterpret_cast<const uint8 *>(Value.c_str()),
								static_cast<DWORD>((Value.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(Key);
	}
	return Result;
}
} // namespace

WindowOperationResult Window::SetApplicationIdentity(const ApplicationIdentity &Identity)
{
	this->RequireOwnerThread();
	if (Identity.ApplicationID.empty())
		return {.Diagnostic = "Application identity requires a non-empty application ID"};
	const std::wstring ApplicationID = ToWideShell(Identity.ApplicationID);
	if (ApplicationID.empty())
		return {.Diagnostic = "Application ID is not valid UTF-8"};
	COMPointer<IPropertyStore> Properties;
	HRESULT Result =
		SHGetPropertyStoreForWindow(glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow())), IID_PPV_ARGS(Properties.Put()));
	if (FAILED(Result))
		return HresultFailure("Window property-store acquisition", Result);
	Result = SetStringProperty(*Properties.Get(), PKEY_AppUserModel_ID, ApplicationID);
	if (SUCCEEDED(Result) && !Identity.RelaunchCommand.empty())
		Result = SetStringProperty(*Properties.Get(), PKEY_AppUserModel_RelaunchCommand, ToWideShell(Identity.RelaunchCommand));
	if (SUCCEEDED(Result) && !Identity.RelaunchDisplayName.empty())
		Result =
			SetStringProperty(*Properties.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource, ToWideShell(Identity.RelaunchDisplayName));
	if (SUCCEEDED(Result) && !Identity.IconResource.empty())
		Result = SetStringProperty(*Properties.Get(), PKEY_AppUserModel_RelaunchIconResource, Identity.IconResource.wstring());
	if (SUCCEEDED(Result))
		Result = Properties->Commit();
	return FAILED(Result) ? HresultFailure("Application identity update", Result) : WindowOperationResult{.Succeeded = true};
}

WindowOperationResult Window::SetJumpList(const JumpList &JumpList)
{
	this->RequireOwnerThread();
	if (JumpList.ApplicationID.empty())
		return {.Diagnostic = "Jump list requires a non-empty application ID"};
	COMPointer<ICustomDestinationList> Destinations;
	HRESULT Result = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(Destinations.Put()));
	if (FAILED(Result))
		return HresultFailure("Jump-list service creation", Result);
	Result = Destinations->SetAppID(ToWideShell(JumpList.ApplicationID).c_str());
	UINT MinimumSlots = 0;
	COMPointer<IObjectArray> Removed;
	if (SUCCEEDED(Result))
		Result = Destinations->BeginList(&MinimumSlots, IID_PPV_ARGS(Removed.Put()));
	if (FAILED(Result))
		return HresultFailure("Jump-list transaction start", Result);
	for (const JumpListCategory &Category : JumpList.Categories)
	{
		if (Category.Name.empty())
		{
			Destinations->AbortList();
			return {.Diagnostic = "Jump-list categories require names"};
		}
		COMPointer<IObjectCollection> Collection;
		Result = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(Collection.Put()));
		if (FAILED(Result))
			break;
		for (const JumpListItem &Item : Category.Items)
		{
			if (Item.Executable.empty() || Item.Title.empty())
			{
				Result = E_INVALIDARG;
				break;
			}
			COMPointer<IShellLinkW> Link;
			Result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(Link.Put()));
			if (FAILED(Result))
				break;
			Result = Link->SetPath(Item.Executable.wstring().c_str());
			if (SUCCEEDED(Result) && !Item.Arguments.empty())
				Result = Link->SetArguments(ToWideShell(Item.Arguments).c_str());
			if (SUCCEEDED(Result) && !Item.Icon.empty())
				Result = Link->SetIconLocation(Item.Icon.wstring().c_str(), Item.IconIndex);
			COMPointer<IPropertyStore> LinkProperties;
			if (SUCCEEDED(Result))
				Result = Link->QueryInterface(IID_PPV_ARGS(LinkProperties.Put()));
			if (SUCCEEDED(Result))
				Result = SetStringProperty(*LinkProperties.Get(), PKEY_Title, ToWideShell(Item.Title));
			if (SUCCEEDED(Result))
				Result = LinkProperties->Commit();
			if (SUCCEEDED(Result))
				Result = Collection->AddObject(Link.Get());
			if (FAILED(Result))
				break;
		}
		if (FAILED(Result))
			break;
		COMPointer<IObjectArray> CategoryObjects;
		Result = Collection->QueryInterface(IID_PPV_ARGS(CategoryObjects.Put()));
		if (SUCCEEDED(Result))
			Result = Destinations->AppendCategory(ToWideShell(Category.Name).c_str(), CategoryObjects.Get());
		if (FAILED(Result))
			break;
	}
	if (SUCCEEDED(Result) && JumpList.IncludeRecent)
		Result = Destinations->AppendKnownCategory(KDC_RECENT);
	if (SUCCEEDED(Result) && JumpList.IncludeFrequent)
		Result = Destinations->AppendKnownCategory(KDC_FREQUENT);
	if (SUCCEEDED(Result))
		Result = Destinations->CommitList();
	else
		Destinations->AbortList();
	return FAILED(Result) ? HresultFailure("Jump-list update", Result) : WindowOperationResult{.Succeeded = true};
}

WindowOperationResult Window::AddRecentDocument(const std::filesystem::path &Path)
{
	this->RequireOwnerThread();
	if (Path.empty())
		return {.Diagnostic = "Recent document path cannot be empty"};
	std::error_code Error;
	const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, Error);
	if (Error || !std::filesystem::is_regular_file(Canonical, Error))
		return {.NativeCode = static_cast<uint32>(Error.value()), .Diagnostic = "Recent document must identify an accessible file"};
	SHAddToRecentDocs(SHARD_PATHW, Canonical.c_str());
	return {.Succeeded = true};
}

WindowOperationResult Window::RegisterFileAssociation(const FileAssociation &Association)
{
	this->RequireOwnerThread();
	if (Association.Extension.size() < 2 || Association.Extension.front() != '.' || Association.ProgrammaticID.empty() ||
		Association.Verbs.empty())
		return {.Diagnostic = "File association requires an extension, programmatic ID, and at least one verb"};
	const std::wstring Extension = ToWideShell(Association.Extension);
	const std::wstring ProgrammaticID = ToWideShell(Association.ProgrammaticID);
	const std::wstring Classes = L"Software\\Classes\\";
	LSTATUS Result = SetRegistryString(HKEY_CURRENT_USER, Classes + Extension, nullptr, ProgrammaticID);
	if (Result == ERROR_SUCCESS)
		Result = SetRegistryString(HKEY_CURRENT_USER, Classes + ProgrammaticID, nullptr, ToWideShell(Association.Description));
	if (Result == ERROR_SUCCESS && !Association.Icon.empty())
		Result = SetRegistryString(HKEY_CURRENT_USER, Classes + ProgrammaticID + L"\\DefaultIcon", nullptr, Association.Icon.wstring());
	for (const FileAssociationVerb &Verb : Association.Verbs)
	{
		if (Result != ERROR_SUCCESS)
			break;
		if (Verb.Name.empty() || Verb.Command.empty())
			return {.Diagnostic = "File-association verbs require names and commands"};
		const std::wstring VerbPath = Classes + ProgrammaticID + L"\\shell\\" + ToWideShell(Verb.Name);
		if (!Verb.DisplayName.empty())
			Result = SetRegistryString(HKEY_CURRENT_USER, VerbPath, nullptr, ToWideShell(Verb.DisplayName));
		if (Result == ERROR_SUCCESS)
			Result = SetRegistryString(HKEY_CURRENT_USER, VerbPath + L"\\command", nullptr, ToWideShell(Verb.Command));
	}
	if (Result != ERROR_SUCCESS)
		return NativeFailure("File-association registration", Result);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
	return {.Succeeded = true};
}

WindowOperationResult Window::UnregisterFileAssociation(const FileAssociation &Association)
{
	this->RequireOwnerThread();
	if (Association.Extension.size() < 2 || Association.Extension.front() != '.' || Association.ProgrammaticID.empty())
		return {.Diagnostic = "File-association removal requires an extension and programmatic ID"};
	const std::wstring Classes = L"Software\\Classes\\";
	const std::wstring Extension = ToWideShell(Association.Extension);
	const std::wstring ProgrammaticID = ToWideShell(Association.ProgrammaticID);
	HKEY ExtensionKey = nullptr;
	LSTATUS Result = RegOpenKeyExW(HKEY_CURRENT_USER, (Classes + Extension).c_str(), 0, KEY_QUERY_VALUE, &ExtensionKey);
	if (Result == ERROR_SUCCESS)
	{
		DWORD Type = 0, Bytes = 0;
		Result = RegQueryValueExW(ExtensionKey, nullptr, nullptr, &Type, nullptr, &Bytes);
		std::wstring Current(Bytes / sizeof(wchar_t), L'\0');
		if (Result == ERROR_SUCCESS)
			Result = RegQueryValueExW(ExtensionKey, nullptr, nullptr, &Type, reinterpret_cast<uint8 *>(Current.data()), &Bytes);
		RegCloseKey(ExtensionKey);
		if (Result == ERROR_SUCCESS)
		{
			while (!Current.empty() && Current.back() == L'\0')
				Current.pop_back();
			if (Current != ProgrammaticID)
				return {.Diagnostic = "Extension is owned by another file association"};
		}
	}
	if (Result != ERROR_SUCCESS && Result != ERROR_FILE_NOT_FOUND)
		return NativeFailure("File-association ownership check", Result);
	Result = RegDeleteTreeW(HKEY_CURRENT_USER, (Classes + Extension).c_str());
	if (Result != ERROR_SUCCESS && Result != ERROR_FILE_NOT_FOUND)
		return NativeFailure("File-association extension removal", Result);
	Result = RegDeleteTreeW(HKEY_CURRENT_USER, (Classes + ProgrammaticID).c_str());
	if (Result != ERROR_SUCCESS && Result != ERROR_FILE_NOT_FOUND)
		return NativeFailure("File-association program removal", Result);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
	return {.Succeeded = true};
}
} // namespace core
