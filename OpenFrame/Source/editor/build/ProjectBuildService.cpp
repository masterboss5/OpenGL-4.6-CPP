#include "ProjectBuildService.h"

#include "Source/core/io/SecurePath.h"
#include "Source/util/UUID.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace editor::build
{
namespace
{
constexpr usize MaximumDiagnosticBytes = 2U * 1'024U * 1'024U;

class NativeHandle final
{
  public:
	NativeHandle() = default;
	explicit NativeHandle(HANDLE Value) noexcept : Value(Value)
	{
	}
	~NativeHandle()
	{
		if (this->Value != nullptr && this->Value != INVALID_HANDLE_VALUE)
			CloseHandle(this->Value);
	}
	NativeHandle(const NativeHandle &) = delete;
	NativeHandle &operator=(const NativeHandle &) = delete;
	[[nodiscard]] HANDLE Get() const noexcept
	{
		return this->Value;
	}
	[[nodiscard]] HANDLE Release() noexcept
	{
		const HANDLE Result = this->Value;
		this->Value = nullptr;
		return Result;
	}

  private:
	HANDLE Value = nullptr;
};

struct ProcessResult final
{
	uint32 ExitCode = 0;
	bool Cancelled = false;
	string Output;
};

class DirectoryCleanup final
{
  public:
	DirectoryCleanup(std::filesystem::path Root, std::filesystem::path Relative) : Root(std::move(Root)), Relative(std::move(Relative))
	{
	}
	~DirectoryCleanup()
	{
		try
		{
			if (std::filesystem::exists(this->Root / this->Relative))
				core::io::SecurePath::RemoveWithin(this->Root, this->Relative, true, "Project build staging cleanup");
		}
		catch (...)
		{
		}
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path Relative;
};

[[nodiscard]] std::wstring Quote(const std::filesystem::path &Argument)
{
	const std::wstring Value = Argument.native();
	std::wstring Result(1, L'"');
	usize Backslashes = 0;
	for (const wchar_t Character : Value)
	{
		if (Character == L'\\')
		{
			++Backslashes;
			continue;
		}
		if (Character == L'"')
		{
			Result.append(Backslashes * 2U + 1U, L'\\');
			Result.push_back(Character);
			Backslashes = 0;
			continue;
		}
		Result.append(Backslashes, L'\\');
		Backslashes = 0;
		Result.push_back(Character);
	}
	Result.append(Backslashes * 2U, L'\\');
	Result.push_back(L'"');
	return Result;
}

[[nodiscard]] std::wstring BuildCommandLine(const std::filesystem::path &Executable, const std::span<const std::wstring> Arguments)
{
	std::wstring Result = Quote(Executable);
	for (const std::wstring &Argument : Arguments)
	{
		Result.push_back(L' ');
		Result += Quote(std::filesystem::path(Argument));
	}
	return Result;
}

void ValidateMSBuildToken(const string_view Value, const string_view Role)
{
	if (Value.empty() || !std::ranges::all_of(Value,
											  [](const char Character)
											  {
												  return std::isalnum(static_cast<unsigned char>(Character)) != 0 || Character == '_' ||
														 Character == '-' || Character == '.';
											  }))
	{
		throw ProjectBuildException(string("Invalid MSBuild ") + string(Role));
	}
}

[[nodiscard]] string ErrorMessage(const string_view Prefix, const DWORD Code)
{
	return string(Prefix) + ": " + std::system_category().message(static_cast<int32>(Code));
}

void AppendOutput(string &Output, const char *Data, const usize Size, bool &Truncated)
{
	const usize Remaining = Output.size() < MaximumDiagnosticBytes ? MaximumDiagnosticBytes - Output.size() : 0;
	const usize Accepted = std::min(Remaining, Size);
	Output.append(Data, Accepted);
	Truncated |= Accepted != Size;
}

void PublishAtomically(const std::filesystem::path &Source, const std::filesystem::path &Destination)
{
	if (!std::filesystem::is_regular_file(Source))
		throw ProjectBuildException("GameModule build completed without producing '" + Source.string() + "'");
	const std::filesystem::path SourceRoot = Source.parent_path();
	const std::filesystem::path DestinationRoot = Destination.parent_path();
	const std::filesystem::path Temporary = Destination.filename().string() + ".tmp-" + util::UUID::GenerateRandomUUID().ToString();
	core::io::SecurePath::CreateTrustedRoot(DestinationRoot, "Project build publication root");
	try
	{
		core::io::SecurePath::CopyWithin(SourceRoot, Source.filename(), DestinationRoot, Temporary, false, false,
										 "Project build staging copy");
		core::io::SecurePath::ReplaceWithin(DestinationRoot, Temporary, Destination.filename(), "Project build publication");
	}
	catch (...)
	{
		try
		{
			if (std::filesystem::exists(DestinationRoot / Temporary))
				core::io::SecurePath::RemoveWithin(DestinationRoot, Temporary, false, "Failed project build staging cleanup");
		}
		catch (...)
		{
		}
		throw;
	}
}

void PublishDirectoryAtomically(const std::filesystem::path &Staging, const std::filesystem::path &Destination)
{
	const std::filesystem::path Root = Destination.parent_path();
	const std::filesystem::path StagingRelative = Staging.lexically_relative(Root);
	const std::filesystem::path DestinationRelative = Destination.filename();
	const std::filesystem::path BackupRelative = Destination.filename().string() + ".backup-" + util::UUID::GenerateRandomUUID().ToString();
	core::io::SecurePath::CreateTrustedRoot(Root, "Project runtime publication root");
	std::error_code Error;
	const bool HadDestination = std::filesystem::exists(Destination, Error);
	if (Error)
		throw ProjectBuildException("Could not inspect the current runtime build: " + Error.message());
	if (HadDestination)
	{
		core::io::SecurePath::MoveWithin(Root, DestinationRelative, Root, BackupRelative, false,
										 "Project runtime last-known-good preservation");
	}
	try
	{
		core::io::SecurePath::MoveWithin(Root, StagingRelative, Root, DestinationRelative, false, "Validated project runtime publication");
	}
	catch (const std::exception &PublishException)
	{
		if (HadDestination)
		{
			try
			{
				core::io::SecurePath::MoveWithin(Root, BackupRelative, Root, DestinationRelative, false,
												 "Project runtime last-known-good restoration");
			}
			catch (const std::exception &RestoreException)
			{
				throw ProjectBuildException("Runtime publication failed ('" + string(PublishException.what()) +
											"') and last-known-good restoration also failed: " + RestoreException.what());
			}
		}
		throw ProjectBuildException("Could not publish validated runtime build: " + string(PublishException.what()));
	}
	if (HadDestination)
		core::io::SecurePath::RemoveWithin(Root, BackupRelative, true, "Retired project runtime backup");
}

[[nodiscard]] ProcessResult RunProcess(const std::filesystem::path &Executable, const std::span<const std::wstring> Arguments,
									   const std::filesystem::path &WorkingDirectory, const std::atomic<bool> &CancelRequested,
									   const string_view Operation)
{
	const std::wstring CommandLine = BuildCommandLine(Executable, Arguments);
	SECURITY_ATTRIBUTES Security{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
	HANDLE ReadValue = nullptr;
	HANDLE WriteValue = nullptr;
	if (CreatePipe(&ReadValue, &WriteValue, &Security, 0) == FALSE)
		throw ProjectBuildException(ErrorMessage("Could not create " + string(Operation) + " output pipe", GetLastError()));
	NativeHandle Read(ReadValue);
	NativeHandle Write(WriteValue);
	if (SetHandleInformation(Read.Get(), HANDLE_FLAG_INHERIT, 0) == FALSE)
		throw ProjectBuildException(ErrorMessage("Could not protect " + string(Operation) + " output handle", GetLastError()));
	NativeHandle Job(CreateJobObjectW(nullptr, nullptr));
	if (Job.Get() == nullptr)
		throw ProjectBuildException(ErrorMessage("Could not create " + string(Operation) + " job", GetLastError()));
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
	Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (SetInformationJobObject(Job.Get(), JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)) == FALSE)
		throw ProjectBuildException(ErrorMessage("Could not configure " + string(Operation) + " job", GetLastError()));

	std::vector<wchar_t> MutableCommandLine(CommandLine.begin(), CommandLine.end());
	MutableCommandLine.push_back(L'\0');
	STARTUPINFOW Startup{};
	Startup.cb = sizeof(Startup);
	Startup.dwFlags = STARTF_USESTDHANDLES;
	Startup.hStdOutput = Write.Get();
	Startup.hStdError = Write.Get();
	Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION ProcessInformation{};
	if (CreateProcessW(Executable.c_str(), MutableCommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
					   WorkingDirectory.c_str(), &Startup, &ProcessInformation) == FALSE)
		throw ProjectBuildException(ErrorMessage("Could not start " + string(Operation), GetLastError()));
	NativeHandle Process(ProcessInformation.hProcess);
	NativeHandle Thread(ProcessInformation.hThread);
	if (AssignProcessToJobObject(Job.Get(), Process.Get()) == FALSE)
	{
		const DWORD Code = GetLastError();
		(void)TerminateProcess(Process.Get(), ERROR_CANCELLED);
		(void)WaitForSingleObject(Process.Get(), INFINITE);
		throw ProjectBuildException(ErrorMessage("Could not assign " + string(Operation) + " to its cancellation job", Code));
	}
	CloseHandle(Write.Release());

	ProcessResult Result;
	bool Truncated = false;
	std::array<char, 16U * 1'024U> Buffer{};
	for (;;)
	{
		if (CancelRequested.load(std::memory_order_acquire))
		{
			(void)TerminateJobObject(Job.Get(), ERROR_CANCELLED);
			(void)WaitForSingleObject(Process.Get(), INFINITE);
			Result.Cancelled = true;
			Result.Output = string(Operation) + " was cancelled";
			return Result;
		}
		DWORD Available = 0;
		if (PeekNamedPipe(Read.Get(), nullptr, 0, nullptr, &Available, nullptr) == FALSE)
		{
			if (GetLastError() != ERROR_BROKEN_PIPE)
				throw ProjectBuildException(ErrorMessage("Could not inspect " + string(Operation) + " output", GetLastError()));
			Available = 0;
		}
		while (Available != 0)
		{
			DWORD ReadSize = 0;
			const DWORD Requested = std::min<DWORD>(Available, static_cast<DWORD>(Buffer.size()));
			if (ReadFile(Read.Get(), Buffer.data(), Requested, &ReadSize, nullptr) == FALSE)
				break;
			AppendOutput(Result.Output, Buffer.data(), ReadSize, Truncated);
			Available -= ReadSize;
		}
		if (WaitForSingleObject(Process.Get(), 10) == WAIT_OBJECT_0)
			break;
	}
	for (;;)
	{
		DWORD Available = 0;
		if (PeekNamedPipe(Read.Get(), nullptr, 0, nullptr, &Available, nullptr) == FALSE || Available == 0)
			break;
		DWORD ReadSize = 0;
		const DWORD Requested = std::min<DWORD>(Available, static_cast<DWORD>(Buffer.size()));
		if (ReadFile(Read.Get(), Buffer.data(), Requested, &ReadSize, nullptr) == FALSE || ReadSize == 0)
			break;
		AppendOutput(Result.Output, Buffer.data(), ReadSize, Truncated);
	}
	DWORD ExitCode = 0;
	if (GetExitCodeProcess(Process.Get(), &ExitCode) == FALSE)
		throw ProjectBuildException(ErrorMessage("Could not read " + string(Operation) + " exit code", GetLastError()));
	Result.ExitCode = static_cast<uint32>(ExitCode);
	if (Truncated)
		Result.Output += "\n[Process output truncated at 2 MiB]";
	return Result;
}
} // namespace

ProjectBuildService::~ProjectBuildService()
{
	this->Cancel();
	this->Wait();
}

void ProjectBuildService::Configure(GameModuleBuildSpecification Specification)
{
	std::scoped_lock Lock(this->Mutex);
	if (this->State == ProjectBuildState::Building)
		throw ProjectBuildException("Cannot reconfigure a running project build");
	if (Specification.MSBuildExecutable.empty() || Specification.Solution.empty() ||
		Specification.BuiltModule.empty() != Specification.PublishedModule.empty())
		throw ProjectBuildException("Project build configuration requires tools, a solution, and either both or neither module paths");
	Specification.MSBuildExecutable = std::filesystem::absolute(Specification.MSBuildExecutable).lexically_normal();
	Specification.Solution = std::filesystem::absolute(Specification.Solution).lexically_normal();
	if (!Specification.BuiltModule.empty())
	{
		Specification.BuiltModule = std::filesystem::absolute(Specification.BuiltModule).lexically_normal();
		Specification.PublishedModule = std::filesystem::absolute(Specification.PublishedModule).lexically_normal();
	}
	if (!Specification.PackageOutputRoot.empty())
		Specification.PackageOutputRoot = std::filesystem::absolute(Specification.PackageOutputRoot).lexically_normal();
	if (!Specification.EngineContentRoot.empty())
		Specification.EngineContentRoot = std::filesystem::absolute(Specification.EngineContentRoot).lexically_normal();
	if (!std::filesystem::is_regular_file(Specification.MSBuildExecutable) || !std::filesystem::is_regular_file(Specification.Solution) ||
		Specification.Target.empty() || Specification.Configuration.empty() || Specification.Platform.empty() ||
		(Specification.BuiltModule.empty() != Specification.PublishedModule.empty()) ||
		(!Specification.EngineContentRoot.empty() && !std::filesystem::is_directory(Specification.EngineContentRoot)))
	{
		throw ProjectBuildException("Project build configuration is incomplete or references missing tools");
	}
	ValidateMSBuildToken(Specification.Target, "target");
	ValidateMSBuildToken(Specification.Configuration, "configuration");
	ValidateMSBuildToken(Specification.Platform, "platform");
	this->Specification = std::move(Specification);
	this->State = ProjectBuildState::Idle;
	this->Result.reset();
}

void ProjectBuildService::BeginGameModuleBuild(core::threading::TaskScheduler &Scheduler)
{
	this->Wait();
	std::scoped_lock Lock(this->Mutex);
	if (this->Specification.MSBuildExecutable.empty() || this->Specification.BuiltModule.empty() ||
		this->Specification.PublishedModule.empty())
		throw ProjectBuildException("GameModule build paths are not configured");
	if (this->State == ProjectBuildState::Building)
		throw ProjectBuildException("A GameModule build is already running");
	this->CancelRequested.store(false, std::memory_order_release);
	this->State = ProjectBuildState::Building;
	try
	{
		this->Pending = Scheduler.Submit([this]() { return this->ExecuteBuild(false); }, core::threading::TaskPriority::Background);
		this->Result.reset();
	}
	catch (...)
	{
		this->State = ProjectBuildState::Idle;
		throw;
	}
}

void ProjectBuildService::BeginProjectBuild(core::threading::TaskScheduler &Scheduler)
{
	this->Wait();
	std::scoped_lock Lock(this->Mutex);
	if (this->Specification.MSBuildExecutable.empty() || this->Specification.PackageOutputRoot.empty())
		throw ProjectBuildException("Complete project build output is not configured");
	this->CancelRequested.store(false, std::memory_order_release);
	this->State = ProjectBuildState::Building;
	try
	{
		this->Pending = Scheduler.Submit([this]() { return this->ExecuteBuild(true); }, core::threading::TaskPriority::Background);
		this->Result.reset();
	}
	catch (...)
	{
		this->State = ProjectBuildState::Idle;
		throw;
	}
}

bool ProjectBuildService::Poll()
{
	std::scoped_lock Lock(this->Mutex);
	if (!this->Pending.valid() || this->Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return false;
	try
	{
		this->Result = this->Pending.get();
		this->State = this->Result->State;
	}
	catch (const std::exception &Exception)
	{
		this->Result = ProjectBuildResult{.State = ProjectBuildState::Failed, .Diagnostic = Exception.what()};
		this->State = ProjectBuildState::Failed;
	}
	catch (...)
	{
		this->Result = ProjectBuildResult{.State = ProjectBuildState::Failed, .Diagnostic = "Build failed with a non-standard exception"};
		this->State = ProjectBuildState::Failed;
	}
	return true;
}

void ProjectBuildService::Cancel() noexcept
{
	this->CancelRequested.store(true, std::memory_order_release);
}

void ProjectBuildService::Wait() noexcept
{
	std::scoped_lock Lock(this->Mutex);
	if (!this->Pending.valid())
		return;
	try
	{
		this->Result = this->Pending.get();
		this->State = this->Result->State;
	}
	catch (...)
	{
		this->State = ProjectBuildState::Failed;
	}
}

void ProjectBuildService::ReportPostBuildFailure(string Diagnostic)
{
	std::scoped_lock Lock(this->Mutex);
	if (this->State == ProjectBuildState::Building)
		throw ProjectBuildException("Cannot report a post-build failure while a build is active");
	if (!this->Result.has_value())
		this->Result = ProjectBuildResult{};
	this->Result->State = ProjectBuildState::Failed;
	this->Result->Diagnostic = std::move(Diagnostic);
	this->State = ProjectBuildState::Failed;
}

bool ProjectBuildService::IsConfigured() const
{
	std::scoped_lock Lock(this->Mutex);
	return !this->Specification.MSBuildExecutable.empty();
}

bool ProjectBuildService::CanBuildGameModule() const
{
	std::scoped_lock Lock(this->Mutex);
	return !this->Specification.MSBuildExecutable.empty() && !this->Specification.BuiltModule.empty() &&
		   !this->Specification.PublishedModule.empty();
}

ProjectBuildState ProjectBuildService::GetState() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->State;
}

std::optional<ProjectBuildResult> ProjectBuildService::GetResult() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->Result;
}

ProjectBuildResult ProjectBuildService::ExecuteBuild(const bool CompleteProject) const
{
	const bool HasGameModule = !this->Specification.MSBuildExecutable.empty() && !this->Specification.BuiltModule.empty() &&
							   !this->Specification.PublishedModule.empty();
	if (CompleteProject)
	{
		const std::filesystem::path RuntimeDirectory = (this->Specification.PackageOutputRoot / "Current").lexically_normal();
		if (RuntimeDirectory.parent_path() != this->Specification.PackageOutputRoot)
			throw ProjectBuildException("Complete project build output escaped its configured root");
		const std::filesystem::path BuildDirectory =
			this->Specification.PackageOutputRoot / (".staging-" + util::UUID::GenerateRandomUUID().ToString());
		const std::filesystem::path BuildDirectoryRelative = BuildDirectory.lexically_relative(this->Specification.PackageOutputRoot);
		core::io::SecurePath::CreateTrustedRoot(this->Specification.PackageOutputRoot, "Project build output root");
		const DirectoryCleanup Cleanup(this->Specification.PackageOutputRoot, BuildDirectoryRelative);
		if (std::filesystem::exists(BuildDirectory))
		{
			core::io::SecurePath::RemoveWithin(this->Specification.PackageOutputRoot, BuildDirectoryRelative, true,
											   "Previous project build staging cleanup");
		}
		core::io::SecurePath::CreateDirectoriesWithin(this->Specification.PackageOutputRoot, BuildDirectoryRelative,
													  "Project build staging directory");
		std::wstring Targets = L"OpenFrameEngine;OpenFrameTools;OpenFrameEditor;OpenFrameGame;OpenFrameValidation";
		if (HasGameModule)
			Targets += L";" + std::wstring(this->Specification.Target.begin(), this->Specification.Target.end());
		const std::vector<std::wstring> BuildArguments{
			this->Specification.Solution.native(),
			L"/m",
			L"/t:" + Targets,
			L"/p:Configuration=" + std::wstring(this->Specification.Configuration.begin(), this->Specification.Configuration.end()),
			L"/p:Platform=" + std::wstring(this->Specification.Platform.begin(), this->Specification.Platform.end()),
			L"/p:OutDir=" + (BuildDirectory / "").native(),
			L"/v:minimal",
			L"/nologo"};
		ProcessResult Build = RunProcess(this->Specification.MSBuildExecutable, BuildArguments, this->Specification.Solution.parent_path(),
										 this->CancelRequested, "complete project build");
		if (Build.Cancelled)
			return {.State = ProjectBuildState::Cancelled, .Diagnostic = std::move(Build.Output)};
		if (Build.ExitCode != 0)
			return {.State = ProjectBuildState::Failed, .ExitCode = Build.ExitCode, .Diagnostic = std::move(Build.Output)};

		const std::filesystem::path Validation = BuildDirectory / "OpenFrameValidation.exe";
		const std::filesystem::path Editor = BuildDirectory / "OpenFrameEditor.exe";
		const std::filesystem::path EngineToolsLibrary = BuildDirectory / "OpenFrameTools.lib";
		const std::filesystem::path Game = BuildDirectory / "OpenFrameGame.exe";
		const std::filesystem::path BuiltGameModule =
			HasGameModule ? BuildDirectory / this->Specification.BuiltModule.filename() : std::filesystem::path{};
		if (!std::filesystem::is_regular_file(EngineToolsLibrary) || !std::filesystem::is_regular_file(Editor) ||
			!std::filesystem::is_regular_file(Validation) || !std::filesystem::is_regular_file(Game) ||
			(HasGameModule && !std::filesystem::is_regular_file(BuiltGameModule)))
			throw ProjectBuildException(HasGameModule ? "Complete project build did not produce OpenFrameTools, OpenFrameEditor, "
														"OpenFrameGame, OpenFrameValidation, and GameModule outputs"
													  : "Complete project build did not produce OpenFrameTools, OpenFrameEditor, "
														"OpenFrameGame, and OpenFrameValidation outputs");
		const std::filesystem::path EngineLibrary = BuildDirectory / "OpenFrameEngine.dll";
		if (!std::filesystem::is_regular_file(EngineLibrary))
			throw ProjectBuildException("Complete project build did not produce OpenFrameEngine.dll");
		std::vector<std::wstring> ValidationArguments{L"--all"};
		if (HasGameModule)
		{
			ValidationArguments.push_back(L"--game-module");
			ValidationArguments.push_back(BuiltGameModule.native());
			ValidationArguments.push_back(EngineLibrary.native());
		}
		const std::filesystem::path ValidationWorkingDirectory = this->Specification.EngineContentRoot.empty()
																	 ? this->Specification.Solution.parent_path()
																	 : this->Specification.EngineContentRoot;
		ProcessResult ValidationResult =
			RunProcess(Validation, ValidationArguments, ValidationWorkingDirectory, this->CancelRequested, "project validation");
		Build.Output += "\n" + ValidationResult.Output;
		if (ValidationResult.Cancelled)
			return {.State = ProjectBuildState::Cancelled, .Diagnostic = std::move(Build.Output)};
		if (ValidationResult.ExitCode != 0)
			return {.State = ProjectBuildState::Failed,
					.ExitCode = ValidationResult.ExitCode,
					.RuntimeDirectory = RuntimeDirectory,
					.Diagnostic = std::move(Build.Output)};
		if (HasGameModule)
		{
			PublishAtomically(BuiltGameModule, this->Specification.PublishedModule);
			std::filesystem::path BuiltSymbols = BuiltGameModule;
			BuiltSymbols.replace_extension(".pdb");
			if (std::filesystem::is_regular_file(BuiltSymbols))
			{
				std::filesystem::path PublishedSymbols = this->Specification.PublishedModule;
				PublishedSymbols.replace_extension(".pdb");
				PublishAtomically(BuiltSymbols, PublishedSymbols);
			}
		}
		PublishDirectoryAtomically(BuildDirectory, RuntimeDirectory);
		return {.State = ProjectBuildState::Succeeded,
				.ExitCode = 0,
				.PublishedModule = HasGameModule ? this->Specification.PublishedModule : std::filesystem::path{},
				.RuntimeDirectory = RuntimeDirectory,
				.Diagnostic = std::move(Build.Output)};
	}

	const std::vector<std::wstring> BuildArguments{
		this->Specification.Solution.native(),
		L"/m",
		L"/t:" + std::wstring(this->Specification.Target.begin(), this->Specification.Target.end()),
		L"/p:Configuration=" + std::wstring(this->Specification.Configuration.begin(), this->Specification.Configuration.end()),
		L"/p:Platform=" + std::wstring(this->Specification.Platform.begin(), this->Specification.Platform.end()),
		L"/p:BuildProjectReferences=false",
		L"/v:minimal",
		L"/nologo"};
	ProcessResult Build = RunProcess(this->Specification.MSBuildExecutable, BuildArguments, this->Specification.Solution.parent_path(),
									 this->CancelRequested, "GameModule build");
	if (Build.Cancelled)
		return {.State = ProjectBuildState::Cancelled, .Diagnostic = std::move(Build.Output)};
	if (Build.ExitCode != 0)
	{
		if (Build.Output.empty())
			Build.Output = "MSBuild exited with code " + std::to_string(Build.ExitCode) + " without producing diagnostics";
		return {.State = ProjectBuildState::Failed, .ExitCode = Build.ExitCode, .Diagnostic = std::move(Build.Output)};
	}
	PublishAtomically(this->Specification.BuiltModule, this->Specification.PublishedModule);
	std::filesystem::path BuiltSymbols = this->Specification.BuiltModule;
	BuiltSymbols.replace_extension(".pdb");
	if (std::filesystem::is_regular_file(BuiltSymbols))
	{
		std::filesystem::path PublishedSymbols = this->Specification.PublishedModule;
		PublishedSymbols.replace_extension(".pdb");
		PublishAtomically(BuiltSymbols, PublishedSymbols);
	}
	return {.State = ProjectBuildState::Succeeded,
			.ExitCode = 0,
			.PublishedModule = this->Specification.PublishedModule,
			.Diagnostic = std::move(Build.Output)};
}
} // namespace editor::build
