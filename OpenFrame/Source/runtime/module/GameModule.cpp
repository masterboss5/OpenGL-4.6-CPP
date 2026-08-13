#include "GameModule.h"

#include "Source/core/io/SecurePath.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <utility>

namespace runtime::module
{
namespace
{
std::atomic<uint64> NextModuleGeneration = 1;

[[nodiscard]] string NativeError(const string_view Prefix, const uint32 Error)
{
	return string(Prefix) + " (native error " + std::to_string(Error) + ")";
}

void CopyDiagnostic(char *Destination, const usize Capacity, const string_view Diagnostic) noexcept
{
	if (Destination == nullptr || Capacity == 0)
		return;
	const usize Length = std::min(Capacity - 1U, Diagnostic.size());
	std::memcpy(Destination, Diagnostic.data(), Length);
	Destination[Length] = '\0';
}

struct BehaviorCollector final
{
	std::vector<behavior::BehaviorDescriptor> Descriptors;
};

bool RegisterBehavior(void *UserData, const GameModuleBehaviorDescriptor *Descriptor, char *Diagnostic,
					  const usize DiagnosticCapacity) noexcept
{
	try
	{
		if (UserData == nullptr || Descriptor == nullptr)
			throw GameModuleRegistrationException("Game module supplied a null behavior registration");
		if (Descriptor->Name == nullptr)
			throw GameModuleRegistrationException("Game module behavior name is null");
		if ((Descriptor->Properties == nullptr) != (Descriptor->PropertyCount == 0))
			throw GameModuleRegistrationException("Game module behavior property pointer/count pair is inconsistent");
		auto *Collector = static_cast<BehaviorCollector *>(UserData);
		Collector->Descriptors.push_back(
			{.Type = Descriptor->Type,
			 .Name = Descriptor->Name,
			 .SchemaVersion = Descriptor->SchemaVersion,
			 .StateSize = Descriptor->StateSize,
			 .StateAlignment = Descriptor->StateAlignment,
			 .ParallelUpdateSafe = Descriptor->ParallelUpdateSafe != 0,
			 .Properties = Descriptor->Properties == nullptr
							   ? std::vector<behavior::BehaviorPropertyDescriptor>{}
							   : std::vector<behavior::BehaviorPropertyDescriptor>(Descriptor->Properties,
																				   Descriptor->Properties + Descriptor->PropertyCount),
			 .Construct = Descriptor->Construct,
			 .Start = Descriptor->Start,
			 .Update = Descriptor->Update,
			 .FixedUpdate = Descriptor->FixedUpdate,
			 .Stop = Descriptor->Stop,
			 .Destroy = Descriptor->Destroy,
			 .SerializeState = Descriptor->SerializeState,
			 .RestoreState = Descriptor->RestoreState,
			 .MigrateProperties = Descriptor->MigrateProperties});
		return true;
	}
	catch (const std::exception &Exception)
	{
		CopyDiagnostic(Diagnostic, DiagnosticCapacity, Exception.what());
		return false;
	}
	catch (...)
	{
		CopyDiagnostic(Diagnostic, DiagnosticCapacity, "Game module behavior registration failed with a non-standard exception");
		return false;
	}
}

[[nodiscard]] std::filesystem::path CreateLoadDirectory(const std::filesystem::path &CacheRoot, const std::filesystem::path &SourcePath)
{
	const uint64 Generation = NextModuleGeneration.fetch_add(1, std::memory_order_relaxed);
	const uint64 Timestamp = static_cast<uint64>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	const std::filesystem::path Relative =
		std::filesystem::path("GameModules") / SourcePath.stem() / (std::to_string(Timestamp) + "-" + std::to_string(Generation));
	try
	{
		core::io::SecurePath::CreateTrustedRoot(CacheRoot, "game-module cache root");
		core::io::SecurePath::CreateDirectoriesWithin(CacheRoot, Relative, "game-module load directory");
		return core::io::SecurePath::ResolveWithin(CacheRoot, Relative, "game-module load directory");
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw GameModuleLoadException("Could not securely create game-module load directory: " + string(Exception.what()));
	}
}

void CopyLoadFile(const std::filesystem::path &SourceRoot, const std::filesystem::path &SourceRelative,
				  const std::filesystem::path &LoadDirectory, const string_view Role)
{
	try
	{
		core::io::SecurePath::CopyWithin(SourceRoot, SourceRelative, LoadDirectory, SourceRelative.filename(), false, true, Role);
	}
	catch (const core::io::SecurePathException &Exception)
	{
		throw GameModuleLoadException("Could not securely shadow-copy game-module file '" + SourceRelative.string() +
									  "': " + Exception.what());
	}
}

void CopyLoadSet(const std::filesystem::path &SourcePath, const std::filesystem::path &LoadDirectory)
{
	const std::filesystem::path SourceRoot = SourcePath.parent_path();
	std::error_code Error;
	CopyLoadFile(SourceRoot, SourcePath.filename(), LoadDirectory, "game-module shadow copy");

	std::filesystem::directory_iterator Dependencies(SourcePath.parent_path(), Error);
	if (Error)
		throw GameModuleLoadException("Could not enumerate game-module dependencies: " + Error.message());
	for (const std::filesystem::directory_entry &Entry : Dependencies)
	{
		Error.clear();
		if (!Entry.is_regular_file(Error) || Error || Entry.path() == SourcePath)
			continue;
		string Extension = Entry.path().extension().string();
		std::ranges::transform(Extension, Extension.begin(),
							   [](const char Character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Character))); });
		if (Extension != ".dll")
			continue;
		CopyLoadFile(SourceRoot, Entry.path().filename(), LoadDirectory, "game-module dependency shadow copy");
	}

	const std::filesystem::path Symbols = SourcePath.parent_path() / (SourcePath.stem().string() + ".pdb");
	if (std::filesystem::is_regular_file(Symbols, Error) && !Error)
	{
		CopyLoadFile(SourceRoot, Symbols.filename(), LoadDirectory, "game-module symbols shadow copy");
	}
}
} // namespace

class GameModule::Implementation final
{
  public:
	~Implementation()
	{
		if (this->Activated && this->Exports != nullptr && this->Exports->OnUnload != nullptr)
			this->Exports->OnUnload();
		if (this->Library != nullptr)
			FreeLibrary(this->Library);
		try
		{
			if (!this->CacheRoot.empty() && !this->LoadDirectory.empty())
			{
				core::io::SecurePath::RemoveWithin(this->CacheRoot, this->LoadDirectory.lexically_relative(this->CacheRoot), true,
												   "game-module load-directory retirement");
			}
		}
		catch (...)
		{
		}
	}

	std::filesystem::path SourcePath;
	std::filesystem::path CacheRoot;
	std::filesystem::path LoadDirectory;
	std::filesystem::path LoadedPath;
	HMODULE Library = nullptr;
	const GameModuleExports *Exports = nullptr;
	string Name;
	std::vector<behavior::BehaviorDescriptor> Behaviors;
	bool Activated = false;
};

GameModule::~GameModule() = default;

std::unique_ptr<GameModule> GameModule::Load(const std::filesystem::path &SourcePath, const std::filesystem::path &CacheRoot)
{
	if (SourcePath.empty() || CacheRoot.empty())
		throw std::invalid_argument("Game module requires explicit source and cache paths");
	std::error_code Error;
	const std::filesystem::path CanonicalSource = std::filesystem::canonical(SourcePath, Error);
	if (Error || !std::filesystem::is_regular_file(CanonicalSource, Error) || Error)
		throw GameModuleLoadException("Game module source does not exist or is inaccessible: '" + SourcePath.string() + "'");
	const std::filesystem::path AbsoluteCache = std::filesystem::absolute(CacheRoot).lexically_normal();

	auto State = std::make_shared<Implementation>();
	State->SourcePath = CanonicalSource;
	State->CacheRoot = AbsoluteCache;
	State->LoadDirectory = CreateLoadDirectory(AbsoluteCache, CanonicalSource);
	try
	{
		CopyLoadSet(CanonicalSource, State->LoadDirectory);
		State->LoadedPath = State->LoadDirectory / CanonicalSource.filename();
		State->Library =
			LoadLibraryExW(State->LoadedPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (State->Library == nullptr)
		{
			const uint32 ErrorCode = static_cast<uint32>(GetLastError());
			throw GameModuleLoadException(NativeError("Could not load game module '" + State->LoadedPath.string() + "'", ErrorCode));
		}

		const FARPROC Address = GetProcAddress(State->Library, GameModuleExportName.data());
		if (Address == nullptr)
			throw GameModuleABIException("Game module does not export " + string(GameModuleExportName));
		const auto GetExports = reinterpret_cast<GetGameModuleExportsFunction>(Address);
		State->Exports = GetExports();
		if (State->Exports == nullptr)
			throw GameModuleABIException("Game module returned a null export table");
		if (State->Exports->ABIVersion != GameModuleABIVersion)
			throw GameModuleABIException("Game module ABI version does not match the engine ABI");
		if (State->Exports->StructureSize < sizeof(GameModuleExports))
			throw GameModuleABIException("Game module export table is smaller than the required ABI structure");
		if (State->Exports->CompilerVersion != GameModuleCompilerVersion)
			throw GameModuleABIException("Game module compiler version does not match the engine ABI");
		if (State->Exports->BuildFlags != GameModuleBuildFlags)
			throw GameModuleABIException("Game module runtime-library or build-configuration flags do not match the engine ABI");
		if (State->Exports->Name == nullptr || State->Exports->Name[0] == '\0')
			throw GameModuleABIException("Game module export table has no module name");
		if (State->Exports->Register == nullptr)
			throw GameModuleABIException("Game module export table has no registration callback");
		if (State->Exports->PrepareReload == nullptr)
			throw GameModuleABIException("Game module export table has no reload-quiescence callback");
		State->Name = State->Exports->Name;

		BehaviorCollector Collector;
		std::array<char, 2'048> Diagnostic{};
		const GameModuleRegistrar Registrar{.UserData = &Collector, .RegisterBehavior = &RegisterBehavior};
		if (!State->Exports->Register(&Registrar, Diagnostic.data(), Diagnostic.size()))
		{
			const string Detail = Diagnostic.front() == '\0' ? "no diagnostic supplied" : string(Diagnostic.data());
			throw GameModuleRegistrationException("Game module '" + State->Name + "' registration failed: " + Detail);
		}
		for (behavior::BehaviorDescriptor &Descriptor : Collector.Descriptors)
		{
			Descriptor.ModuleName = State->Name;
			Descriptor.StableTypeID = components::MakeBehaviorStableTypeID(State->Name, Descriptor.Name);
		}
		behavior::BehaviorRegistry ValidationRegistry;
		ValidationRegistry.ReplaceAll(Collector.Descriptors);
		State->Behaviors = ValidationRegistry.Snapshot();
		for (behavior::BehaviorDescriptor &Descriptor : State->Behaviors)
			Descriptor.ModuleLease = State;
	}
	catch (...)
	{
		State.reset();
		throw;
	}
	return std::unique_ptr<GameModule>(new GameModule(std::move(State)));
}

void GameModule::PrepareReload()
{
	std::array<char, 2'048> Diagnostic{};
	if (!this->State->Exports->PrepareReload(Diagnostic.data(), Diagnostic.size()))
	{
		throw GameModuleException("Game module '" + this->State->Name + "' could not quiesce for reload: " +
								  (Diagnostic.front() == '\0' ? string("no diagnostic supplied") : string(Diagnostic.data())));
	}
}

void GameModule::Activate() noexcept
{
	if (this->State->Activated)
		return;
	if (this->State->Exports->OnLoad != nullptr)
		this->State->Exports->OnLoad();
	this->State->Activated = true;
}

const string &GameModule::GetName() const noexcept
{
	return this->State->Name;
}

const std::filesystem::path &GameModule::GetSourcePath() const noexcept
{
	return this->State->SourcePath;
}

const std::filesystem::path &GameModule::GetLoadedPath() const noexcept
{
	return this->State->LoadedPath;
}

const std::vector<behavior::BehaviorDescriptor> &GameModule::GetBehaviors() const noexcept
{
	return this->State->Behaviors;
}

GameModule::GameModule(std::shared_ptr<Implementation> Implementation) : State(std::move(Implementation))
{
}

GameModuleManager::GameModuleManager(behavior::BehaviorRegistry &Registry) noexcept : Registry(&Registry)
{
}

GameModuleManager::~GameModuleManager()
{
	this->Registry->Clear();
	this->LoadedModule.reset();
}

void GameModuleManager::Configure(std::filesystem::path SourcePath, std::filesystem::path CacheRoot)
{
	if (SourcePath.empty() || CacheRoot.empty())
		throw std::invalid_argument("Game-module manager requires explicit source and cache paths");
	if (this->LoadedModule != nullptr)
		throw GameModuleException("Unload the current game module before changing its configuration");
	this->SourcePath = std::filesystem::absolute(std::move(SourcePath)).lexically_normal();
	this->CacheRoot = std::filesystem::absolute(std::move(CacheRoot)).lexically_normal();
	this->HasAttemptedWriteTime = false;
	this->ReloadPending = true;
	this->Diagnostic.clear();
}

bool GameModuleManager::PollReload(const bool RuntimeQuiescent)
{
	return this->PollReload(RuntimeQuiescent, {});
}

bool GameModuleManager::PollReload(const bool RuntimeQuiescent, const GameModuleReloadHooks &Hooks)
{
	if (!this->IsConfigured())
		return false;
	std::error_code Error;
	const std::filesystem::file_time_type WriteTime = std::filesystem::last_write_time(this->SourcePath, Error);
	if (Error)
	{
		this->Diagnostic = "Configured game module is unavailable: " + Error.message();
		return false;
	}
	if (this->HasAttemptedWriteTime && WriteTime == this->LastAttemptedWriteTime)
		return false;
	if (!RuntimeQuiescent)
	{
		this->ReloadPending = true;
		return false;
	}
	return this->TryReplace(WriteTime, Hooks);
}

bool GameModuleManager::ForceReload(const bool RuntimeQuiescent)
{
	return this->ForceReload(RuntimeQuiescent, {});
}

bool GameModuleManager::ForceReload(const bool RuntimeQuiescent, const GameModuleReloadHooks &Hooks)
{
	if (!this->IsConfigured())
		throw GameModuleException("Game-module manager is not configured");
	if (!RuntimeQuiescent)
	{
		this->ReloadPending = true;
		return false;
	}
	std::error_code Error;
	const std::filesystem::file_time_type WriteTime = std::filesystem::last_write_time(this->SourcePath, Error);
	if (Error)
	{
		this->Diagnostic = "Configured game module is unavailable: " + Error.message();
		return false;
	}
	return this->TryReplace(WriteTime, Hooks);
}

void GameModuleManager::Unload(const bool RuntimeQuiescent)
{
	if (!RuntimeQuiescent)
		throw GameModuleException("Game module cannot unload while runtime behavior instances may execute its callbacks");
	this->Registry->Clear();
	this->LoadedModule.reset();
	this->ReloadPending = false;
	this->Diagnostic.clear();
}

bool GameModuleManager::IsConfigured() const noexcept
{
	return !this->SourcePath.empty() && !this->CacheRoot.empty();
}

bool GameModuleManager::IsLoaded() const noexcept
{
	return this->LoadedModule != nullptr;
}

bool GameModuleManager::IsReloadPending() const noexcept
{
	return this->ReloadPending;
}

const string &GameModuleManager::GetDiagnostic() const noexcept
{
	return this->Diagnostic;
}

const GameModule *GameModuleManager::GetLoadedModule() const noexcept
{
	return this->LoadedModule.get();
}

bool GameModuleManager::TryReplace(const std::filesystem::file_time_type WriteTime, const GameModuleReloadHooks &Hooks)
{
	this->LastAttemptedWriteTime = WriteTime;
	this->HasAttemptedWriteTime = true;
	bool Prepared = false;
	bool RegistryReplaced = false;
	bool ModulePublished = false;
	std::vector<behavior::BehaviorDescriptor> PreviousDescriptors;
	std::unique_ptr<GameModule> Replacement;
	try
	{
		Replacement = GameModule::Load(this->SourcePath, this->CacheRoot);
		if (this->LoadedModule != nullptr)
			this->LoadedModule->PrepareReload();
		if (Hooks.Prepare)
		{
			Hooks.Prepare();
			Prepared = true;
		}
		PreviousDescriptors = this->Registry->Snapshot();
		this->Registry->ReplaceAll(Replacement->GetBehaviors());
		RegistryReplaced = true;
		Replacement->Activate();
		this->LoadedModule.swap(Replacement);
		ModulePublished = true;
		if (Hooks.Apply)
			Hooks.Apply();
		this->ReloadPending = false;
		this->Diagnostic.clear();
		return true;
	}
	catch (const std::exception &Exception)
	{
		this->Diagnostic = Exception.what();
	}
	catch (...)
	{
		this->Diagnostic = "Game-module replacement failed with a non-standard exception";
	}

	try
	{
		if (ModulePublished)
			this->LoadedModule.swap(Replacement);
		if (RegistryReplaced)
			this->Registry->RestoreSnapshot(PreviousDescriptors);
		if (Prepared && Hooks.Rollback)
			Hooks.Rollback();
	}
	catch (const std::exception &Exception)
	{
		this->Diagnostic += "; rollback failed: " + string(Exception.what());
	}
	catch (...)
	{
		this->Diagnostic += "; rollback failed with a non-standard exception";
	}
	this->ReloadPending = false;
	return false;
}
} // namespace runtime::module
