#include "AssetManager.h"

#include "src/resource/asset/importer/ModelAssetImporter.h"
#include "src/resource/asset/importer/ShaderSourceImporter.h"
#include "src/resource/asset/importer/Texture2DImporter.h"

#include <algorithm>
#include <unordered_set>

namespace resource
{
AssetManager::AssetManager()
{
	this->AddAssetImporter<importer::Texture2DImporter>();
	this->AddAssetImporter<importer::ModelAssetImporter>();
	this->AddAssetImporter<importer::ShaderSourceImporter>();
}

AssetManager::~AssetManager()
{
	std::scoped_lock Lock(this->Mutex);
	for (auto &[id, record] : this->Records)
	{
		(void)id;
		record->ReleaseStrong();
	}
	this->Records.clear();
}

AssetRecord *AssetManager::LoadRecord(AssetType Type, const std::filesystem::path &Path, bool ForceReload)
{
	const std::filesystem::path CanonicalPath = CanonicalizePath(Path);
	const AssetID ID = MakeAssetID(Type, CanonicalPath);
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

		const usize ImporterIndex = static_cast<usize>(Type);
		if (ImporterIndex < this->AssetImporters.size())
		{
			Importer = this->AssetImporters[ImporterIndex];
		}
	}

	uint64 LoadOperation = 0;
	{
		std::unique_lock PublicationLock(Record->PublicationMutex);
		bool WaitedForLoad = false;
		while (Record->State == AssetLoadState::LoadingCPU)
		{
			WaitedForLoad = true;
			Record->PublicationChanged.wait(PublicationLock);
		}

		if (!ForceReload && (Record->State == AssetLoadState::CPUReady || Record->State == AssetLoadState::RealizingGPU ||
							 Record->State == AssetLoadState::Ready))
		{
			return Record;
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
		Record->State = AssetLoadState::LoadingCPU;
		Record->Error.clear();
		Record->LoadException = nullptr;
		Record->IsImportRoot = true;
	}

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
		importer::AssetImportContext ImportContext([this](AssetType ProductType, const std::filesystem::path &ProductPath)
												   { return this->ReserveRecord(ProductType, ProductPath); });
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
			const std::filesystem::path DependencyPath = CanonicalizePath(Dependency.Path);
			RootDependencies.push_back({MakeAssetID(Dependency.Type, DependencyPath), DependencyPath});
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
			const std::filesystem::path ProductPath = CanonicalizePath(Product.CanonicalPath);
			const AssetID ProductID = MakeAssetID(Product.Type, ProductPath);
			if (!ProductIDs.insert(ProductID).second)
			{
				throw importer::AssetContentValidationException(Type, CanonicalPath,
																"Importer produced duplicate subasset identity " + ProductID);
			}
			AssetRecord *ProductRecord = this->ReserveRecord(Product.Type, ProductPath);
			PendingProducts.push_back({ProductRecord, std::move(Product.Asset), std::move(Product.Dependencies)});
		}
		for (PendingProduct &Product : PendingProducts)
		{
			std::vector<PreparedDependency> ProductDependencies;
			ProductDependencies.reserve(Product.Dependencies.size());
			for (const AssetDependency &Dependency : Product.Dependencies)
			{
				const std::filesystem::path DependencyPath = CanonicalizePath(Dependency.Path);
				const AssetID DependencyID = MakeAssetID(Dependency.Type, DependencyPath);
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
			return Record;
		for (PreparedProduct &Product : Prepared)
		{
			for (const AssetID &PreviousDependency : Product.Record->Dependencies)
			{
				auto Reverse = this->ReverseDependencies.find(PreviousDependency);
				if (Reverse != this->ReverseDependencies.end())
				{
					Reverse->second.erase(Product.Record->ID);
					if (Reverse->second.empty())
						this->ReverseDependencies.erase(Reverse);
				}
			}

			Product.Record->Dependencies.clear();
			Product.Record->DependencyWriteTimes.clear();
			Product.Record->Dependencies.reserve(Product.Dependencies.size());
			for (const PreparedDependency &Dependency : Product.Dependencies)
			{
				Product.Record->Dependencies.push_back(Dependency.ID);
				this->ReverseDependencies[Dependency.ID].insert(Product.Record->ID);
				this->DependencyPaths.insert_or_assign(Dependency.ID, Dependency.Path);
				std::error_code DependencyTimeError;
				const std::filesystem::file_time_type DependencyWriteTime =
					std::filesystem::last_write_time(Dependency.Path, DependencyTimeError);
				if (!DependencyTimeError)
					Product.Record->DependencyWriteTimes.emplace(Dependency.ID, DependencyWriteTime);
			}
			Product.Record->Asset = std::move(Product.Asset);
			Product.Record->SourceWriteTime = SourceWriteTime;
			Product.Record->State = Product.Record->Asset->RequiresGPURealization() ? AssetLoadState::CPUReady : AssetLoadState::Ready;
			Product.Record->Error.clear();
			Product.Record->LoadException = nullptr;
			Product.Record->PublishedGeneration.fetch_add(1, std::memory_order_release);
		}
		for (PreparedProduct &Product : Prepared)
			Product.Record->PublicationChanged.notify_all();
		return Record;
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

AssetRecord *AssetManager::ReserveRecord(AssetType Type, const std::filesystem::path &Path)
{
	const std::filesystem::path CanonicalPath = CanonicalizePath(Path);
	const AssetID ID = MakeAssetID(Type, CanonicalPath);
	std::scoped_lock Lock(this->Mutex);
	const auto Existing = this->Records.find(ID);
	if (Existing != this->Records.end())
	{
		if (Existing->second->Type != Type)
		{
			throw std::logic_error("Asset record identity was reserved with a conflicting type");
		}
		return Existing->second;
	}
	AssetRecord *Record = new AssetRecord(ID, CanonicalPath, Type);
	this->Records.emplace(ID, Record);
	return Record;
}

bool AssetManager::RealizeGPU(pipeline::device::Device &Device, const AssetID &ID)
{
	AssetRecord *Record = nullptr;
	AssetPtr<resource::Asset> Asset;
	uint64 Generation = 0;
	{
		std::scoped_lock Lock(this->Mutex);
		auto Iterator = this->Records.find(ID);
		if (Iterator == this->Records.end())
		{
			return false;
		}

		Record = Iterator->second;
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->State == AssetLoadState::Ready)
		{
			return true;
		}
		if (Record->State != AssetLoadState::CPUReady || Record->Asset == nullptr)
		{
			return false;
		}

		Record->State = AssetLoadState::RealizingGPU;
		Asset = Record->Asset;
		Generation = Record->PublishedGeneration.load(std::memory_order_acquire);
	}

	try
	{
		AssetGPURealizationResult Result = Asset->RealizeGPU(Device);
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->PublishedGeneration.load(std::memory_order_acquire) != Generation || Record->Asset.Get() != Asset.Get() ||
			Record->State != AssetLoadState::RealizingGPU)
		{
			return false;
		}
		Record->State = Result.Succeeded ? AssetLoadState::Ready : AssetLoadState::Failed;
		Record->Error = Result.Succeeded ? "" : std::move(Result.Error);
		Record->LoadException = nullptr;
		PublicationLock.unlock();
		Record->PublicationChanged.notify_all();
		return Result.Succeeded;
	}
	catch (...)
	{
		std::unique_lock PublicationLock(Record->PublicationMutex);
		if (Record->PublishedGeneration.load(std::memory_order_acquire) == Generation && Record->Asset.Get() == Asset.Get() &&
			Record->State == AssetLoadState::RealizingGPU)
		{
			Record->State = AssetLoadState::Failed;
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
	return this->RealizeGPU(Device, MakeAssetID(Type, CanonicalizePath(Path)));
}

void AssetManager::RealizeAllPendingGPU(pipeline::device::Device &Device)
{
	std::vector<AssetID> PendingIDs;
	{
		std::scoped_lock Lock(this->Mutex);
		for (const auto &[id, record] : this->Records)
		{
			if (record->GetState() == AssetLoadState::CPUReady)
			{
				PendingIDs.push_back(id);
			}
		}
	}

	for (const AssetID &ID : PendingIDs)
	{
		(void)this->RealizeGPU(Device, ID);
	}
}

usize AssetManager::ReloadChangedAssets()
{
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

const AssetRecord *AssetManager::GetRecord(AssetType Type, const std::filesystem::path &Path) const
{
	const AssetID ID = MakeAssetID(Type, CanonicalizePath(Path));
	std::scoped_lock Lock(this->Mutex);
	auto Iterator = this->Records.find(ID);
	return Iterator == this->Records.end() ? nullptr : Iterator->second;
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

AssetID AssetManager::MakeAssetID(AssetType Type, const std::filesystem::path &CanonicalPath)
{
	return std::to_string(static_cast<uint32>(Type)) + ":" + CanonicalPath.generic_string();
}
} // namespace resource
