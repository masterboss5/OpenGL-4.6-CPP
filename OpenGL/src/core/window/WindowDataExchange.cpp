#include "Window.h"
#include "WindowException.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace core
{
namespace
{
constexpr usize BytesPerPixel = 4;

[[nodiscard]] std::wstring ToWideData(const std::string_view Source)
{
	if (Source.empty())
		return {};
	if (Source.size() > static_cast<usize>(std::numeric_limits<int32>::max()))
		throw WindowException("Text exceeds native conversion limits");
	const int32 Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0);
	if (Count <= 0)
		throw WindowException("Text is not valid UTF-8");
	std::wstring Result(static_cast<usize>(Count), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Count) != Count)
		throw WindowException("UTF-8 conversion failed");
	return Result;
}

[[nodiscard]] std::string ToUtf8Data(const std::wstring_view Source)
{
	if (Source.empty())
		return {};
	if (Source.size() > static_cast<usize>(std::numeric_limits<int32>::max()))
		throw WindowException("Text exceeds native conversion limits");
	const int32 Count =
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), nullptr, 0, nullptr, nullptr);
	if (Count <= 0)
		throw WindowException("Unicode text conversion failed");
	std::string Result(static_cast<usize>(Count), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Source.data(), static_cast<int32>(Source.size()), Result.data(), Count, nullptr,
							nullptr) != Count)
		throw WindowException("Unicode text conversion failed");
	return Result;
}

[[nodiscard]] usize CheckedAdd(const usize Left, const usize Right, const std::string_view Operation)
{
	if (Right > std::numeric_limits<usize>::max() - Left)
		throw WindowException(std::string(Operation) + " size overflow");
	return Left + Right;
}

[[nodiscard]] usize CheckedMultiply(const usize Left, const usize Right, const std::string_view Operation)
{
	if (Left != 0 && Right > std::numeric_limits<usize>::max() / Left)
		throw WindowException(std::string(Operation) + " size overflow");
	return Left * Right;
}

class ClipboardSession final
{
  public:
	explicit ClipboardSession(HWND Owner)
	{
		if (OpenClipboard(Owner) == FALSE)
			throw WindowException("Cannot open the clipboard; another process may own it");
	}
	~ClipboardSession()
	{
		CloseClipboard();
	}
	ClipboardSession(const ClipboardSession &) = delete;
	ClipboardSession &operator=(const ClipboardSession &) = delete;
};

class GlobalBlock final
{
  public:
	explicit GlobalBlock(const usize Size) : Handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, Size))
	{
		if (this->Handle == nullptr)
			throw WindowException("Clipboard memory allocation failed");
	}
	~GlobalBlock()
	{
		if (this->Handle != nullptr)
			GlobalFree(this->Handle);
	}
	GlobalBlock(const GlobalBlock &) = delete;
	GlobalBlock &operator=(const GlobalBlock &) = delete;
	[[nodiscard]] HGLOBAL Get() const noexcept
	{
		return this->Handle;
	}
	[[nodiscard]] void *Lock()
	{
		void *Memory = GlobalLock(this->Handle);
		if (Memory == nullptr)
			throw WindowException("Clipboard memory lock failed");
		return Memory;
	}
	void Unlock() noexcept
	{
		GlobalUnlock(this->Handle);
	}
	[[nodiscard]] HGLOBAL Release() noexcept
	{
		return std::exchange(this->Handle, nullptr);
	}

  private:
	HGLOBAL Handle = nullptr;
};

void CommitClipboardBlock(const UINT Format, GlobalBlock &Block)
{
	if (SetClipboardData(Format, Block.Get()) == nullptr)
		throw WindowException("Setting clipboard format failed with native error " + std::to_string(static_cast<uint32>(GetLastError())));
	(void)Block.Release();
}

[[nodiscard]] UINT RegisterNamedFormat(const std::string &Name)
{
	if (Name.empty())
		throw WindowException("A named data format cannot be empty");
	const std::wstring WideName = ToWideData(Name);
	const UINT Format = RegisterClipboardFormatW(WideName.c_str());
	if (Format == 0)
		throw WindowException("Named data-format registration failed for '" + Name + "'");
	return Format;
}

void SetText(const std::string &Text)
{
	const std::wstring Wide = ToWideData(Text);
	const usize Bytes = CheckedMultiply(CheckedAdd(Wide.size(), 1, "Clipboard text"), sizeof(wchar_t), "Clipboard text");
	GlobalBlock Block(Bytes);
	void *Memory = Block.Lock();
	std::memcpy(Memory, Wide.c_str(), Bytes);
	Block.Unlock();
	CommitClipboardBlock(CF_UNICODETEXT, Block);
}

void SetFiles(const std::vector<std::filesystem::path> &Files)
{
	if (Files.empty())
		return;
	usize CharacterCount = 1;
	std::vector<std::wstring> NativePaths;
	NativePaths.reserve(Files.size());
	for (const std::filesystem::path &Path : Files)
	{
		if (Path.empty())
			throw WindowException("Clipboard file lists cannot contain empty paths");
		NativePaths.push_back(Path.wstring());
		CharacterCount = CheckedAdd(CharacterCount, CheckedAdd(NativePaths.back().size(), 1, "Clipboard file list"), "Clipboard file list");
	}
	const usize TextBytes = CheckedMultiply(CharacterCount, sizeof(wchar_t), "Clipboard file list");
	GlobalBlock Block(CheckedAdd(sizeof(DROPFILES), TextBytes, "Clipboard file list"));
	uint8 *Memory = static_cast<uint8 *>(Block.Lock());
	auto *Header = reinterpret_cast<DROPFILES *>(Memory);
	Header->pFiles = sizeof(DROPFILES);
	Header->fWide = TRUE;
	wchar_t *Destination = reinterpret_cast<wchar_t *>(Memory + sizeof(DROPFILES));
	for (const std::wstring &Path : NativePaths)
	{
		std::memcpy(Destination, Path.c_str(), CheckedMultiply(Path.size(), sizeof(wchar_t), "Clipboard file list"));
		Destination += Path.size() + 1;
	}
	*Destination = L'\0';
	Block.Unlock();
	CommitClipboardBlock(CF_HDROP, Block);
}

void SetImage(const TransferredImage &Image)
{
	if (!Image.IsValid())
		throw WindowException("Clipboard images must contain tightly packed RGBA8 pixels");
	if (Image.Extent.Width > static_cast<uint32>(std::numeric_limits<int32>::max()) ||
		Image.Extent.Height > static_cast<uint32>(std::numeric_limits<int32>::max()))
		throw WindowException("Clipboard image dimensions exceed native limits");
	const usize PixelBytes =
		CheckedMultiply(CheckedMultiply(Image.Extent.Width, Image.Extent.Height, "Clipboard image"), BytesPerPixel, "Clipboard image");
	GlobalBlock Block(CheckedAdd(sizeof(BITMAPV5HEADER), PixelBytes, "Clipboard image"));
	uint8 *Memory = static_cast<uint8 *>(Block.Lock());
	auto *Header = reinterpret_cast<BITMAPV5HEADER *>(Memory);
	Header->bV5Size = sizeof(BITMAPV5HEADER);
	Header->bV5Width = static_cast<LONG>(Image.Extent.Width);
	Header->bV5Height = -static_cast<LONG>(Image.Extent.Height);
	Header->bV5Planes = 1;
	Header->bV5BitCount = 32;
	Header->bV5Compression = BI_BITFIELDS;
	Header->bV5SizeImage = static_cast<DWORD>(PixelBytes);
	Header->bV5RedMask = 0x00FF0000;
	Header->bV5GreenMask = 0x0000FF00;
	Header->bV5BlueMask = 0x000000FF;
	Header->bV5AlphaMask = 0xFF000000;
	Header->bV5CSType = LCS_sRGB;
	uint8 *Destination = Memory + sizeof(BITMAPV5HEADER);
	for (usize Pixel = 0; Pixel < Image.Rgba8.size() / BytesPerPixel; ++Pixel)
	{
		Destination[Pixel * 4] = Image.Rgba8[Pixel * 4 + 2];
		Destination[Pixel * 4 + 1] = Image.Rgba8[Pixel * 4 + 1];
		Destination[Pixel * 4 + 2] = Image.Rgba8[Pixel * 4];
		Destination[Pixel * 4 + 3] = Image.Rgba8[Pixel * 4 + 3];
	}
	Block.Unlock();
	CommitClipboardBlock(CF_DIBV5, Block);
}

void SetCustom(const CustomDataPayload &Custom)
{
	const UINT Format = RegisterNamedFormat(Custom.Format);
	GlobalBlock Block(std::max<usize>(Custom.Bytes.size(), 1));
	void *Memory = Block.Lock();
	if (!Custom.Bytes.empty())
		std::memcpy(Memory, Custom.Bytes.data(), Custom.Bytes.size());
	Block.Unlock();
	CommitClipboardBlock(Format, Block);
}

[[nodiscard]] std::optional<std::string> ReadTextHandle(HGLOBAL Handle, const usize MaximumBytes)
{
	if (Handle == nullptr)
		return std::nullopt;
	const usize Size = GlobalSize(Handle);
	if (Size == 0 || Size > MaximumBytes || Size % sizeof(wchar_t) != 0)
		throw WindowException("Clipboard Unicode text has an invalid size");
	const wchar_t *Memory = static_cast<const wchar_t *>(GlobalLock(Handle));
	if (Memory == nullptr)
		throw WindowException("Clipboard Unicode text cannot be locked");
	const usize Capacity = Size / sizeof(wchar_t);
	const wchar_t *Terminator = std::find(Memory, Memory + Capacity, L'\0');
	if (Terminator == Memory + Capacity)
	{
		GlobalUnlock(Handle);
		throw WindowException("Clipboard Unicode text is not terminated");
	}
	std::wstring Value(Memory, Terminator);
	GlobalUnlock(Handle);
	return ToUtf8Data(Value);
}

[[nodiscard]] std::vector<std::filesystem::path> ReadFilesHandle(HGLOBAL Handle, const usize MaximumBytes)
{
	std::vector<std::filesystem::path> Result;
	if (Handle == nullptr)
		return Result;
	const usize Size = GlobalSize(Handle);
	if (Size == 0 || Size > MaximumBytes)
		throw WindowException("Dropped file data exceeds the configured payload limit");
	HDROP Drop = static_cast<HDROP>(Handle);
	const UINT Count = DragQueryFileW(Drop, 0xFFFFFFFFU, nullptr, 0);
	Result.reserve(Count);
	for (UINT Index = 0; Index < Count; ++Index)
	{
		const UINT Length = DragQueryFileW(Drop, Index, nullptr, 0);
		std::wstring Path(static_cast<usize>(Length) + 1, L'\0');
		if (DragQueryFileW(Drop, Index, Path.data(), Length + 1) != Length)
			throw WindowException("Dropped file path extraction failed");
		Path.resize(Length);
		Result.emplace_back(std::move(Path));
	}
	return Result;
}

[[nodiscard]] std::optional<TransferredImage> ReadImageHandle(HGLOBAL Handle, const usize MaximumBytes)
{
	if (Handle == nullptr)
		return std::nullopt;
	const usize Size = GlobalSize(Handle);
	if (Size < sizeof(BITMAPV5HEADER) || Size > MaximumBytes)
		throw WindowException("Clipboard image has an invalid size");
	const uint8 *Memory = static_cast<const uint8 *>(GlobalLock(Handle));
	if (Memory == nullptr)
		throw WindowException("Clipboard image cannot be locked");
	const auto *Header = reinterpret_cast<const BITMAPV5HEADER *>(Memory);
	if (Header->bV5Size < sizeof(BITMAPV5HEADER) || Header->bV5Planes != 1 || Header->bV5BitCount != 32 || Header->bV5Width <= 0 ||
		Header->bV5Height == 0 || (Header->bV5Compression != BI_RGB && Header->bV5Compression != BI_BITFIELDS))
	{
		GlobalUnlock(Handle);
		throw WindowException("Clipboard image is not a supported 32-bit DIBV5 image");
	}
	const uint32 Width = static_cast<uint32>(Header->bV5Width);
	const int64 SignedHeight = Header->bV5Height;
	const uint64 AbsoluteHeight = SignedHeight < 0 ? static_cast<uint64>(-SignedHeight) : static_cast<uint64>(SignedHeight);
	if (AbsoluteHeight > std::numeric_limits<uint32>::max())
	{
		GlobalUnlock(Handle);
		throw WindowException("Clipboard image height exceeds engine limits");
	}
	const uint32 Height = static_cast<uint32>(AbsoluteHeight);
	const usize PixelBytes = CheckedMultiply(CheckedMultiply(Width, Height, "Clipboard image"), BytesPerPixel, "Clipboard image");
	if (Header->bV5Size > Size || PixelBytes > Size - Header->bV5Size)
	{
		GlobalUnlock(Handle);
		throw WindowException("Clipboard image data is truncated");
	}
	TransferredImage Result{.Extent = {Width, Height}, .Rgba8 = std::vector<uint8>(PixelBytes)};
	const uint8 *Source = Memory + Header->bV5Size;
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		const uint32 SourceY = SignedHeight < 0 ? Y : Height - Y - 1;
		for (uint32 X = 0; X < Width; ++X)
		{
			const usize SourceIndex = (static_cast<usize>(SourceY) * Width + X) * 4;
			const usize DestinationIndex = (static_cast<usize>(Y) * Width + X) * 4;
			Result.Rgba8[DestinationIndex] = Source[SourceIndex + 2];
			Result.Rgba8[DestinationIndex + 1] = Source[SourceIndex + 1];
			Result.Rgba8[DestinationIndex + 2] = Source[SourceIndex];
			Result.Rgba8[DestinationIndex + 3] = Source[SourceIndex + 3];
		}
	}
	GlobalUnlock(Handle);
	return Result;
}

[[nodiscard]] std::optional<CustomDataPayload> ReadCustomHandle(const UINT Format, HGLOBAL Handle, const usize MaximumBytes)
{
	if (Handle == nullptr || Format < 0xC000U)
		return std::nullopt;
	std::wstring Name(256, L'\0');
	const int32 Length = GetClipboardFormatNameW(Format, Name.data(), static_cast<int32>(Name.size()));
	if (Length <= 0)
		return std::nullopt;
	Name.resize(static_cast<usize>(Length));
	const usize Size = GlobalSize(Handle);
	if (Size > MaximumBytes)
		throw WindowException("Named clipboard payload exceeds the configured payload limit");
	const void *Memory = Size == 0 ? nullptr : GlobalLock(Handle);
	if (Size != 0 && Memory == nullptr)
		throw WindowException("Named clipboard payload cannot be locked");
	CustomDataPayload Result{.Format = ToUtf8Data(Name), .Bytes = std::vector<uint8>(Size)};
	if (Size != 0)
	{
		std::memcpy(Result.Bytes.data(), Memory, Size);
		GlobalUnlock(Handle);
	}
	return Result;
}

[[nodiscard]] FORMATETC GlobalFormat(const CLIPFORMAT Format) noexcept
{
	return {.cfFormat = Format, .ptd = nullptr, .dwAspect = DVASPECT_CONTENT, .lindex = -1, .tymed = TYMED_HGLOBAL};
}

[[nodiscard]] bool Supports(IDataObject &Object, const CLIPFORMAT Format)
{
	FORMATETC Request = GlobalFormat(Format);
	return Object.QueryGetData(&Request) == S_OK;
}

[[nodiscard]] bool SupportsTransferData(IDataObject &Object)
{
	if (Supports(Object, CF_UNICODETEXT) || Supports(Object, CF_HDROP) || Supports(Object, CF_DIBV5))
		return true;
	IEnumFORMATETC *Enumerator = nullptr;
	if (FAILED(Object.EnumFormatEtc(DATADIR_GET, &Enumerator)) || Enumerator == nullptr)
		return false;
	bool Supported = false;
	FORMATETC Format{};
	while (Enumerator->Next(1, &Format, nullptr) == S_OK)
	{
		if (Format.cfFormat >= 0xC000U && (Format.tymed & TYMED_HGLOBAL) != 0)
			Supported = true;
		if (Format.ptd != nullptr)
			CoTaskMemFree(Format.ptd);
		if (Supported)
			break;
	}
	Enumerator->Release();
	return Supported;
}

template <typename ReaderFunction, typename ResultType>
void ReadObjectFormat(IDataObject &Object, const CLIPFORMAT Format, ReaderFunction &&Reader, ResultType &Destination)
{
	FORMATETC Request = GlobalFormat(Format);
	STGMEDIUM Medium{};
	const HRESULT OperationResult = Object.GetData(&Request, &Medium);
	if (FAILED(OperationResult))
		throw WindowException("Dropped data extraction failed with HRESULT " + std::to_string(static_cast<uint32>(OperationResult)));
	try
	{
		Destination = Reader(Medium.hGlobal);
	}
	catch (...)
	{
		ReleaseStgMedium(&Medium);
		throw;
	}
	ReleaseStgMedium(&Medium);
}

[[nodiscard]] DataPayload ReadDataObject(IDataObject &Object, const DataReadRequest &Request)
{
	DataPayload Payload;
	if (Request.Text && Supports(Object, CF_UNICODETEXT))
		ReadObjectFormat(
			Object, CF_UNICODETEXT, [&Request](HGLOBAL Handle) { return ReadTextHandle(Handle, Request.MaximumPayloadBytes); },
			Payload.Text);
	if (Request.Files && Supports(Object, CF_HDROP))
		ReadObjectFormat(
			Object, CF_HDROP, [&Request](HGLOBAL Handle) { return ReadFilesHandle(Handle, Request.MaximumPayloadBytes); }, Payload.Files);
	if (Request.Image && Supports(Object, CF_DIBV5))
		ReadObjectFormat(
			Object, CF_DIBV5, [&Request](HGLOBAL Handle) { return ReadImageHandle(Handle, Request.MaximumPayloadBytes); }, Payload.Image);
	std::vector<UINT> NamedFormats;
	for (const std::string &Name : Request.NamedFormats)
		NamedFormats.push_back(RegisterNamedFormat(Name));
	if (Request.AllNamedFormats)
	{
		IEnumFORMATETC *Enumerator = nullptr;
		if (SUCCEEDED(Object.EnumFormatEtc(DATADIR_GET, &Enumerator)) && Enumerator != nullptr)
		{
			FORMATETC Format{};
			while (Enumerator->Next(1, &Format, nullptr) == S_OK)
			{
				if (Format.cfFormat >= 0xC000U && (Format.tymed & TYMED_HGLOBAL) != 0)
					NamedFormats.push_back(Format.cfFormat);
				if (Format.ptd != nullptr)
					CoTaskMemFree(Format.ptd);
			}
			Enumerator->Release();
		}
	}
	std::sort(NamedFormats.begin(), NamedFormats.end());
	NamedFormats.erase(std::unique(NamedFormats.begin(), NamedFormats.end()), NamedFormats.end());
	for (const UINT NativeFormat : NamedFormats)
	{
		if (!Supports(Object, static_cast<CLIPFORMAT>(NativeFormat)))
			continue;
		std::optional<CustomDataPayload> Custom;
		ReadObjectFormat(
			Object, static_cast<CLIPFORMAT>(NativeFormat), [NativeFormat, &Request](HGLOBAL Handle)
			{ return ReadCustomHandle(NativeFormat, Handle, Request.MaximumPayloadBytes); }, Custom);
		if (Custom)
			Payload.Custom.push_back(std::move(*Custom));
	}
	return Payload;
}

[[nodiscard]] DWORD ChooseEffect(const DWORD Allowed, const DWORD Keys) noexcept
{
	if ((Keys & MK_CONTROL) != 0 && (Allowed & DROPEFFECT_COPY) != 0)
		return DROPEFFECT_COPY;
	if ((Keys & MK_SHIFT) != 0 && (Allowed & DROPEFFECT_MOVE) != 0)
		return DROPEFFECT_MOVE;
	if ((Keys & MK_ALT) != 0 && (Allowed & DROPEFFECT_LINK) != 0)
		return DROPEFFECT_LINK;
	if ((Allowed & DROPEFFECT_COPY) != 0)
		return DROPEFFECT_COPY;
	if ((Allowed & DROPEFFECT_MOVE) != 0)
		return DROPEFFECT_MOVE;
	if ((Allowed & DROPEFFECT_LINK) != 0)
		return DROPEFFECT_LINK;
	return DROPEFFECT_NONE;
}

[[nodiscard]] DataTransferOperation ToOperation(const DWORD Effect) noexcept
{
	if ((Effect & DROPEFFECT_COPY) != 0)
		return DataTransferOperation::Copy;
	if ((Effect & DROPEFFECT_MOVE) != 0)
		return DataTransferOperation::Move;
	if ((Effect & DROPEFFECT_LINK) != 0)
		return DataTransferOperation::Link;
	return DataTransferOperation::None;
}
} // namespace

class DropTarget final : public IDropTarget
{
  public:
	explicit DropTarget(Window &Window) noexcept : Window(&Window)
	{
	}
	void Retarget(Window &Replacement) noexcept
	{
		this->Window = &Replacement;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID InterfaceID, void **Object) override
	{
		if (Object == nullptr)
			return E_POINTER;
		*Object = nullptr;
		if (InterfaceID == IID_IUnknown || InterfaceID == IID_IDropTarget)
		{
			*Object = static_cast<IDropTarget *>(this);
			this->AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return static_cast<ULONG>(++this->References);
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		const uint32 Remaining = --this->References;
		if (Remaining == 0)
			delete this;
		return static_cast<ULONG>(Remaining);
	}
	HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *Object, DWORD Keys, POINTL, DWORD *Effect) override
	{
		if (Object == nullptr || Effect == nullptr)
			return E_INVALIDARG;
		const bool Supported = SupportsTransferData(*Object);
		*Effect = Supported ? ChooseEffect(*Effect, Keys) : DROPEFFECT_NONE;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DragOver(DWORD Keys, POINTL, DWORD *Effect) override
	{
		if (Effect == nullptr)
			return E_INVALIDARG;
		*Effect = ChooseEffect(*Effect, Keys);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DragLeave() override
	{
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Drop(IDataObject *Object, DWORD Keys, POINTL Point, DWORD *Effect) override
	{
		if (Object == nullptr || Effect == nullptr || this->Window == nullptr)
			return E_INVALIDARG;
		const DWORD Selected = ChooseEffect(*Effect, Keys);
		*Effect = Selected;
		try
		{
			DataReadRequest Request;
			Request.AllNamedFormats = true;
			DataPayload Payload = ReadDataObject(*Object, Request);
			if (Payload.Empty())
			{
				*Effect = DROPEFFECT_NONE;
				return DV_E_FORMATETC;
			}
			POINT ClientPoint{Point.x, Point.y};
			HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->Window->GetNativeWindow()));
			if (ScreenToClient(NativeWindow, &ClientPoint) == FALSE)
				throw WindowException("Dropped data position conversion failed");
			this->Window->PublishDrop(std::move(Payload), {ClientPoint.x, ClientPoint.y}, ToOperation(Selected));
			return S_OK;
		}
		catch (...)
		{
			this->Window->RecordNativeFailure(std::current_exception());
			*Effect = DROPEFFECT_NONE;
			return E_FAIL;
		}
	}

  private:
	std::atomic<uint32> References{1};
	Window *Window = nullptr;
};

void Window::InitializeDataTransfer()
{
	if (this->GetDataDropTarget() != nullptr)
		throw WindowException("Window data-transfer target is already initialized");
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	if (NativeWindow == nullptr)
		throw WindowException("Cannot initialize data transfer without a native window");
	DropTarget *Target = new DropTarget(*this);
	const HRESULT RegistrationResult = RegisterDragDrop(NativeWindow, Target);
	if (FAILED(RegistrationResult))
	{
		Target->Release();
		throw WindowException("OLE drag/drop registration failed with HRESULT " + std::to_string(static_cast<uint32>(RegistrationResult)));
	}
	this->SetDataDropTarget(Target);
}

void Window::ShutdownDataTransfer() noexcept
{
	DropTarget *Target = static_cast<DropTarget *>(this->GetDataDropTarget());
	if (Target == nullptr)
		return;
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	if (NativeWindow != nullptr)
		(void)RevokeDragDrop(NativeWindow);
	Target->Release();
	this->SetDataDropTarget(nullptr);
}

void Window::RetargetDataTransfer() noexcept
{
	DropTarget *Target = static_cast<DropTarget *>(this->GetDataDropTarget());
	if (Target != nullptr)
		Target->Retarget(*this);
}

void Window::SetClipboardData(const DataPayload &Payload)
{
	this->RequireOwnerThread();
	if (Payload.Empty())
		throw WindowException("Cannot set an empty clipboard payload; use ClearClipboard explicitly");
	for (const CustomDataPayload &Custom : Payload.Custom)
	{
		if (Custom.Format.empty())
			throw WindowException("A named clipboard format cannot be empty");
		if (Custom.Bytes.empty())
			throw WindowException("A named clipboard payload cannot be empty");
	}
	for (usize First = 0; First < Payload.Custom.size(); ++First)
	{
		for (usize Second = First + 1; Second < Payload.Custom.size(); ++Second)
		{
			if (Payload.Custom[First].Format == Payload.Custom[Second].Format)
				throw WindowException("A clipboard payload cannot contain duplicate named formats");
		}
	}
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	ClipboardSession Session(NativeWindow);
	if (EmptyClipboard() == FALSE)
		throw WindowException("Clearing existing clipboard formats failed");
	if (Payload.Text)
		SetText(*Payload.Text);
	SetFiles(Payload.Files);
	if (Payload.Image)
		SetImage(*Payload.Image);
	for (const CustomDataPayload &Custom : Payload.Custom)
		SetCustom(Custom);
}

DataPayload Window::ReadClipboardData(const DataReadRequest &Request) const
{
	this->RequireOwnerThread();
	if (Request.MaximumPayloadBytes == 0)
		throw WindowException("Clipboard payload limit must be non-zero");
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	ClipboardSession Session(NativeWindow);
	DataPayload Payload;
	if (Request.Text && IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE)
		Payload.Text = ReadTextHandle(GetClipboardData(CF_UNICODETEXT), Request.MaximumPayloadBytes);
	if (Request.Files && IsClipboardFormatAvailable(CF_HDROP) != FALSE)
		Payload.Files = ReadFilesHandle(GetClipboardData(CF_HDROP), Request.MaximumPayloadBytes);
	if (Request.Image && IsClipboardFormatAvailable(CF_DIBV5) != FALSE)
		Payload.Image = ReadImageHandle(GetClipboardData(CF_DIBV5), Request.MaximumPayloadBytes);
	std::vector<UINT> Formats;
	for (const std::string &Name : Request.NamedFormats)
		Formats.push_back(RegisterNamedFormat(Name));
	if (Request.AllNamedFormats)
	{
		UINT Format = 0;
		SetLastError(ERROR_SUCCESS);
		while ((Format = EnumClipboardFormats(Format)) != 0)
			if (Format >= 0xC000U)
				Formats.push_back(Format);
		if (GetLastError() != ERROR_SUCCESS)
			throw WindowException("Clipboard format enumeration failed");
	}
	std::sort(Formats.begin(), Formats.end());
	Formats.erase(std::unique(Formats.begin(), Formats.end()), Formats.end());
	for (const UINT Format : Formats)
	{
		if (IsClipboardFormatAvailable(Format) == FALSE)
			continue;
		std::optional<CustomDataPayload> Custom = ReadCustomHandle(Format, GetClipboardData(Format), Request.MaximumPayloadBytes);
		if (Custom)
			Payload.Custom.push_back(std::move(*Custom));
	}
	return Payload;
}

std::vector<std::string> Window::GetClipboardFormats() const
{
	this->RequireOwnerThread();
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	ClipboardSession Session(NativeWindow);
	std::vector<std::string> Formats;
	UINT Format = 0;
	SetLastError(ERROR_SUCCESS);
	while ((Format = EnumClipboardFormats(Format)) != 0)
	{
		if (Format == CF_UNICODETEXT)
			Formats.emplace_back("text/unicode");
		else if (Format == CF_HDROP)
			Formats.emplace_back("files");
		else if (Format == CF_DIBV5)
			Formats.emplace_back("image/rgba8");
		else if (Format >= 0xC000U)
		{
			std::wstring Name(256, L'\0');
			const int32 Length = GetClipboardFormatNameW(Format, Name.data(), static_cast<int32>(Name.size()));
			if (Length > 0)
			{
				Name.resize(static_cast<usize>(Length));
				Formats.push_back(ToUtf8Data(Name));
			}
		}
	}
	if (GetLastError() != ERROR_SUCCESS)
		throw WindowException("Clipboard format enumeration failed");
	return Formats;
}

void Window::ClearClipboard()
{
	this->RequireOwnerThread();
	HWND NativeWindow = glfwGetWin32Window(static_cast<GLFWwindow *>(this->GetNativeWindow()));
	ClipboardSession Session(NativeWindow);
	if (EmptyClipboard() == FALSE)
		throw WindowException("Clearing the clipboard failed");
}

void Window::PublishDrop(DataPayload Payload, const WindowPosition Position, const DataTransferOperation Operation)
{
	this->Publish({.Type = WindowEventType::DataDropped,
				   .Window = this->GetID(),
				   .DataPosition = Position,
				   .DataOperation = Operation,
				   .Data = std::make_shared<const DataPayload>(std::move(Payload))});
}
} // namespace core
