#include "AssetManager.h"

#include "Source/core/io/SecurePath.h"
#include "Source/resource/asset/importer/MaterialAssetImporter.h"
#include "Source/resource/asset/importer/ModelAssetImporter.h"
#include "Source/resource/asset/importer/ShaderSourceImporter.h"
#include "Source/resource/asset/importer/Texture2DImporter.h"

#include <algorithm>
#include <unordered_set>

namespace resource
{
namespace
{
thread_local std::unordered_map<const AssetManager *, uint32> SharedLoadAccessDepth;
thread_local std::vector<const AssetManager *> ExclusiveLoadTransactions;
} // namespace

class AssetLoadSharedAccess final
{
  public:
	explicit AssetLoadSharedAccess(const AssetManager &Manager) : Manager(&Manager)
	{
		if (std::ranges::find(ExclusiveLoadTransactions, &Manager) != ExclusiveLoadTransactions.end())
		{
			this->Manager = nullptr;
			return;
		}
		uint32 &Depth = SharedLoadAccessDepth[&Manager];
		if (Depth++ == 0)
			this->Lock = std::shared_lock(Manager.LoadTransactionMutex);
	}

	~AssetLoadSharedAccess()
	{
		if (this->Manager == nullptr)
			return;
		auto Depth = SharedLoadAccessDepth.find(this->Manager);
		if (Depth == SharedLoadAccessDepth.end() || Depth->second == 0)
			std::terminate();
		if (--Depth->second == 0)
			SharedLoadAccessDepth.erase(Depth);
	}

	AssetLoadSharedAccess(const AssetLoadSharedAccess &) = delete;
	AssetLoadSharedAccess &operator=(const AssetLoadSharedAccess &) = delete;

  private:
	const AssetManager *Manager = nullptr;
	std::shared_lock<std::shared_mutex> Lock;
};

struct AssetLoadTransaction::State final
{
	struct RecordState final
	{
		AssetRecordHandle Lifetime;
		std::filesystem::file_time_type SourceWriteTime{};
		AssetLoadState LoadState = AssetLoadState::Unloaded;
		string Error;
		std::vector<AssetID> Dependencies;
		std::unordered_map<AssetID, std::filesystem::file_time_type> DependencyWriteTimes;
		AssetPtr<resource::Asset> PublishedAsset;
		AssetPtr<resource::Asset> PendingGPUAsset;
		std::vector<AssetID> PendingDependencies;
		std::unordered_map<AssetID, std::filesystem::file_time_type> PendingDependencyWriteTimes;
		std::exception_ptr LoadException;
		uint64 ActiveLoadOperation = 0;
		uint64 PublishedGeneration = 0;
		bool IsImportRoot = false;
		bool HasReadyGeneration = false;
		bool GPURealizationQueued = false;
		bool AcceptingStrongReferences = true;
	};

	AssetManager *Manager = nullptr;
	std::unique_lock<std::shared_mutex> Lock;
	std::thread::id OwnerThread;
	std::unordered_map<AssetID, RecordState> Records;
	std::unordered_map<AssetID, std::unordered_set<AssetID>> ReverseDependencies;
	std::unordered_map<AssetID, std::filesystem::path> DependencyPaths;
	std::deque<AssetID> PendingGPUAssetIDs;

	explicit State(AssetManager &Manager) : Manager(&Manager), OwnerThread(std::this_thread::get_id())
	{
		if (std::ranges::find(ExclusiveLoadTransactions, &Manager) != ExclusiveLoadTransactions.end())
			throw std::logic_error("Asset load transactions cannot be nested for one manager");
		if (SharedLoadAccessDepth.contains(&Manager))
			throw std::logic_error("Asset load transaction cannot begin from inside an ordinary asset-load operation");
		this->Lock = std::unique_lock(Manager.LoadTransactionMutex);
		ExclusiveLoadTransactions.push_back(&Manager);
	}

	~State()
	{
		if (std::this_thread::get_id() != this->OwnerThread || ExclusiveLoadTransactions.empty() ||
			ExclusiveLoadTransactions.back() != this->Manager)
			std::terminate();
		ExclusiveLoadTransactions.pop_back();
	}
};

AssetLoadTransaction::AssetLoadTransaction(AssetManager &Manager) : TransactionState(std::make_unique<State>(Manager))
{
	State &Transaction = *this->TransactionState;
	std::scoped_lock ManagerLock(Manager.Mutex);
	Transaction.Records.reserve(Manager.Records.size());
	for (const auto &[ID, Record] : Manager.Records)
	{
		std::shared_lock PublicationLock(Record->PublicationMutex);
		Transaction.Records.emplace(
			ID, State::RecordState{.Lifetime = AssetRecordHandle(Record),
								   .SourceWriteTime = Record->SourceWriteTime,
								   .LoadState = Record->State,
								   .Error = Record->Error,
								   .Dependencies = Record->Dependencies,
								   .DependencyWriteTimes = Record->DependencyWriteTimes,
								   .PublishedAsset = Record->Asset,
								   .PendingGPUAsset = Record->PendingGPUAsset,
								   .PendingDependencies = Record->PendingDependencies,
								   .PendingDependencyWriteTimes = Record->PendingDependencyWriteTimes,
								   .LoadException = Record->LoadException,
								   .ActiveLoadOperation = Record->ActiveLoadOperation,
								   .PublishedGeneration = Record->PublishedGeneration.load(std::memory_order_acquire),
								   .IsImportRoot = Record->IsImportRoot,
								   .HasReadyGeneration = Record->HasReadyGeneration,
								   .GPURealizationQueued = Record->GPURealizationQueued,
								   .AcceptingStrongReferences = Record->AcceptingStrongReferences.load(std::memory_order_acquire)});
	}
	Transaction.ReverseDependencies = Manager.ReverseDependencies;
	Transaction.DependencyPaths = Manager.DependencyPaths;
	Transaction.PendingGPUAssetIDs = Manager.PendingGPUAssetIDs;
}

AssetLoadTransaction::~AssetLoadTransaction()
{
	if (this->TransactionState == nullptr)
		return;
	try
	{
		this->Rollback();
	}
	catch (...)
	{
		std::terminate();
	}
}

void AssetLoadTransaction::Commit() noexcept
{
	this->TransactionState.reset();
}

void AssetLoadTransaction::Rollback()
{
	if (this->TransactionState == nullptr)
		return;
	State &Transaction = *this->TransactionState;
	AssetManager &Manager = *Transaction.Manager;
	std::vector<AssetRecord *> RemovedRecords;
	{
		std::scoped_lock ManagerLock(Manager.Mutex);
		Manager.ReverseDependencies = Transaction.ReverseDependencies;
		Manager.DependencyPaths = Transaction.DependencyPaths;
		Manager.PendingGPUAssetIDs = Transaction.PendingGPUAssetIDs;
		for (auto &[ID, Saved] : Transaction.Records)
		{
			const auto Current = Manager.Records.find(ID);
			if (Current == Manager.Records.end())
				throw std::logic_error("Asset load transaction baseline record identity changed before rollback");
			AssetRecord *Record = Current->second;
			std::unique_lock PublicationLock(Record->PublicationMutex);
			Record->SourceWriteTime = Saved.SourceWriteTime;
			Record->State = Saved.LoadState;
			Record->Error = std::move(Saved.Error);
			Record->Dependencies = std::move(Saved.Dependencies);
			Record->DependencyWriteTimes = std::move(Saved.DependencyWriteTimes);
			Record->Asset = std::move(Saved.PublishedAsset);
			Record->PendingGPUAsset = std::move(Saved.PendingGPUAsset);
			Record->PendingDependencies = std::move(Saved.PendingDependencies);
			Record->PendingDependencyWriteTimes = std::move(Saved.PendingDependencyWriteTimes);
			Record->LoadException = std::move(Saved.LoadException);
			Record->ActiveLoadOperation = Saved.ActiveLoadOperation;
			Record->PublishedGeneration.store(Saved.PublishedGeneration, std::memory_order_release);
			Record->IsImportRoot = Saved.IsImportRoot;
			Record->HasReadyGeneration = Saved.HasReadyGeneration;
			Record->GPURealizationQueued = Saved.GPURealizationQueued;
			Record->AcceptingStrongReferences.store(Saved.AcceptingStrongReferences, std::memory_order_release);
			PublicationLock.unlock();
			Record->PublicationChanged.notify_all();
		}
		for (auto Iterator = Manager.Records.begin(); Iterator != Manager.Records.end();)
		{
			if (Transaction.Records.contains(Iterator->first))
			{
				++Iterator;
				continue;
			}
			AssetRecord *Record = Iterator->second;
			{
				std::unique_lock PublicationLock(Record->PublicationMutex);
				Record->AcceptingStrongReferences.store(false, std::memory_order_release);
				Record->Asset.Reset();
				Record->PendingGPUAsset.Reset();
				Record->Dependencies.clear();
				Record->DependencyWriteTimes.clear();
				Record->PendingDependencies.clear();
				Record->PendingDependencyWriteTimes.clear();
				Record->State = AssetLoadState::Unloaded;
				Record->HasReadyGeneration = false;
				Record->GPURealizationQueued = false;
				Record->PublicationChanged.notify_all();
			}
			RemovedRecords.push_back(Record);
			Iterator = Manager.Records.erase(Iterator);
		}
	}
	for (AssetRecord *Record : RemovedRecords)
		Record->ReleaseStrong();
	this->TransactionState.reset();
}

bool AssetLoadTransaction::IsActive() const noexcept
{
	return this->TransactionState != nullptr;
}

const AssetID &GeneratedAssetStage::GetID() const
{
	if (!this->Record)
		throw std::logic_error("Generated asset stage is empty");
	return this->Record->GetID();
}

bool GeneratedAssetStage::IsCommitted() const noexcept
{
	return this->Committed;
}

AssetManager::AssetManager(std::filesystem::path RootPath)
	: RootPath(std::filesystem::absolute(RootPath.empty() ? std::filesystem::current_path() : RootPath).lexically_normal())
{
	core::io::SecurePath::CreateTrustedRoot(this->RootPath, "Asset manager root");
	this->AddAssetImporter<importer::Texture2DImporter>();
	this->AddAssetImporter<importer::MaterialAssetImporter>();
	this->AddAssetImporter<importer::MaterialInstanceAssetImporter>();
	this->AddAssetImporter<importer::ModelAssetImporter>();
	this->AddAssetImporter<importer::ShaderSourceImporter>();
}

AssetManager::~AssetManager()
{
	std::scoped_lock Lock(this->Mutex);
	for (auto &[id, record] : this->Records)
	{
		(void)id;
		record->AcceptingStrongReferences.store(false, std::memory_order_release);
		record->ReleaseStrong();
	}
	this->Records.clear();
}

void AssetManager::ReplaceDependencies(AssetRecord &Record, std::vector<AssetID> Dependencies,
									   std::unordered_map<AssetID, std::filesystem::file_time_type> DependencyWriteTimes)
{
	for (const AssetID &PreviousDependency : Record.Dependencies)
	{
		auto Reverse = this->ReverseDependencies.find(PreviousDependency);
		if (Reverse == this->ReverseDependencies.end())
			continue;
		Reverse->second.erase(Record.ID);
		if (Reverse->second.empty())
			this->ReverseDependencies.erase(Reverse);
	}
	Record.Dependencies = std::move(Dependencies);
	Record.DependencyWriteTimes = std::move(DependencyWriteTimes);
	for (const AssetID &Dependency : Record.Dependencies)
		this->ReverseDependencies[Dependency].insert(Record.ID);
}

void AssetManager::QueueGPURealization(AssetRecord &Record)
{
	if (Record.GPURealizationQueued)
		return;
	this->PendingGPUAssetIDs.push_back(Record.ID);
	Record.GPURealizationQueued = true;
}

void AssetManager::RegisterLoadOwner(const AssetID &ID)
{
	std::scoped_lock Lock(this->LoadGraphMutex);
	const std::thread::id CurrentThread = std::this_thread::get_id();
	const auto [Iterator, Inserted] = this->LoadOwners.emplace(ID, CurrentThread);
	if (!Inserted && Iterator->second != CurrentThread)
		throw std::logic_error("Asset load ownership was published by multiple threads");
}

void AssetManager::ReleaseLoadOwner(const AssetID &ID) noexcept
{
	std::scoped_lock Lock(this->LoadGraphMutex);
	const auto Owner = this->LoadOwners.find(ID);
	if (Owner != this->LoadOwners.end() && Owner->second == std::this_thread::get_id())
		this->LoadOwners.erase(Owner);
}

void AssetManager::BeginLoadWait(const AssetID &ID, const AssetType Type, const std::filesystem::path &Path)
{
	std::scoped_lock Lock(this->LoadGraphMutex);
	const std::thread::id CurrentThread = std::this_thread::get_id();
	if (!this->WaitingLoads.emplace(CurrentThread, ID).second)
		throw std::logic_error("A loading thread cannot wait for multiple assets simultaneously");

	const auto RequestedOwner = this->LoadOwners.find(ID);
	if (RequestedOwner == this->LoadOwners.end())
		return;

	std::thread::id OwnerThread = RequestedOwner->second;
	std::unordered_set<std::thread::id> Visited;
	while (Visited.insert(OwnerThread).second)
	{
		if (OwnerThread == CurrentThread)
		{
			this->WaitingLoads.erase(CurrentThread);
			throw importer::AssetDependencyCycleException(Type, Path,
														  "load wait graph reaches the requesting thread for asset '" + ID + "'");
		}
		const auto Waiting = this->WaitingLoads.find(OwnerThread);
		if (Waiting == this->WaitingLoads.end())
			break;
		const auto Owner = this->LoadOwners.find(Waiting->second);
		if (Owner == this->LoadOwners.end())
			break;
		OwnerThread = Owner->second;
	}
}

void AssetManager::EndLoadWait() noexcept
{
	std::scoped_lock Lock(this->LoadGraphMutex);
	this->WaitingLoads.erase(std::this_thread::get_id());
}

AssetRecordHandle AssetManager::LoadRecord(AssetType Type, const std::filesystem::path &Path, bool ForceReload)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	const std::filesystem::path CanonicalPath = this->ResolvePath(Path);
	const AssetID ID = this->ResolveAssetID(Type, CanonicalPath);
	AssetRecord *Record = nullptr;
	std::shared_ptr<importer::AssetImporter> Importer;

	{
		std::scoped_lock Lock(this->Mutex);
		auto Existing = this->Records.find(ID);
		if (Existing == this->Records.end())
		{
			Record = new AssetRecord(ID, CanonicalPath, Type);
			this->Records.emplace(ID, Record);
		}
		else
		{
			Record = Existing->second;
		}
		Record->RetainStrong();

		const usize ImporterIndex = static_cast<usize>(Type);
		if (ImporterIndex < this->AssetImporters.size())
		{
			Importer = this->AssetImporters[ImporterIndex];
		}
	}
	AssetRecordHandle RecordLifetime(Record, AssetRecordHandle::AdoptStrongReference{});

	uint64 LoadOperation = 0;
	{
		std::unique_lock PublicationLock(Record->PublicationMutex);
		bool WaitedForLoad = false;
		while (Record->State == AssetLoadState::LoadingCPU)
		{
			WaitedForLoad = true;
			this->BeginLoadWait(ID, Type, CanonicalPath);
			try
			{
				Record->PublicationChanged.wait(PublicationLock);
			}
			catch (...)
			{
				this->EndLoadWait();
				throw;
			}
			this->EndLoadWait();
		}

		if (!ForceReload && (Record->State == AssetLoadState::CPUReady || Record->State == AssetLoadState::RealizingGPU ||
							 Record->State == AssetLoadState::Ready))
		{
			return RecordLifetime;
		}
		if (!ForceReload && WaitedForLoad && Record->State == AssetLoadState::Failed)
		{
			const std::exception_ptr Failure = Record->LoadException;
			const string Diagnostic = Record->Error;
			PublicationLock.unlock();
			if (Failure != nullptr)
				std::rethrow_exception(Failure);
			throw importer::AssetUnexpectedImportException(Type, CanonicalPath, Diagnostic);
		}

		++Record->ActiveLoadOperation;
		if (Record->ActiveLoadOperation == 0)
			++Record->ActiveLoadOperation;
		LoadOperation = Record->ActiveLoadOperation;
		Record->PendingGPUAsset.Reset();
		Record->PendingDependencies.clear();
		Record->PendingDependencyWriteTimes.clear();
		Record->State = AssetLoadState::LoadingCPU;
		Record->Error.clear();
		Record->LoadException = nullptr;
		Record->IsImportRoot = true;
		this->RegisterLoadOwner(ID);
	}
	const auto ReleaseOwner = [this, ID](AssetManager *) noexcept { this->ReleaseLoadOwner(ID); };
	const std::unique_ptr<AssetManager, decltype(ReleaseOwner)> LoadOwnerGuard(this, ReleaseOwner);

	const auto RecordFailure = [Record, LoadOperation](const string &Error, std::exception_ptr Failure)
	{
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->ActiveLoadOperation != LoadOperation)
			return;
		Record->State = AssetLoadState::Failed;
		Record->Error = Error;
		Record->LoadException = std::move(Failure);
		PublicationLock.unlock();
		Record->PublicationChanged.notify_all();
	};

	if (Importer == nullptr)
	{
		importer::AssetImporterNotRegisteredException Exception(Type, CanonicalPath);
		RecordFailure(Exception.what(), std::make_exception_ptr(Exception));
		throw Exception;
	}

	try
	{
		importer::AssetImportContext ImportContext(
			[this](AssetType ProductType, const std::filesystem::path &ProductPath)
			{ return this->ReserveRecord(ProductType, ProductPath); },
			[this](AssetType DependencyType, const AssetID &ID) { return this->ResolvePublishedRecord(DependencyType, ID); },
			[this](const std::span<AssetRecord *const> Records) { this->RollbackImportReservations(Records); },
			[this](const AssetType OwnerType, const std::filesystem::path &OwnerPath, const std::filesystem::path &RelativePath,
				   const string_view Role)
			{
				if (RelativePath.empty() || RelativePath.is_absolute())
					throw importer::AssetContentValidationException(OwnerType, OwnerPath,
																	string(Role) + " must be a non-empty relative path");
				const std::filesystem::path Candidate = (OwnerPath.parent_path() / RelativePath).lexically_normal();
				const std::filesystem::path RelativeToRoot = Candidate.lexically_relative(this->RootPath);
				try
				{
					return core::io::SecurePath::ResolveWithin(this->RootPath, RelativeToRoot, Role);
				}
				catch (const core::io::SecurePathException &Exception)
				{
					throw importer::AssetContentValidationException(
						OwnerType, OwnerPath, string(Role) + " is outside the trusted asset root: " + Exception.what());
				}
			});
		importer::AssetImportResult Result = Importer->ImportCPU(CanonicalPath, ImportContext);
		std::vector<importer::AssetImportProduct> Products = ImportContext.ReleaseProducts();
		struct PreparedDependency final
		{
			AssetID ID;
			std::filesystem::path Path;
		};
		std::vector<PreparedDependency> RootDependencies;
		RootDependencies.reserve(Result.Dependencies.size());
		for (const AssetDependency &Dependency : Result.Dependencies)
		{
			const std::filesystem::path DependencyPath = this->ResolvePath(Dependency.Path);
			const AssetID DependencyID = this->ResolveAssetID(Dependency.Type, DependencyPath);
			if (DependencyID == Record->ID)
				throw importer::AssetContentValidationException(Type, CanonicalPath, "Asset cannot depend on itself");
			if (Dependency.Type == AssetType::Texture2D || Dependency.Type == AssetType::ShaderSource)
				(void)this->LoadRecord(Dependency.Type, DependencyPath, false);
			RootDependencies.push_back({DependencyID, DependencyPath});
		}

		std::error_code WriteTimeError;
		const std::filesystem::file_time_type SourceWriteTime = std::filesystem::last_write_time(CanonicalPath, WriteTimeError);
		if (WriteTimeError)
		{
			throw importer::AssetFileReadException(Type, CanonicalPath, "Unable to read source write time: " + WriteTimeError.message());
		}

		struct PreparedProduct final
		{
			AssetRecord *Record = nullptr;
			AssetPtr<resource::Asset> Asset;
			std::vector<PreparedDependency> Dependencies;
		};
		std::vector<PreparedProduct> Prepared;
		Prepared.reserve(Products.size() + 1);
		if (Result.Asset == nullptr)
			throw importer::AssetContentValidationException(Type, CanonicalPath, "Importer returned an empty success asset");
		Prepared.push_back({Record, std::move(Result.Asset), std::move(RootDependencies)});
		std::unordered_set<AssetID> ProductIDs;
		ProductIDs.insert(Record->ID);
		struct PendingProduct final
		{
			AssetRecord *Record = nullptr;
			AssetPtr<resource::Asset> Asset;
			std::vector<AssetDependency> Dependencies;
		};
		std::vector<PendingProduct> PendingProducts;
		PendingProducts.reserve(Products.size());
		for (auto &Product : Products)
		{
			const std::filesystem::path ProductPath = this->ResolvePath(Product.CanonicalPath);
			const AssetID ProductID = this->ResolveAssetID(Product.Type, ProductPath);
			if (!ProductIDs.insert(ProductID).second)
			{
				throw importer::AssetContentValidationException(Type, CanonicalPath,
																"Importer produced duplicate subasset identity " + ProductID);
			}
			importer::AssetImportReservation ProductReservation = this->ReserveRecord(Product.Type, ProductPath);
			AssetRecord *ProductRecord = ProductReservation.Handle.Record;
			PendingProducts.push_back({ProductRecord, std::move(Product.Asset), std::move(Product.Dependencies)});
		}
		for (PendingProduct &Product : PendingProducts)
		{
			std::vector<PreparedDependency> ProductDependencies;
			ProductDependencies.reserve(Product.Dependencies.size());
			for (const AssetDependency &Dependency : Product.Dependencies)
			{
				const std::filesystem::path DependencyPath = this->ResolvePath(Dependency.Path);
				const AssetID DependencyID = this->ResolveAssetID(Dependency.Type, DependencyPath);
				if (!ProductIDs.contains(DependencyID) &&
					(Dependency.Type == AssetType::Texture2D || Dependency.Type == AssetType::ShaderSource))
				{
					(void)this->LoadRecord(Dependency.Type, DependencyPath, false);
				}
				ProductDependencies.push_back({std::move(DependencyID), DependencyPath});
			}
			if (Product.Asset == nullptr)
				throw importer::AssetContentValidationException(Type, CanonicalPath, "Importer staged an empty subasset");
			Prepared.push_back({Product.Record, std::move(Product.Asset), std::move(ProductDependencies)});
		}

		std::sort(Prepared.begin(), Prepared.end(),
				  [](const PreparedProduct &Left, const PreparedProduct &Right) { return Left.Record->ID < Right.Record->ID; });
		std::scoped_lock Lock(this->Mutex);
		std::vector<std::unique_lock<std::shared_mutex>> PublicationLocks;
		PublicationLocks.reserve(Prepared.size());
		for (PreparedProduct &Product : Prepared)
		{
			PublicationLocks.emplace_back(Product.Record->PublicationMutex);
		}
		if (Record->ActiveLoadOperation != LoadOperation)
			return RecordLifetime;
		for (PreparedProduct &Product : Prepared)
		{
			std::vector<AssetID> Dependencies;
			std::unordered_map<AssetID, std::filesystem::file_time_type> DependencyWriteTimes;
			Dependencies.reserve(Product.Dependencies.size());
			for (const PreparedDependency &Dependency : Product.Dependencies)
			{
				Dependencies.push_back(Dependency.ID);
				this->DependencyPaths.insert_or_assign(Dependency.ID, Dependency.Path);
				std::error_code DependencyTimeError;
				const std::filesystem::file_time_type DependencyWriteTime =
					std::filesystem::last_write_time(Dependency.Path, DependencyTimeError);
				if (!DependencyTimeError)
					DependencyWriteTimes.emplace(Dependency.ID, DependencyWriteTime);
			}
			const bool RequiresGPURealization = Product.Asset->RequiresGPURealization();
			Product.Record->SourceWriteTime = SourceWriteTime;
			Product.Record->Error.clear();
			Product.Record->LoadException = nullptr;
			if (RequiresGPURealization && Product.Record->HasReadyGeneration && Product.Record->Asset != nullptr)
			{
				Product.Record->PendingGPUAsset = std::move(Product.Asset);
				Product.Record->PendingDependencies = std::move(Dependencies);
				Product.Record->PendingDependencyWriteTimes = std::move(DependencyWriteTimes);
				Product.Record->State = AssetLoadState::Ready;
				this->QueueGPURealization(*Product.Record);
				continue;
			}
			Product.Record->PendingGPUAsset.Reset();
			Product.Record->PendingDependencies.clear();
			Product.Record->PendingDependencyWriteTimes.clear();
			this->ReplaceDependencies(*Product.Record, std::move(Dependencies), std::move(DependencyWriteTimes));
			Product.Record->Asset = std::move(Product.Asset);
			Product.Record->State = RequiresGPURealization ? AssetLoadState::CPUReady : AssetLoadState::Ready;
			Product.Record->HasReadyGeneration = !RequiresGPURealization;
			if (RequiresGPURealization)
				this->QueueGPURealization(*Product.Record);
			Product.Record->PublishedGeneration.fetch_add(1, std::memory_order_release);
		}
		for (PreparedProduct &Product : Prepared)
			Product.Record->PublicationChanged.notify_all();
		ImportContext.Commit();
		return RecordLifetime;
	}
	catch (const importer::AssetImportException &Exception)
	{
		RecordFailure(Exception.what(), std::current_exception());
		throw;
	}
	catch (const std::exception &Exception)
	{
		importer::AssetUnexpectedImportException WrappedException(Type, CanonicalPath, Exception.what());
		RecordFailure(WrappedException.what(), std::make_exception_ptr(WrappedException));
		throw WrappedException;
	}
	catch (...)
	{
		importer::AssetUnexpectedImportException WrappedException(Type, CanonicalPath, "Unknown non-standard exception");
		RecordFailure(WrappedException.what(), std::make_exception_ptr(WrappedException));
		throw WrappedException;
	}
}

AssetLoadTransaction AssetManager::BeginLoadTransaction()
{
	return AssetLoadTransaction(*this);
}

importer::AssetImportReservation AssetManager::ReserveRecord(AssetType Type, const std::filesystem::path &Path)
{
	const std::filesystem::path CanonicalPath = this->ResolvePath(Path);
	const AssetID ID = this->ResolveAssetID(Type, CanonicalPath);
	std::scoped_lock Lock(this->Mutex);
	const auto Existing = this->Records.find(ID);
	if (Existing != this->Records.end())
	{
		if (Existing->second->Type != Type)
		{
			throw std::logic_error("Asset record identity was reserved with a conflicting type");
		}
		return {.Handle = AssetRecordHandle(Existing->second), .WasCreated = false};
	}
	AssetRecord *Record = new AssetRecord(ID, CanonicalPath, Type);
	this->Records.emplace(ID, Record);
	return {.Handle = AssetRecordHandle(Record), .WasCreated = true};
}

void AssetManager::RollbackImportReservations(const std::span<AssetRecord *const> Reservations) noexcept
{
	std::scoped_lock Lock(this->Mutex);
	for (AssetRecord *const Record : Reservations)
	{
		if (Record == nullptr)
			continue;
		const auto Existing = this->Records.find(Record->ID);
		if (Existing == this->Records.end() || Existing->second != Record)
			continue;
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->StrongReferences.load(std::memory_order_acquire) != 1 || Record->State != AssetLoadState::Unloaded ||
			Record->Asset != nullptr || Record->PendingGPUAsset != nullptr || !Record->Dependencies.empty() || Record->IsImportRoot ||
			this->PublishedAssets.contains(Record->ID) || this->GeneratedAssets.contains(Record->ID))
			continue;
		Record->AcceptingStrongReferences.store(false, std::memory_order_release);
		if (Record->StrongReferences.load(std::memory_order_acquire) != 1)
		{
			Record->AcceptingStrongReferences.store(true, std::memory_order_release);
			continue;
		}
		this->Records.erase(Existing);
		PublicationLock.unlock();
		Record->ReleaseStrong();
	}
}

AssetRecordHandle AssetManager::ResolvePublishedRecord(const AssetType Type, const AssetID &ID)
{
	std::filesystem::path CanonicalPath;
	{
		std::scoped_lock Lock(this->Mutex);
		const auto Published = this->PublishedAssets.find(ID);
		if (Published != this->PublishedAssets.end())
		{
			if (Published->second.Type != Type)
				throw AssetTypeMismatchException(ID, Published->second.Type);
			CanonicalPath = Published->second.CanonicalPath;
		}
		else
		{
			const auto Generated = this->GeneratedAssets.find(ID);
			if (Generated == this->GeneratedAssets.end())
				throw AssetUnavailableException(ID, Type, "asset ID is not registered");
			if (Generated->second.Type != Type)
				throw AssetTypeMismatchException(ID, Generated->second.Type);
			CanonicalPath = Generated->second.CanonicalPath;
		}
	}
	importer::AssetImportReservation Reservation = this->ReserveRecord(Type, CanonicalPath);
	return std::move(Reservation.Handle);
}

AssetRecordHandle AssetManager::PublishGeneratedAssetRecord(AssetID ID, const std::filesystem::path &Path, const AssetType Type,
															AssetPtr<Asset> Asset, std::vector<AssetID> Dependencies)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	if (ID.empty() || Type == AssetType::Count || !Asset)
		throw std::invalid_argument("Generated asset publication requires an ID, concrete type, and owned asset");
	const std::filesystem::path CanonicalPath = this->ResolvePath(Path);
	const AssetID PathKey = AssetManager::MakeAssetID(Type, CanonicalPath);
	std::ranges::sort(Dependencies);
	if (std::ranges::adjacent_find(Dependencies) != Dependencies.end())
		throw std::invalid_argument("Generated asset publication contains duplicate dependencies");
	if (std::ranges::find(Dependencies, ID) != Dependencies.end())
		throw std::invalid_argument("Generated asset publication cannot depend on itself");

	std::scoped_lock ManagerLock(this->Mutex);
	AssetRecord *Record = nullptr;
	const auto Existing = this->Records.find(ID);
	if (Existing == this->Records.end())
	{
		Record = new AssetRecord(ID, CanonicalPath, Type);
		this->Records.emplace(ID, Record);
	}
	else
	{
		Record = Existing->second;
		if (Record->Type != Type || Record->CanonicalPath != CanonicalPath)
			throw std::invalid_argument("Generated asset ID is already bound to a different type or canonical path");
	}
	const auto PublishedPath = this->PublishedIDsByPathKey.find(PathKey);
	const auto GeneratedPath = this->GeneratedIDsByPathKey.find(PathKey);
	if ((PublishedPath != this->PublishedIDsByPathKey.end() && PublishedPath->second != ID) ||
		(GeneratedPath != this->GeneratedIDsByPathKey.end() && GeneratedPath->second != ID))
		throw std::invalid_argument("Generated asset path is already bound to a different asset ID");

	std::unique_lock PublicationLock(Record->PublicationMutex);
	const bool RequiresGPURealization = Asset->RequiresGPURealization();
	if (RequiresGPURealization && Record->HasReadyGeneration && Record->Asset != nullptr)
	{
		Record->PendingGPUAsset = std::move(Asset);
		Record->PendingDependencies = Dependencies;
		Record->PendingDependencyWriteTimes.clear();
		Record->State = AssetLoadState::Ready;
		this->QueueGPURealization(*Record);
	}
	else
	{
		Record->PendingGPUAsset.Reset();
		Record->PendingDependencies.clear();
		Record->PendingDependencyWriteTimes.clear();
		this->ReplaceDependencies(*Record, Dependencies, {});
		Record->Asset = std::move(Asset);
		Record->State = RequiresGPURealization ? AssetLoadState::CPUReady : AssetLoadState::Ready;
		Record->HasReadyGeneration = !RequiresGPURealization;
		if (RequiresGPURealization)
			this->QueueGPURealization(*Record);
		Record->PublishedGeneration.fetch_add(1, std::memory_order_release);
	}
	Record->Error.clear();
	Record->LoadException = nullptr;
	Record->IsImportRoot = true;
	this->GeneratedAssets.insert_or_assign(
		ID, AssetPublication{.ID = ID, .CanonicalPath = CanonicalPath, .Type = Type, .Dependencies = Record->Dependencies});
	this->GeneratedIDsByPathKey.insert_or_assign(PathKey, ID);
	PublicationLock.unlock();
	Record->PublicationChanged.notify_all();
	return AssetRecordHandle(Record);
}

GeneratedAssetStage AssetManager::StageGeneratedAssetRecord(AssetID ID, const std::filesystem::path &Path, const AssetType Type,
															AssetPtr<Asset> Asset, std::vector<AssetID> Dependencies)
{
	if (ID.empty() || Type == AssetType::Count || !Asset)
		throw std::invalid_argument("Generated asset staging requires an ID, concrete type, and owned asset");
	const std::filesystem::path CanonicalPath = this->ResolvePath(Path);
	std::ranges::sort(Dependencies);
	if (std::ranges::adjacent_find(Dependencies) != Dependencies.end())
		throw std::invalid_argument("Generated asset staging contains duplicate dependencies");
	if (std::ranges::find(Dependencies, ID) != Dependencies.end())
		throw std::invalid_argument("Generated asset staging cannot depend on itself");

	AssetRecord *Record = new AssetRecord(std::move(ID), CanonicalPath, Type);
	{
		std::unique_lock PublicationLock(Record->PublicationMutex);
		Record->Asset = std::move(Asset);
		Record->State = Record->Asset->RequiresGPURealization() ? AssetLoadState::CPUReady : AssetLoadState::Ready;
		Record->HasReadyGeneration = !Record->Asset->RequiresGPURealization();
		Record->IsImportRoot = true;
	}
	return GeneratedAssetStage(AssetRecordHandle(Record, AssetRecordHandle::AdoptStrongReference{}), std::move(Dependencies));
}

void AssetManager::CommitGeneratedAssets(const std::span<GeneratedAssetStage *const> Stages)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	if (Stages.empty())
		throw std::invalid_argument("Generated asset commit requires at least one stage");
	std::unordered_set<AssetID> IDs;
	std::unordered_set<AssetID> PathKeys;
	for (GeneratedAssetStage *Stage : Stages)
	{
		if (Stage == nullptr || !Stage->Record || Stage->Committed)
			throw std::invalid_argument("Generated asset commit contains an invalid or already committed stage");
		const AssetRecord &Record = *Stage->Record;
		if (!IDs.insert(Record.ID).second || !PathKeys.insert(AssetManager::MakeAssetID(Record.Type, Record.CanonicalPath)).second)
			throw std::invalid_argument("Generated asset commit contains duplicate IDs or paths");
	}

	std::vector<AssetRecord *> ReplacedRecords;
	{
		std::scoped_lock ManagerLock(this->Mutex);
		auto NewRecords = this->Records;
		auto NewPublishedAssets = this->PublishedAssets;
		auto NewGeneratedAssets = this->GeneratedAssets;
		auto NewGeneratedPaths = this->GeneratedIDsByPathKey;
		auto NewReverseDependencies = this->ReverseDependencies;
		auto NewPendingGPU = this->PendingGPUAssetIDs;
		for (GeneratedAssetStage *Stage : Stages)
		{
			AssetRecord *Record = Stage->Record.Record;
			const AssetID PathKey = AssetManager::MakeAssetID(Record->Type, Record->CanonicalPath);
			const auto Published = NewPublishedAssets.find(Record->ID);
			const auto PublishedPath = this->PublishedIDsByPathKey.find(PathKey);
			const auto Generated = NewGeneratedAssets.find(Record->ID);
			const auto GeneratedPath = NewGeneratedPaths.find(PathKey);
			const bool ReplacesPublishedAsset = Published != NewPublishedAssets.end() || PublishedPath != this->PublishedIDsByPathKey.end();
			if (ReplacesPublishedAsset && (Published == NewPublishedAssets.end() || PublishedPath == this->PublishedIDsByPathKey.end() ||
										   PublishedPath->second != Record->ID || Published->second.Type != Record->Type ||
										   Published->second.CanonicalPath != Record->CanonicalPath))
			{
				throw std::invalid_argument("Generated asset replacement does not exactly match the published asset identity");
			}
			if (ReplacesPublishedAsset && (Generated != NewGeneratedAssets.end() || GeneratedPath != NewGeneratedPaths.end()) &&
				(Generated == NewGeneratedAssets.end() || GeneratedPath == NewGeneratedPaths.end() || GeneratedPath->second != Record->ID ||
				 Generated->second.Type != Record->Type || Generated->second.CanonicalPath != Record->CanonicalPath))
			{
				throw std::invalid_argument("Published asset replacement conflicts with a different generated asset identity");
			}
			if (!ReplacesPublishedAsset)
			{
				if (GeneratedPath != NewGeneratedPaths.end() && GeneratedPath->second != Record->ID)
				{
					throw std::invalid_argument("Generated asset path is already bound to a different asset ID");
				}
			}
			if (const auto Existing = NewRecords.find(Record->ID); Existing != NewRecords.end())
			{
				if (Existing->second->Type != Record->Type || Existing->second->CanonicalPath != Record->CanonicalPath)
					throw std::invalid_argument("Generated asset ID is already bound to a different type or path");
				ReplacedRecords.push_back(Existing->second);
				for (const AssetID &Dependency : Existing->second->Dependencies)
				{
					auto Reverse = NewReverseDependencies.find(Dependency);
					if (Reverse != NewReverseDependencies.end())
					{
						Reverse->second.erase(Record->ID);
						if (Reverse->second.empty())
							NewReverseDependencies.erase(Reverse);
					}
				}
			}
			NewRecords.insert_or_assign(Record->ID, Record);
			if (ReplacesPublishedAsset)
			{
				AssetPublication Replacement = Published->second;
				Replacement.Dependencies = Stage->Dependencies;
				NewPublishedAssets.insert_or_assign(Record->ID, std::move(Replacement));
				if (Generated != NewGeneratedAssets.end())
				{
					AssetPublication GeneratedReplacement = Generated->second;
					GeneratedReplacement.Dependencies = Stage->Dependencies;
					NewGeneratedAssets.insert_or_assign(Record->ID, std::move(GeneratedReplacement));
				}
			}
			else
			{
				NewGeneratedPaths.insert_or_assign(PathKey, Record->ID);
				NewGeneratedAssets.insert_or_assign(Record->ID, AssetPublication{.ID = Record->ID,
																				 .CanonicalPath = Record->CanonicalPath,
																				 .Type = Record->Type,
																				 .Dependencies = Stage->Dependencies});
			}
			for (const AssetID &Dependency : Stage->Dependencies)
				NewReverseDependencies[Dependency].insert(Record->ID);
			if (Record->State == AssetLoadState::CPUReady)
				NewPendingGPU.push_back(Record->ID);
		}

		for (GeneratedAssetStage *Stage : Stages)
		{
			AssetRecord *Record = Stage->Record.Record;
			Record->Dependencies = std::move(Stage->Dependencies);
			Record->GPURealizationQueued = Record->State == AssetLoadState::CPUReady;
			Record->PublishedGeneration.store(1, std::memory_order_release);
			Record->RetainStrong();
			Stage->Committed = true;
		}
		this->Records.swap(NewRecords);
		this->PublishedAssets.swap(NewPublishedAssets);
		this->GeneratedAssets.swap(NewGeneratedAssets);
		this->GeneratedIDsByPathKey.swap(NewGeneratedPaths);
		this->ReverseDependencies.swap(NewReverseDependencies);
		this->PendingGPUAssetIDs.swap(NewPendingGPU);
	}
	for (AssetRecord *Record : ReplacedRecords)
		Record->ReleaseStrong();
}

GeneratedAssetRetirementAttempt AssetManager::TryRetireGeneratedAsset(const AssetID &ID)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	AssetRecord *Record = nullptr;
	GeneratedAssetRetirement Retirement;
	{
		std::scoped_lock ManagerLock(this->Mutex);
		const auto Generated = this->GeneratedAssets.find(ID);
		if (Generated == this->GeneratedAssets.end())
			return {.Status = GeneratedAssetRetirementStatus::NotGenerated};
		const auto Existing = this->Records.find(ID);
		if (Existing == this->Records.end())
			return {.Status = GeneratedAssetRetirementStatus::Busy};
		Record = Existing->second;
		if (const auto Dependents = this->ReverseDependencies.find(ID);
			Dependents != this->ReverseDependencies.end() && !Dependents->second.empty())
		{
			return {.Status = GeneratedAssetRetirementStatus::HasDependents};
		}
		Record->AcceptingStrongReferences.store(false, std::memory_order_release);
		if (Record->StrongReferences.load(std::memory_order_acquire) != 1)
		{
			Record->AcceptingStrongReferences.store(true, std::memory_order_release);
			return {.Status = GeneratedAssetRetirementStatus::Referenced};
		}

		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->State == AssetLoadState::LoadingCPU || Record->State == AssetLoadState::RealizingGPU ||
			Record->PendingGPUAsset != nullptr || Record->GPURealizationQueued || Record->Asset == nullptr)
		{
			Record->AcceptingStrongReferences.store(true, std::memory_order_release);
			return {.Status = GeneratedAssetRetirementStatus::Busy};
		}

		Retirement.Publication = Generated->second;
		Retirement.Publication.Dependencies = Record->Dependencies;
		if (const auto Published = this->PublishedAssets.find(ID); Published != this->PublishedAssets.end())
		{
			Retirement.Publication.VirtualPath = Published->second.VirtualPath;
			Retirement.WasRegistryPublished = true;
		}
		Retirement.Asset = std::move(Record->Asset);
		this->ReplaceDependencies(*Record, {}, {});
		Record->State = AssetLoadState::Unloaded;
		Record->HasReadyGeneration = false;
		Record->IsImportRoot = false;

		const AssetID PathKey = MakeAssetID(Retirement.Publication.Type, Retirement.Publication.CanonicalPath);
		this->GeneratedAssets.erase(Generated);
		if (const auto Path = this->GeneratedIDsByPathKey.find(PathKey); Path != this->GeneratedIDsByPathKey.end() && Path->second == ID)
		{
			this->GeneratedIDsByPathKey.erase(Path);
		}
		this->PublishedAssets.erase(ID);
		if (const auto Path = this->PublishedIDsByPathKey.find(PathKey); Path != this->PublishedIDsByPathKey.end() && Path->second == ID)
		{
			this->PublishedIDsByPathKey.erase(Path);
		}
		if (!Retirement.Publication.VirtualPath.empty())
		{
			const auto Virtual = this->PublishedIDsByVirtualPath.find(Retirement.Publication.VirtualPath);
			if (Virtual != this->PublishedIDsByVirtualPath.end() && Virtual->second == ID)
				this->PublishedIDsByVirtualPath.erase(Virtual);
		}
		this->DependencyPaths.erase(ID);
		this->PendingGPUAssetIDs.erase(std::remove(this->PendingGPUAssetIDs.begin(), this->PendingGPUAssetIDs.end(), ID),
									   this->PendingGPUAssetIDs.end());
		this->Records.erase(Existing);
	}
	Record->ReleaseStrong();
	return {.Status = GeneratedAssetRetirementStatus::Retired, .Retirement = std::move(Retirement)};
}

void AssetManager::RestoreRetiredGeneratedAsset(GeneratedAssetRetirement Retirement)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	if (Retirement.Publication.ID.empty() || Retirement.Publication.Type == AssetType::Count || !Retirement.Asset)
		throw std::invalid_argument("Generated asset restoration requires a complete retirement token");
	const AssetPublication Publication = Retirement.Publication;
	(void)this->PublishGeneratedAssetRecord(Publication.ID, Publication.CanonicalPath, Publication.Type, std::move(Retirement.Asset),
											Publication.Dependencies);
	if (!Retirement.WasRegistryPublished)
		return;

	const AssetID PathKey = MakeAssetID(Publication.Type, Publication.CanonicalPath);
	std::scoped_lock Lock(this->Mutex);
	this->PublishedAssets.insert_or_assign(Publication.ID, Publication);
	this->PublishedIDsByPathKey.insert_or_assign(PathKey, Publication.ID);
	if (!Publication.VirtualPath.empty())
		this->PublishedIDsByVirtualPath.insert_or_assign(Publication.VirtualPath, Publication.ID);
}

bool AssetManager::RealizeGPU(pipeline::device::Device &Device, const AssetID &ID)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	AssetRecord *Record = nullptr;
	AssetPtr<resource::Asset> Asset;
	uint64 Generation = 0;
	bool PendingReplacement = false;
	{
		std::scoped_lock Lock(this->Mutex);
		auto Iterator = this->Records.find(ID);
		if (Iterator == this->Records.end())
		{
			return false;
		}

		Record = Iterator->second;
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->PendingGPUAsset != nullptr)
		{
			Asset = Record->PendingGPUAsset;
			PendingReplacement = true;
		}
		else if (Record->State == AssetLoadState::Ready)
		{
			return true;
		}
		else if (Record->State == AssetLoadState::CPUReady && Record->Asset != nullptr)
		{
			Asset = Record->Asset;
		}
		else
		{
			return false;
		}

		Record->State = AssetLoadState::RealizingGPU;
		Generation = Record->PublishedGeneration.load(std::memory_order_acquire);
	}

	try
	{
		AssetGPURealizationResult Result = Asset->RealizeGPU(Device);
		std::scoped_lock ManagerLock(this->Mutex);
		std::unique_lock PublicationLock(Record->PublicationMutex);
		const bool CandidateMatches =
			PendingReplacement ? Record->PendingGPUAsset.Get() == Asset.Get() : Record->Asset.Get() == Asset.Get();
		if (Record->PublishedGeneration.load(std::memory_order_acquire) != Generation || !CandidateMatches ||
			Record->State != AssetLoadState::RealizingGPU)
		{
			return false;
		}
		if (Result.Succeeded)
		{
			if (PendingReplacement)
			{
				this->ReplaceDependencies(*Record, std::move(Record->PendingDependencies), std::move(Record->PendingDependencyWriteTimes));
				Record->Asset = std::move(Record->PendingGPUAsset);
				Record->PublishedGeneration.fetch_add(1, std::memory_order_release);
				const auto Generated = this->GeneratedAssets.find(ID);
				if (Generated != this->GeneratedAssets.end())
					Generated->second.Dependencies = Record->Dependencies;
			}
			Record->State = AssetLoadState::Ready;
			Record->HasReadyGeneration = true;
			Record->Error.clear();
		}
		else
		{
			Record->PendingGPUAsset.Reset();
			Record->PendingDependencies.clear();
			Record->PendingDependencyWriteTimes.clear();
			Record->State = PendingReplacement ? AssetLoadState::Ready : AssetLoadState::Failed;
			Record->Error = Result.Error.empty() ? "GPU realization failed without a diagnostic" : std::move(Result.Error);
		}
		Record->LoadException = nullptr;
		PublicationLock.unlock();
		Record->PublicationChanged.notify_all();
		return Result.Succeeded;
	}
	catch (...)
	{
		std::scoped_lock ManagerLock(this->Mutex);
		std::unique_lock PublicationLock(Record->PublicationMutex);
		const bool CandidateMatches =
			PendingReplacement ? Record->PendingGPUAsset.Get() == Asset.Get() : Record->Asset.Get() == Asset.Get();
		if (Record->PublishedGeneration.load(std::memory_order_acquire) == Generation && CandidateMatches &&
			Record->State == AssetLoadState::RealizingGPU)
		{
			Record->PendingGPUAsset.Reset();
			Record->PendingDependencies.clear();
			Record->PendingDependencyWriteTimes.clear();
			Record->State = PendingReplacement ? AssetLoadState::Ready : AssetLoadState::Failed;
			Record->Error = "GPU realization threw an exception";
			Record->LoadException = std::current_exception();
			PublicationLock.unlock();
			Record->PublicationChanged.notify_all();
		}
		throw;
	}
}

bool AssetManager::RealizeGPU(pipeline::device::Device &Device, AssetType Type, const std::filesystem::path &Path)
{
	return this->RealizeGPU(Device, this->ResolveAssetID(Type, this->ResolvePath(Path)));
}

GPURealizationBatchResult AssetManager::ProcessPendingGPU(pipeline::device::Device &Device, const GPURealizationBudget Budget)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	if (!Budget.IsValid())
		throw std::invalid_argument("GPU realization budget must provide a non-zero asset and wall-time limit");
	GPURealizationBatchResult Batch;
	const auto Deadline = std::chrono::steady_clock::now() + Budget.MaximumWallTime;
	while (Batch.Attempted < Budget.MaximumAssets && std::chrono::steady_clock::now() < Deadline)
	{
		AssetID ID;
		{
			std::scoped_lock Lock(this->Mutex);
			if (this->PendingGPUAssetIDs.empty())
				break;
			ID = std::move(this->PendingGPUAssetIDs.front());
			this->PendingGPUAssetIDs.pop_front();
			const auto RecordIterator = this->Records.find(ID);
			if (RecordIterator != this->Records.end())
			{
				std::unique_lock PublicationLock(RecordIterator->second->PublicationMutex);
				RecordIterator->second->GPURealizationQueued = false;
			}
		}
		++Batch.Attempted;
		try
		{
			if (this->RealizeGPU(Device, ID))
			{
				++Batch.Realized;
				continue;
			}
		}
		catch (...)
		{
			++Batch.Failed;
			continue;
		}

		std::scoped_lock Lock(this->Mutex);
		const auto RecordIterator = this->Records.find(ID);
		if (RecordIterator == this->Records.end())
			continue;
		std::unique_lock PublicationLock(RecordIterator->second->PublicationMutex);
		const bool StillPending = (RecordIterator->second->State == AssetLoadState::CPUReady && RecordIterator->second->Asset != nullptr) ||
								  RecordIterator->second->PendingGPUAsset != nullptr;
		if (StillPending)
			this->QueueGPURealization(*RecordIterator->second);
	}
	{
		std::scoped_lock Lock(this->Mutex);
		Batch.Remaining = this->PendingGPUAssetIDs.size();
	}
	return Batch;
}

void AssetManager::RealizeAllPendingGPU(pipeline::device::Device &Device)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	for (;;)
	{
		AssetID ID;
		{
			std::scoped_lock Lock(this->Mutex);
			if (this->PendingGPUAssetIDs.empty())
				break;
			ID = std::move(this->PendingGPUAssetIDs.front());
			this->PendingGPUAssetIDs.pop_front();
			const auto RecordIterator = this->Records.find(ID);
			if (RecordIterator != this->Records.end())
			{
				std::unique_lock PublicationLock(RecordIterator->second->PublicationMutex);
				RecordIterator->second->GPURealizationQueued = false;
			}
		}

		if (this->RealizeGPU(Device, ID))
			continue;

		std::scoped_lock Lock(this->Mutex);
		const auto RecordIterator = this->Records.find(ID);
		if (RecordIterator == this->Records.end())
			continue;
		std::unique_lock PublicationLock(RecordIterator->second->PublicationMutex);
		const bool StillPending = (RecordIterator->second->State == AssetLoadState::CPUReady && RecordIterator->second->Asset != nullptr) ||
								  RecordIterator->second->PendingGPUAsset != nullptr;
		if (StillPending)
			this->QueueGPURealization(*RecordIterator->second);
	}
}

usize AssetManager::ReloadChangedAssets()
{
	AssetLoadSharedAccess TransactionAccess(*this);
	std::unordered_set<AssetID> DirtyIDs;
	std::vector<std::pair<AssetType, std::filesystem::path>> ChangedAssets;
	{
		std::scoped_lock Lock(this->Mutex);
		for (const auto &[id, record] : this->Records)
		{
			std::shared_lock PublicationLock(record->PublicationMutex);
			const AssetLoadState State = record->State;
			if (State == AssetLoadState::LoadingCPU || State == AssetLoadState::RealizingGPU)
			{
				continue;
			}

			std::error_code Error;
			const std::filesystem::file_time_type CurrentWriteTime = std::filesystem::last_write_time(record->CanonicalPath, Error);
			if (!Error && CurrentWriteTime != record->SourceWriteTime)
			{
				DirtyIDs.insert(id);
				continue;
			}

			for (const AssetID &Dependency : record->Dependencies)
			{
				const auto Path = this->DependencyPaths.find(Dependency);
				if (Path == this->DependencyPaths.end())
					continue;
				std::error_code DependencyError;
				const std::filesystem::file_time_type CurrentDependencyWriteTime =
					std::filesystem::last_write_time(Path->second, DependencyError);
				const auto PreviousDependencyWriteTime = record->DependencyWriteTimes.find(Dependency);
				if ((DependencyError && PreviousDependencyWriteTime != record->DependencyWriteTimes.end()) ||
					(!DependencyError && (PreviousDependencyWriteTime == record->DependencyWriteTimes.end() ||
										  CurrentDependencyWriteTime != PreviousDependencyWriteTime->second)))
				{
					DirtyIDs.insert(id);
					break;
				}
			}
		}

		std::vector<AssetID> Pending(DirtyIDs.begin(), DirtyIDs.end());
		for (usize Index = 0; Index < Pending.size(); ++Index)
		{
			const auto Dependents = this->ReverseDependencies.find(Pending[Index]);
			if (Dependents == this->ReverseDependencies.end())
				continue;
			for (const AssetID &Dependent : Dependents->second)
			{
				if (DirtyIDs.insert(Dependent).second)
					Pending.push_back(Dependent);
			}
		}

		for (const AssetID &DirtyID : DirtyIDs)
		{
			const auto RecordIterator = this->Records.find(DirtyID);
			if (RecordIterator == this->Records.end())
				continue;
			AssetRecord *const Record = RecordIterator->second;
			std::shared_lock PublicationLock(Record->PublicationMutex);
			if (Record->IsImportRoot && Record->State != AssetLoadState::LoadingCPU && Record->State != AssetLoadState::RealizingGPU)
				ChangedAssets.emplace_back(Record->Type, Record->CanonicalPath);
		}
	}
	std::sort(ChangedAssets.begin(), ChangedAssets.end(), [](const auto &Left, const auto &Right)
			  { return AssetManager::MakeAssetID(Left.first, Left.second) < AssetManager::MakeAssetID(Right.first, Right.second); });

	for (const auto &[type, path] : ChangedAssets)
	{
		try
		{
			(void)this->LoadRecord(type, path, true);
		}
		catch (const importer::AssetImportException &)
		{
			// The failed record already contains the diagnostic; continue reloading other assets.
		}
	}
	return ChangedAssets.size();
}

AssetRecordHandle AssetManager::GetRecord(AssetType Type, const std::filesystem::path &Path) const
{
	AssetLoadSharedAccess TransactionAccess(*this);
	const AssetID ID = this->ResolveAssetID(Type, this->ResolvePath(Path));
	std::scoped_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end() || !Iterator->second->TryRetainStrong())
		return {};
	return AssetRecordHandle(Iterator->second, AssetRecordHandle::AdoptStrongReference{});
}

void AssetManager::PublishAssetRegistry(const std::span<const AssetPublication> Assets)
{
	AssetLoadSharedAccess TransactionAccess(*this);
	std::unordered_map<AssetID, AssetPublication> ByID;
	std::unordered_map<AssetID, AssetID> ByPath;
	std::unordered_map<string, AssetID> ByVirtualPath;
	ByID.reserve(Assets.size());
	ByPath.reserve(Assets.size());
	ByVirtualPath.reserve(Assets.size());
	for (const AssetPublication &Publication : Assets)
	{
		if (Publication.ID.empty() || Publication.Type == AssetType::Count || Publication.VirtualPath.empty() ||
			!Publication.VirtualPath.starts_with('/'))
			throw std::invalid_argument("Published assets require an ID, absolute virtual path, and concrete type");
		AssetPublication Canonical = Publication;
		Canonical.CanonicalPath = this->ResolvePath(Publication.CanonicalPath);
		const AssetID PathKey = MakeAssetID(Canonical.Type, Canonical.CanonicalPath);
		if (!ByID.emplace(Canonical.ID, Canonical).second)
			throw std::invalid_argument("Published asset registry contains a duplicate ID: " + Canonical.ID);
		if (!ByPath.emplace(PathKey, Canonical.ID).second)
			throw std::invalid_argument("Published asset registry contains a duplicate typed path: " + Canonical.CanonicalPath.string());
		if (!ByVirtualPath.emplace(Canonical.VirtualPath, Canonical.ID).second)
			throw std::invalid_argument("Published asset registry contains a duplicate virtual path: " + Canonical.VirtualPath);
	}

	std::scoped_lock Lock(this->Mutex);
	for (const auto &[ID, Publication] : ByID)
	{
		const auto Existing = this->Records.find(ID);
		if (Existing == this->Records.end())
			continue;
		Existing->second->CanonicalPath = Publication.CanonicalPath;
	}
	this->PublishedAssets = std::move(ByID);
	this->PublishedIDsByPathKey = std::move(ByPath);
	this->PublishedIDsByVirtualPath = std::move(ByVirtualPath);
}

AssetHandle<Asset> AssetManager::ReloadAssetByID(const AssetID &ID)
{
	AssetPublication Publication;
	{
		std::scoped_lock Lock(this->Mutex);
		const auto Published = this->PublishedAssets.find(ID);
		if (Published != this->PublishedAssets.end())
			Publication = Published->second;
		else
		{
			const auto Generated = this->GeneratedAssets.find(ID);
			if (Generated == this->GeneratedAssets.end())
				throw AssetUnavailableException(ID, AssetType::Count, "asset ID is not registered");
			Publication = Generated->second;
		}
	}
	AssetRecordHandle Record = this->LoadRecord(Publication.Type, Publication.CanonicalPath, true);
	(void)Record->Pin<Asset>();
	return AssetHandle<Asset>(Record.Record);
}

std::optional<AssetPublication> AssetManager::ResolveVirtualPath(const string_view VirtualPath) const
{
	AssetLoadSharedAccess TransactionAccess(*this);
	std::scoped_lock Lock(this->Mutex);
	const auto ID = this->PublishedIDsByVirtualPath.find(string(VirtualPath));
	if (ID == this->PublishedIDsByVirtualPath.end())
		return std::nullopt;
	const auto Publication = this->PublishedAssets.find(ID->second);
	return Publication == this->PublishedAssets.end() ? std::nullopt : std::optional<AssetPublication>(Publication->second);
}

AssetRecordHandle AssetManager::GetRecord(const AssetID &ID) const
{
	AssetLoadSharedAccess TransactionAccess(*this);
	std::scoped_lock Lock(this->Mutex);
	const auto Iterator = this->Records.find(ID);
	if (Iterator == this->Records.end() || !Iterator->second->TryRetainStrong())
		return {};
	return AssetRecordHandle(Iterator->second, AssetRecordHandle::AdoptStrongReference{});
}

std::vector<AssetRecordSnapshot> AssetManager::SnapshotRecords() const
{
	AssetLoadSharedAccess TransactionAccess(*this);
	std::vector<AssetRecordSnapshot> Result;
	{
		std::scoped_lock Lock(this->Mutex);
		Result.reserve(this->Records.size());
		for (const auto &[id, record] : this->Records)
		{
			std::shared_lock PublicationLock(record->PublicationMutex);
			std::vector<AssetID> Reverse;
			if (const auto Dependents = this->ReverseDependencies.find(id); Dependents != this->ReverseDependencies.end())
				Reverse.assign(Dependents->second.begin(), Dependents->second.end());
			std::ranges::sort(Reverse);
			Result.push_back({.ID = id,
							  .CanonicalPath = record->CanonicalPath,
							  .Type = record->Type,
							  .State = record->State,
							  .Error = record->Error,
							  .Dependencies = record->Dependencies,
							  .ReverseDependencies = std::move(Reverse),
							  .PublishedGeneration = record->PublishedGeneration.load(std::memory_order_acquire),
							  .StrongReferences = record->StrongReferences.load(std::memory_order_acquire)});
		}
	}
	std::sort(Result.begin(), Result.end(),
			  [](const AssetRecordSnapshot &Left, const AssetRecordSnapshot &Right)
			  {
				  if (Left.CanonicalPath != Right.CanonicalPath)
					  return Left.CanonicalPath.generic_string() < Right.CanonicalPath.generic_string();
				  return static_cast<uint32>(Left.Type) < static_cast<uint32>(Right.Type);
			  });
	return Result;
}

std::optional<AssetRecordSnapshot> AssetManager::SnapshotRecord(const AssetID &ID) const
{
	std::vector<AssetRecordSnapshot> Records = this->SnapshotRecords();
	const auto Record = std::ranges::find(Records, ID, &AssetRecordSnapshot::ID);
	return Record == Records.end() ? std::nullopt : std::optional<AssetRecordSnapshot>(std::move(*Record));
}

std::filesystem::path AssetManager::CanonicalizePath(const std::filesystem::path &Path)
{
	std::error_code Error;
	const std::filesystem::path AbsolutePath = std::filesystem::absolute(Path, Error);
	if (Error)
	{
		return Path.lexically_normal();
	}

	const std::filesystem::path CanonicalPath = std::filesystem::weakly_canonical(AbsolutePath, Error);
	return Error ? AbsolutePath.lexically_normal() : CanonicalPath;
}

const std::filesystem::path &AssetManager::GetRootPath() const noexcept
{
	return this->RootPath;
}

std::filesystem::path AssetManager::ResolvePath(const std::filesystem::path &Path) const
{
	if (Path.empty())
		throw std::invalid_argument("Asset path cannot be empty");
	if (Path.is_absolute())
	{
		core::io::SecurePath::VerifyContained(this->RootPath, Path, "Asset path");
		return std::filesystem::absolute(Path).lexically_normal();
	}
	return core::io::SecurePath::ResolveWithin(this->RootPath, Path, "Asset path");
}

AssetID AssetManager::MakeAssetID(AssetType Type, const std::filesystem::path &CanonicalPath)
{
	return std::to_string(static_cast<uint32>(Type)) + ":" + CanonicalPath.generic_string();
}

AssetID AssetManager::ResolveAssetID(const AssetType Type, const std::filesystem::path &CanonicalPath) const
{
	const AssetID PathKey = MakeAssetID(Type, CanonicalPath);
	std::scoped_lock Lock(this->Mutex);
	const auto Published = this->PublishedIDsByPathKey.find(PathKey);
	if (Published != this->PublishedIDsByPathKey.end())
		return Published->second;
	const auto Generated = this->GeneratedIDsByPathKey.find(PathKey);
	return Generated == this->GeneratedIDsByPathKey.end() ? PathKey : Generated->second;
}
} // namespace resource
