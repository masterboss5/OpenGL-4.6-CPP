#include "SecurePath.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

namespace core::io
{
namespace
{
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif

using NtCreateFileFunction = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
											   ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);

struct NativeFileRenameInformation final
{
	BOOLEAN ReplaceIfExists = FALSE;
	HANDLE RootDirectory = nullptr;
	ULONG FileNameLength = 0;
	WCHAR FileName[1]{};
};

class NativeHandle final
{
  public:
	explicit NativeHandle(const HANDLE Value) noexcept : Value(Value)
	{
	}
	~NativeHandle()
	{
		if (this->Value != INVALID_HANDLE_VALUE)
			CloseHandle(this->Value);
	}
	NativeHandle(const NativeHandle &) = delete;
	NativeHandle &operator=(const NativeHandle &) = delete;
	NativeHandle(NativeHandle &&Other) noexcept : Value(Other.Release())
	{
	}
	NativeHandle &operator=(NativeHandle &&Other) noexcept
	{
		if (this != &Other)
		{
			if (this->Value != INVALID_HANDLE_VALUE)
				CloseHandle(this->Value);
			this->Value = Other.Release();
		}
		return *this;
	}
	[[nodiscard]] HANDLE Get() const noexcept
	{
		return this->Value;
	}
	[[nodiscard]] HANDLE Release() noexcept
	{
		const HANDLE Result = this->Value;
		this->Value = INVALID_HANDLE_VALUE;
		return Result;
	}

  private:
	HANDLE Value = INVALID_HANDLE_VALUE;
};

[[nodiscard]] NtCreateFileFunction GetNtCreateFile()
{
	static const auto Function = reinterpret_cast<NtCreateFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
	if (Function == nullptr)
		throw SecurePathException("Native handle-relative file creation is unavailable");
	return Function;
}

[[nodiscard]] NtSetInformationFileFunction GetNtSetInformationFile()
{
	static const auto Function =
		reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
	if (Function == nullptr)
		throw SecurePathException("Native handle-relative file renaming is unavailable");
	return Function;
}

[[nodiscard]] NativeHandle OpenRoot(const std::filesystem::path &Root, const string_view Role, const bool Mutable = false)
{
	ACCESS_MASK Access = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE;
	if (Mutable)
		Access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
	NativeHandle Handle(CreateFileW(std::filesystem::absolute(Root).lexically_normal().c_str(), Access,
									FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (Handle.Get() == INVALID_HANDLE_VALUE)
		throw SecurePathException(string(Role) + " could not open its trusted root handle");
	FILE_ATTRIBUTE_TAG_INFO Tag{};
	if (GetFileInformationByHandleEx(Handle.Get(), FileAttributeTagInfo, &Tag, sizeof(Tag)) == FALSE ||
		(Tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		throw SecurePathException(string(Role) + " trusted root is not a direct directory");
	}
	return Handle;
}

[[nodiscard]] NativeHandle DuplicateNativeHandle(const HANDLE Handle, const string_view Role)
{
	HANDLE Duplicate = INVALID_HANDLE_VALUE;
	if (DuplicateHandle(GetCurrentProcess(), Handle, GetCurrentProcess(), &Duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE)
		throw SecurePathException(string(Role) + " could not duplicate its trusted root handle");
	return NativeHandle(Duplicate);
}

[[nodiscard]] std::filesystem::path SafeRelative(const std::filesystem::path &Path, const string_view Role)
{
	if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory())
		throw SecurePathException(string(Role) + " must be a non-empty relative path");
	const std::filesystem::path Result = Path.lexically_normal();
	for (const std::filesystem::path &Part : Result)
	{
		if (Part == "." || Part == "..")
			throw SecurePathException(string(Role) + " contains an unsafe path component");
	}
	return Result;
}

[[nodiscard]] NativeHandle OpenRelative(const HANDLE Root, const std::filesystem::path &Relative, const ACCESS_MASK Access,
										const ULONG Disposition, const ULONG Options, const string_view Role,
										const ULONG Attributes = FILE_ATTRIBUTE_NORMAL)
{
	std::wstring Text = SafeRelative(Relative, Role).native();
	if (Text.size() > static_cast<usize>(std::numeric_limits<USHORT>::max() / sizeof(wchar_t)))
		throw SecurePathException(string(Role) + " exceeds the native relative-path limit");
	UNICODE_STRING Name{.Length = static_cast<USHORT>(Text.size() * sizeof(wchar_t)),
						.MaximumLength = static_cast<USHORT>(Text.size() * sizeof(wchar_t)),
						.Buffer = Text.data()};
	OBJECT_ATTRIBUTES Object{};
	InitializeObjectAttributes(&Object, &Name, OBJ_CASE_INSENSITIVE, Root, nullptr);
	IO_STATUS_BLOCK Status{};
	HANDLE Value = INVALID_HANDLE_VALUE;
	const NTSTATUS Result = GetNtCreateFile()(
		&Value, Access | SYNCHRONIZE, &Object, &Status, nullptr, Attributes, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		Disposition, Options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT, nullptr, 0);
	if (Result < 0 || Value == INVALID_HANDLE_VALUE)
		throw SecurePathException(string(Role) + " failed a handle-relative file operation");
	NativeHandle Handle(Value);
	FILE_ATTRIBUTE_TAG_INFO Tag{};
	if (GetFileInformationByHandleEx(Handle.Get(), FileAttributeTagInfo, &Tag, sizeof(Tag)) != FALSE &&
		(Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		throw SecurePathException(string(Role) + " encountered a reparse point");
	return Handle;
}

[[nodiscard]] NativeHandle OpenDirectoryChain(const HANDLE Root, const std::filesystem::path &Relative, const bool Create,
											  const bool Mutable, const string_view Role)
{
	NativeHandle Current = DuplicateNativeHandle(Root, Role);
	if (Relative.empty() || Relative == ".")
		return Current;
	for (const std::filesystem::path &Part : SafeRelative(Relative, Role))
	{
		ACCESS_MASK Access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
		if (Create || Mutable)
			Access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
		NativeHandle Next = OpenRelative(Current.Get(), Part, Access, Create ? FILE_OPEN_IF : FILE_OPEN, FILE_DIRECTORY_FILE, Role,
										 FILE_ATTRIBUTE_DIRECTORY);
		Current = std::move(Next);
	}
	return Current;
}

struct ParentAndLeaf final
{
	NativeHandle Parent;
	std::filesystem::path Leaf;
};

[[nodiscard]] ParentAndLeaf OpenParent(const HANDLE Root, const std::filesystem::path &Relative, const bool CreateParents,
									   const bool Mutable, const string_view Role)
{
	const std::filesystem::path Safe = SafeRelative(Relative, Role);
	const std::filesystem::path Parent = Safe.parent_path();
	return ParentAndLeaf{OpenDirectoryChain(Root, Parent.empty() ? std::filesystem::path(".") : Parent, CreateParents, Mutable, Role),
						 Safe.filename()};
}

[[nodiscard]] NativeHandle OpenObjectRelative(const HANDLE Root, const std::filesystem::path &Relative, const ACCESS_MASK Access,
											  const ULONG Disposition, const ULONG Options, const string_view Role)
{
	ParentAndLeaf Object = OpenParent(Root, Relative, false, false, Role);
	return OpenRelative(Object.Parent.Get(), Object.Leaf, Access, Disposition, Options, Role);
}

void RenameRelative(const HANDLE SourceRoot, const std::filesystem::path &SourceRelative, const HANDLE DestinationRoot,
					const std::filesystem::path &DestinationRelative, const bool ReplaceExisting, const string_view Role)
{
	NativeHandle Source = OpenObjectRelative(SourceRoot, SourceRelative, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Role);
	ParentAndLeaf Destination = OpenParent(DestinationRoot, DestinationRelative, true, true, Role);
	const std::wstring Name = Destination.Leaf.native();
	const usize Bytes = offsetof(NativeFileRenameInformation, FileName) + (Name.size() * sizeof(wchar_t));
	if (Bytes > static_cast<usize>(std::numeric_limits<DWORD>::max()))
		throw SecurePathException(string(Role) + " rename target is too long");
	std::vector<uint8> Storage(Bytes);
	auto *Information = reinterpret_cast<NativeFileRenameInformation *>(Storage.data());
	Information->ReplaceIfExists = ReplaceExisting ? TRUE : FALSE;
	Information->RootDirectory = Destination.Parent.Get();
	Information->FileNameLength = static_cast<DWORD>(Name.size() * sizeof(wchar_t));
	std::copy(Name.begin(), Name.end(), Information->FileName);
	IO_STATUS_BLOCK Status{};
	constexpr ULONG FileRenameInformationClass = 10;
	const NTSTATUS Result =
		GetNtSetInformationFile()(Source.Get(), &Status, Information, static_cast<ULONG>(Bytes), FileRenameInformationClass);
	if (Result < 0)
		throw SecurePathException(string(Role) + " could not complete a handle-relative rename (status " +
								  std::to_string(static_cast<int32>(Result)) + ")");
}

void RemoveHandle(const HANDLE Handle, const string_view Role)
{
#if defined(FILE_DISPOSITION_FLAG_DELETE)
	FILE_DISPOSITION_INFO_EX Extended{};
	Extended.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
	if (SetFileInformationByHandle(Handle, FileDispositionInfoEx, &Extended, sizeof(Extended)) != FALSE)
		return;
#endif
	FILE_DISPOSITION_INFO Information{.DeleteFile = TRUE};
	if (SetFileInformationByHandle(Handle, FileDispositionInfo, &Information, sizeof(Information)) == FALSE)
		throw SecurePathException(string(Role) + " could not delete the handle-relative path");
}

void CopyFileHandles(const HANDLE SourceRoot, const std::filesystem::path &SourceRelative, const HANDLE DestinationRoot,
					 const std::filesystem::path &DestinationRelative, const bool ReplaceExisting, const string_view Role)
{
	NativeHandle Source = OpenObjectRelative(SourceRoot, SourceRelative, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_OPEN,
											 FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	ParentAndLeaf Destination = OpenParent(DestinationRoot, DestinationRelative, true, true, Role);
	NativeHandle Output =
		OpenRelative(Destination.Parent.Get(), Destination.Leaf, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
					 ReplaceExisting ? FILE_OVERWRITE_IF : FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	std::vector<uint8> Buffer(1024U * 1024U);
	for (;;)
	{
		DWORD Read = 0;
		if (ReadFile(Source.Get(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &Read, nullptr) == FALSE)
			throw SecurePathException(string(Role) + " failed while reading a source file");
		if (Read == 0)
			break;
		DWORD Offset = 0;
		while (Offset < Read)
		{
			DWORD Written = 0;
			if (WriteFile(Output.Get(), Buffer.data() + Offset, Read - Offset, &Written, nullptr) == FALSE || Written == 0)
				throw SecurePathException(string(Role) + " failed while writing a destination file");
			Offset += Written;
		}
	}
	if (FlushFileBuffers(Output.Get()) == FALSE)
		throw SecurePathException(string(Role) + " could not durably flush the copied file");
}

void WriteBytes(const HANDLE File, const std::span<const uint8> Bytes, const string_view Role)
{
	usize Offset = 0;
	while (Offset < Bytes.size())
	{
		const usize Remaining = Bytes.size() - Offset;
		const DWORD Requested = static_cast<DWORD>(std::min<usize>(Remaining, std::numeric_limits<DWORD>::max()));
		DWORD Written = 0;
		if (WriteFile(File, Bytes.data() + Offset, Requested, &Written, nullptr) == FALSE || Written == 0)
			throw SecurePathException(string(Role) + " failed while writing a handle-relative file");
		Offset += Written;
	}
}

[[nodiscard]] bool IsDirectoryHandle(const HANDLE Handle, const string_view Role)
{
	FILE_ATTRIBUTE_TAG_INFO Information{};
	if (GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Information, sizeof(Information)) == FALSE)
		throw SecurePathException(string(Role) + " could not inspect a handle-relative path");
	if ((Information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		throw SecurePathException(string(Role) + " encountered a reparse point");
	return (Information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void RemoveTree(const std::filesystem::path &TrustedRoot, const HANDLE RootHandle, const std::filesystem::path &Relative,
				const string_view Role)
{
	NativeHandle Target = OpenObjectRelative(RootHandle, Relative, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Role);
	if (!IsDirectoryHandle(Target.Get(), Role))
	{
		RemoveHandle(Target.Get(), Role);
		return;
	}

	const std::filesystem::path Absolute = SecurePath::ResolveWithin(TrustedRoot, Relative, Role);
	for (const std::filesystem::directory_entry &Entry : std::filesystem::directory_iterator(Absolute))
	{
		const std::filesystem::file_status Status = Entry.symlink_status();
		const DWORD Attributes = GetFileAttributesW(Entry.path().c_str());
		if (std::filesystem::is_symlink(Status) ||
			(Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
			throw SecurePathException(string(Role) + " encountered a reparse point while deleting a directory");
		RemoveTree(TrustedRoot, RootHandle, Relative / Entry.path().filename(), Role);
	}
	RemoveHandle(Target.Get(), Role);
}

void CopyTree(const std::filesystem::path &SourceRoot, const HANDLE SourceHandle, const std::filesystem::path &SourceRelative,
			  const std::filesystem::path &DestinationRoot, const HANDLE DestinationHandle,
			  const std::filesystem::path &DestinationRelative, const bool ReplaceExisting, const string_view Role)
{
	NativeHandle Source = OpenObjectRelative(SourceHandle, SourceRelative, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Role);
	if (!IsDirectoryHandle(Source.Get(), Role))
	{
		CopyFileHandles(SourceHandle, SourceRelative, DestinationHandle, DestinationRelative, ReplaceExisting, Role);
		return;
	}

	SecurePath::CreateDirectoriesWithin(DestinationRoot, DestinationRelative, Role);
	const std::filesystem::path Absolute = SecurePath::ResolveWithin(SourceRoot, SourceRelative, Role);
	for (const std::filesystem::directory_entry &Entry : std::filesystem::directory_iterator(Absolute))
	{
		const std::filesystem::file_status Status = Entry.symlink_status();
		const DWORD Attributes = GetFileAttributesW(Entry.path().c_str());
		if (std::filesystem::is_symlink(Status) ||
			(Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
			throw SecurePathException(string(Role) + " encountered a reparse point while copying a directory");
		CopyTree(SourceRoot, SourceHandle, SourceRelative / Entry.path().filename(), DestinationRoot, DestinationHandle,
				 DestinationRelative / Entry.path().filename(), ReplaceExisting, Role);
	}
}

[[nodiscard]] std::filesystem::path GetFinalPath(const std::filesystem::path &Path, const string_view Role)
{
	const NativeHandle Handle(CreateFileW(Path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
										  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	if (Handle.Get() == INVALID_HANDLE_VALUE)
		throw SecurePathException(string(Role) + " could not open its path boundary");
	const DWORD Required = GetFinalPathNameByHandleW(Handle.Get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (Required == 0)
		throw SecurePathException(string(Role) + " could not resolve its path boundary");
	std::vector<wchar_t> Buffer(static_cast<usize>(Required) + 1U);
	const DWORD Written =
		GetFinalPathNameByHandleW(Handle.Get(), Buffer.data(), static_cast<DWORD>(Buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (Written == 0 || static_cast<usize>(Written) >= Buffer.size())
		throw SecurePathException(string(Role) + " could not resolve its path boundary");
	return std::filesystem::path(Buffer.data(), Buffer.data() + Written).lexically_normal();
}

[[nodiscard]] bool EqualComponent(const std::filesystem::path &Left, const std::filesystem::path &Right)
{
	const std::wstring LeftText = Left.native();
	const std::wstring RightText = Right.native();
	return CompareStringOrdinal(LeftText.c_str(), static_cast<int32>(LeftText.size()), RightText.c_str(),
								static_cast<int32>(RightText.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool IsWithin(const std::filesystem::path &Root, const std::filesystem::path &Candidate)
{
	auto CandidatePart = Candidate.begin();
	for (auto RootPart = Root.begin(); RootPart != Root.end(); ++RootPart, ++CandidatePart)
	{
		if (CandidatePart == Candidate.end() || !EqualComponent(*RootPart, *CandidatePart))
			return false;
	}
	return true;
}
} // namespace

std::filesystem::path SecurePath::ResolveWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
												const string_view Role)
{
	if (TrustedRoot.empty() || RelativePath.empty() || RelativePath.is_absolute() || RelativePath.has_root_name() ||
		RelativePath.has_root_directory())
		throw SecurePathException(string(Role) + " must be a non-empty relative path");
	const std::filesystem::path NormalRelative = RelativePath.lexically_normal();
	for (const std::filesystem::path &Part : NormalRelative)
	{
		if (Part == "." || Part == "..")
			throw SecurePathException(string(Role) + " contains an unsafe path component");
	}
	const std::filesystem::path Result = std::filesystem::absolute(TrustedRoot / NormalRelative).lexically_normal();
	SecurePath::VerifyContained(TrustedRoot, Result, Role);
	return Result;
}

void SecurePath::VerifyContained(const std::filesystem::path &TrustedRoot, const std::filesystem::path &Candidate, const string_view Role)
{
	const std::filesystem::path NormalRoot = std::filesystem::absolute(TrustedRoot).lexically_normal();
	const std::filesystem::path NormalCandidate = std::filesystem::absolute(Candidate).lexically_normal();
	if (!std::filesystem::is_directory(NormalRoot))
		throw SecurePathException(string(Role) + " trusted root does not exist");
	const std::filesystem::path LexicalRelative = NormalCandidate.lexically_relative(NormalRoot);
	if (LexicalRelative.empty() || *LexicalRelative.begin() == "..")
		throw SecurePathException(string(Role) + " escapes its trusted root");

	std::filesystem::path Existing = NormalRoot;
	for (const std::filesystem::path &Part : LexicalRelative)
	{
		if (Part == ".")
			continue;
		const std::filesystem::path Next = Existing / Part;
		const DWORD Attributes = GetFileAttributesW(Next.c_str());
		if (Attributes == INVALID_FILE_ATTRIBUTES)
			break;
		if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			throw SecurePathException(string(Role) + " traverses a reparse point");
		Existing = Next;
	}
	const std::filesystem::path FinalRoot = GetFinalPath(NormalRoot, Role);
	const std::filesystem::path FinalExisting = GetFinalPath(Existing, Role);
	if (!IsWithin(FinalRoot, FinalExisting))
		throw SecurePathException(string(Role) + " resolves outside its trusted root");
}

void SecurePath::CreateTrustedRoot(const std::filesystem::path &TrustedRoot, const string_view Role)
{
	const std::filesystem::path Absolute = std::filesystem::absolute(TrustedRoot).lexically_normal();
	if (Absolute.empty())
		throw SecurePathException(string(Role) + " requires a trusted root path");
	std::filesystem::path Existing = Absolute;
	while (!std::filesystem::exists(Existing))
	{
		const std::filesystem::path Parent = Existing.parent_path();
		if (Parent == Existing || Parent.empty())
			throw SecurePathException(string(Role) + " has no existing trusted ancestor");
		Existing = Parent;
	}
	if (!std::filesystem::is_directory(Existing))
		throw SecurePathException(string(Role) + " trusted ancestor is not a directory");
	const std::filesystem::path Relative = Absolute.lexically_relative(Existing);
	if (Relative.empty() || Relative == ".")
	{
		(void)OpenRoot(Existing, Role);
		return;
	}
	NativeHandle Root = OpenRoot(Existing, Role, true);
	(void)OpenDirectoryChain(Root.Get(), SafeRelative(Relative, Role), true, true, Role);
}

void SecurePath::CreateDirectoriesWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
										 const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role, true);
	(void)OpenDirectoryChain(Root.Get(), SafeRelative(RelativePath, Role), true, true, Role);
}

void SecurePath::CopyWithin(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
							const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative,
							const bool Recursive, const bool ReplaceExisting, const string_view Role)
{
	NativeHandle Source = OpenRoot(SourceRoot, Role);
	NativeHandle Destination = OpenRoot(DestinationRoot, Role, true);
	NativeHandle SourceObject =
		OpenObjectRelative(Source.Get(), SourceRelative, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Role);
	const bool IsDirectory = IsDirectoryHandle(SourceObject.Get(), Role);
	if (IsDirectory && !Recursive)
		throw SecurePathException(string(Role) + " requires recursive copying for a directory");
	if (IsDirectory)
		CopyTree(SourceRoot, Source.Get(), SourceRelative, DestinationRoot, Destination.Get(), DestinationRelative, ReplaceExisting, Role);
	else
		CopyFileHandles(Source.Get(), SourceRelative, Destination.Get(), DestinationRelative, ReplaceExisting, Role);
}

void SecurePath::MoveWithin(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
							const std::filesystem::path &DestinationRoot, const std::filesystem::path &DestinationRelative,
							const bool ReplaceExisting, const string_view Role)
{
	NativeHandle Source = OpenRoot(SourceRoot, Role);
	NativeHandle Destination = OpenRoot(DestinationRoot, Role, true);
	RenameRelative(Source.Get(), SourceRelative, Destination.Get(), DestinationRelative, ReplaceExisting, Role);
}

void SecurePath::RemoveWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath, const bool Recursive,
							  const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role);
	NativeHandle Target =
		OpenObjectRelative(Root.Get(), RelativePath, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Role);
	if (IsDirectoryHandle(Target.Get(), Role) && Recursive)
	{
		Target = NativeHandle(INVALID_HANDLE_VALUE);
		RemoveTree(TrustedRoot, Root.Get(), RelativePath, Role);
		return;
	}
	RemoveHandle(Target.Get(), Role);
}

void SecurePath::ReplaceWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &TemporaryRelative,
							   const std::filesystem::path &DestinationRelative, const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role, true);
	RenameRelative(Root.Get(), TemporaryRelative, Root.Get(), DestinationRelative, true, Role);
}

std::vector<uint8> SecurePath::ReadFileWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
											  const uint64 MaximumBytes, const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role);
	NativeHandle File = OpenObjectRelative(Root.Get(), RelativePath, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_OPEN,
										   FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	FILE_STANDARD_INFO Information{};
	if (GetFileInformationByHandleEx(File.Get(), FileStandardInfo, &Information, sizeof(Information)) == FALSE ||
		Information.EndOfFile.QuadPart < 0)
	{
		throw SecurePathException(string(Role) + " could not inspect a handle-relative file");
	}
	const uint64 Size = static_cast<uint64>(Information.EndOfFile.QuadPart);
	if (Size > MaximumBytes || Size > static_cast<uint64>(std::numeric_limits<usize>::max()))
		throw SecurePathException(string(Role) + " exceeds its configured byte limit");
	std::vector<uint8> Bytes(static_cast<usize>(Size));
	usize Offset = 0;
	while (Offset < Bytes.size())
	{
		const DWORD Requested = static_cast<DWORD>(std::min<usize>(Bytes.size() - Offset, std::numeric_limits<DWORD>::max()));
		DWORD Read = 0;
		if (ReadFile(File.Get(), Bytes.data() + Offset, Requested, &Read, nullptr) == FALSE || Read == 0)
			throw SecurePathException(string(Role) + " could not completely read a handle-relative file");
		Offset += Read;
	}
	return Bytes;
}

uint64 SecurePath::ReadFileChunksWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
										const uint64 MaximumBytes, const SecureFileChunkVisitor &Visitor, const string_view Role)
{
	if (!Visitor)
		throw SecurePathException(string(Role) + " requires a file-chunk visitor");
	NativeHandle Root = OpenRoot(TrustedRoot, Role);
	NativeHandle File = OpenObjectRelative(Root.Get(), RelativePath, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_OPEN,
										   FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	FILE_STANDARD_INFO Information{};
	if (GetFileInformationByHandleEx(File.Get(), FileStandardInfo, &Information, sizeof(Information)) == FALSE ||
		Information.EndOfFile.QuadPart < 0)
	{
		throw SecurePathException(string(Role) + " could not inspect a handle-relative file");
	}
	const uint64 Size = static_cast<uint64>(Information.EndOfFile.QuadPart);
	if (Size > MaximumBytes)
		throw SecurePathException(string(Role) + " exceeds its configured byte limit");
	std::vector<uint8> Buffer(1024U * 1024U);
	uint64 Offset = 0;
	while (Offset < Size)
	{
		const DWORD Requested = static_cast<DWORD>(std::min<uint64>(Buffer.size(), Size - Offset));
		DWORD Read = 0;
		if (ReadFile(File.Get(), Buffer.data(), Requested, &Read, nullptr) == FALSE || Read == 0)
			throw SecurePathException(string(Role) + " could not completely read a handle-relative file");
		Visitor(std::span<const uint8>(Buffer.data(), Read));
		Offset += Read;
	}
	return Size;
}

std::vector<uint8> SecurePath::ReadFileRangeWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
												   const uint64 Offset, const uint64 ByteCount, const uint64 MaximumFileBytes,
												   const string_view Role)
{
	if (ByteCount > static_cast<uint64>(std::numeric_limits<usize>::max()) ||
		Offset > static_cast<uint64>(std::numeric_limits<int64>::max()))
		throw SecurePathException(string(Role) + " range is outside the addressable limit");
	NativeHandle Root = OpenRoot(TrustedRoot, Role);
	NativeHandle File = OpenObjectRelative(Root.Get(), RelativePath, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_OPEN,
										   FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	FILE_STANDARD_INFO Information{};
	if (GetFileInformationByHandleEx(File.Get(), FileStandardInfo, &Information, sizeof(Information)) == FALSE ||
		Information.EndOfFile.QuadPart < 0)
	{
		throw SecurePathException(string(Role) + " could not inspect a handle-relative file");
	}
	const uint64 Size = static_cast<uint64>(Information.EndOfFile.QuadPart);
	if (Size > MaximumFileBytes || Offset > Size || ByteCount > Size - Offset)
		throw SecurePathException(string(Role) + " range exceeds its configured file boundary");
	LARGE_INTEGER Position{};
	Position.QuadPart = static_cast<int64>(Offset);
	if (SetFilePointerEx(File.Get(), Position, nullptr, FILE_BEGIN) == FALSE)
		throw SecurePathException(string(Role) + " could not seek within a handle-relative file");
	std::vector<uint8> Bytes(static_cast<usize>(ByteCount));
	usize ReadOffset = 0;
	while (ReadOffset < Bytes.size())
	{
		const DWORD Requested = static_cast<DWORD>(std::min<usize>(Bytes.size() - ReadOffset, std::numeric_limits<DWORD>::max()));
		DWORD Read = 0;
		if (ReadFile(File.Get(), Bytes.data() + ReadOffset, Requested, &Read, nullptr) == FALSE || Read == 0)
			throw SecurePathException(string(Role) + " could not completely read a handle-relative range");
		ReadOffset += Read;
	}
	return Bytes;
}

void SecurePath::WriteFileWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath,
								 const std::span<const uint8> Bytes, const bool ReplaceExisting, const bool Durable, const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role, true);
	ParentAndLeaf Destination = OpenParent(Root.Get(), RelativePath, true, true, Role);
	NativeHandle File =
		OpenRelative(Destination.Parent.Get(), Destination.Leaf, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
					 ReplaceExisting ? FILE_OVERWRITE_IF : FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, Role);
	WriteBytes(File.Get(), Bytes, Role);
	if (Durable && FlushFileBuffers(File.Get()) == FALSE)
		throw SecurePathException(string(Role) + " could not durably flush a handle-relative file");
}

void SecurePath::WriteFileAtWithin(const std::filesystem::path &TrustedRoot, const std::filesystem::path &RelativePath, const uint64 Offset,
								   const std::span<const uint8> Bytes, const bool Durable, const string_view Role)
{
	NativeHandle Root = OpenRoot(TrustedRoot, Role);
	NativeHandle File =
		OpenObjectRelative(Root.Get(), RelativePath, GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE, Role);
	LARGE_INTEGER Position{};
	Position.QuadPart = static_cast<LONGLONG>(Offset);
	if (SetFilePointerEx(File.Get(), Position, nullptr, FILE_BEGIN) == FALSE)
		throw SecurePathException(string(Role) + " could not seek within a handle-relative file");
	WriteBytes(File.Get(), Bytes, Role);
	if (Durable && FlushFileBuffers(File.Get()) == FALSE)
		throw SecurePathException(string(Role) + " could not durably flush a handle-relative file");
}
} // namespace core::io
