#include "AssetManager.h"

#include <algorithm>

#include "src/resource/asset/importer/ModelAssetImporter.h"
#include "src/resource/asset/importer/ShaderSourceImporter.h"
#include "src/resource/asset/importer/Texture2DImporter.h"

namespace resource
{
	AssetManager::AssetManager()
	{
		this->addAssetImporter<importer::Texture2DImporter>();
		this->addAssetImporter<importer::ModelAssetImporter>();
		this->addAssetImporter<importer::ShaderSourceImporter>();
	}

	AssetRecord* AssetManager::loadRecord(AssetType type, const std::filesystem::path& path, bool forceReload)
	{
		const std::filesystem::path canonicalPath = canonicalizePath(path);
		const AssetID id = makeAssetID(type, canonicalPath);
		AssetRecord* record = nullptr;
		importer::AssetImporter* importer = nullptr;

		{
			std::scoped_lock lock(this->mutex);
			auto existing = this->records.find(id);
			if (existing != this->records.end() && !forceReload && existing->second->state != AssetLoadState::Failed)
			{
				return existing->second.get();
			}

			if (existing == this->records.end())
			{
				auto newRecord = std::make_unique<AssetRecord>();
				newRecord->id = id;
				newRecord->canonicalPath = canonicalPath;
				newRecord->type = type;
				record = newRecord.get();
				this->records.emplace(id, std::move(newRecord));
			}
			else
			{
				record = existing->second.get();
			}

			record->state = AssetLoadState::LoadingCpu;
			record->error.clear();

			const size_t importerIndex = static_cast<size_t>(type);
			if (importerIndex < this->assetImporters.size())
			{
				importer = this->assetImporters[importerIndex].get();
			}
		}

		const auto recordFailure = [this, record](const std::string& error)
		{
			std::scoped_lock lock(this->mutex);
			record->state = AssetLoadState::Failed;
			record->error = error;
		};

		if (importer == nullptr)
		{
			importer::AssetImporterNotRegisteredException exception(type, canonicalPath);
			recordFailure(exception.what());
			throw exception;
		}

		try
		{
			importer::AssetImportResult result = importer->importCpu(canonicalPath);
			std::vector<AssetID> dependencyIDs;
			dependencyIDs.reserve(result.dependencies.size());
			for (const AssetDependency& dependency : result.dependencies)
			{
				dependencyIDs.push_back(makeAssetID(dependency.type, canonicalizePath(dependency.path)));
			}

			std::error_code writeTimeError;
			const std::filesystem::file_time_type sourceWriteTime = std::filesystem::last_write_time(canonicalPath, writeTimeError);
			if (writeTimeError)
			{
				throw importer::AssetFileReadException(type, canonicalPath, "Unable to read source write time: " + writeTimeError.message());
			}

			std::scoped_lock lock(this->mutex);
			record->asset = std::move(result.asset);
			record->dependencies = std::move(dependencyIDs);
			record->sourceWriteTime = sourceWriteTime;
			record->state = record->asset->requiresGpuRealization() ? AssetLoadState::CpuReady : AssetLoadState::Ready;
			record->error.clear();
			return record;
		}
		catch (const importer::AssetImportException& exception)
		{
			recordFailure(exception.what());
			throw;
		}
		catch (const std::exception& exception)
		{
			importer::AssetUnexpectedImportException wrappedException(type, canonicalPath, exception.what());
			recordFailure(wrappedException.what());
			throw wrappedException;
		}
		catch (...)
		{
			importer::AssetUnexpectedImportException wrappedException(type, canonicalPath, "Unknown non-standard exception");
			recordFailure(wrappedException.what());
			throw wrappedException;
		}
	}

	bool AssetManager::realizeGpu(const AssetID& id)
	{
		AssetRecord* record = nullptr;
		AssetPtr<Asset> asset;
		{
			std::scoped_lock lock(this->mutex);
			auto iterator = this->records.find(id);
			if (iterator == this->records.end())
			{
				return false;
			}

			record = iterator->second.get();
			if (record->state == AssetLoadState::Ready)
			{
				return true;
			}
			if (record->state != AssetLoadState::CpuReady || record->asset == nullptr)
			{
				return false;
			}

			record->state = AssetLoadState::RealizingGpu;
			asset = record->asset;
		}

		AssetGpuRealizationResult result = asset->realizeGpu();
		std::scoped_lock lock(this->mutex);
		record->state = result.succeeded ? AssetLoadState::Ready : AssetLoadState::Failed;
		record->error = result.succeeded ? "" : std::move(result.error);
		return result.succeeded;
	}

	bool AssetManager::realizeGpu(AssetType type, const std::filesystem::path& path)
	{
		return this->realizeGpu(makeAssetID(type, canonicalizePath(path)));
	}

	void AssetManager::realizeAllPendingGpu()
	{
		std::vector<AssetID> pendingIDs;
		{
			std::scoped_lock lock(this->mutex);
			for (const auto& [id, record] : this->records)
			{
				if (record->state == AssetLoadState::CpuReady)
				{
					pendingIDs.push_back(id);
				}
			}
		}

		for (const AssetID& id : pendingIDs)
		{
			(void)this->realizeGpu(id);
		}
	}

	size_t AssetManager::reloadChangedAssets()
	{
		std::vector<std::pair<AssetType, std::filesystem::path>> changedAssets;
		{
			std::scoped_lock lock(this->mutex);
			for (const auto& [id, record] : this->records)
			{
				if (record->state == AssetLoadState::LoadingCpu || record->state == AssetLoadState::RealizingGpu)
				{
					continue;
				}

				std::error_code error;
				const std::filesystem::file_time_type currentWriteTime = std::filesystem::last_write_time(record->canonicalPath, error);
				if (!error && currentWriteTime != record->sourceWriteTime)
				{
					changedAssets.emplace_back(record->type, record->canonicalPath);
				}
			}
		}

		for (const auto& [type, path] : changedAssets)
		{
			try
			{
				(void)this->loadRecord(type, path, true);
			}
			catch (const importer::AssetImportException&)
			{
				// The failed record already contains the diagnostic; continue reloading other assets.
			}
		}
		return changedAssets.size();
	}

	const AssetRecord* AssetManager::getRecord(AssetType type, const std::filesystem::path& path) const
	{
		const AssetID id = makeAssetID(type, canonicalizePath(path));
		std::scoped_lock lock(this->mutex);
		auto iterator = this->records.find(id);
		return iterator == this->records.end() ? nullptr : iterator->second.get();
	}

	std::filesystem::path AssetManager::canonicalizePath(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
		if (error)
		{
			return path.lexically_normal();
		}

		const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absolutePath, error);
		return error ? absolutePath.lexically_normal() : canonicalPath;
	}

	AssetID AssetManager::makeAssetID(AssetType type, const std::filesystem::path& canonicalPath)
	{
		return std::to_string(static_cast<uint32>(type)) + ":" + canonicalPath.generic_string();
	}
}
