#include "AssetReloadService.h"

#include "AssetMetadata.h"
#include "Source/core/io/SecurePath.h"

#include <chrono>
#include <string>
#include <stdexcept>
#include <utility>

namespace editor::asset
{
AssetReloadService::AssetReloadService(resource::AssetManager &Assets, std::filesystem::path ContentRoot)
	: Assets(&Assets), ContentRoot(std::filesystem::absolute(std::move(ContentRoot)).lexically_normal())
{
}

AssetReloadService::~AssetReloadService()
{
	this->Wait();
}

void AssetReloadService::Begin(core::threading::TaskScheduler &Scheduler, resource::AssetID ID, std::filesystem::path MetadataPath)
{
	if (this->Pending.valid())
		throw std::logic_error("An asset reload is already in progress");
	if (ID.empty())
		throw std::invalid_argument("Asset reload requires a stable asset ID");
	if (MetadataPath.empty())
		throw std::invalid_argument("Asset reload requires an asset metadata sidecar");
	const std::filesystem::path AbsoluteMetadata =
		core::io::SecurePath::ResolveWithin(this->ContentRoot, std::move(MetadataPath), "Asset reload metadata");
	resource::AssetManager *Assets = this->Assets;
	this->Result.reset();
	this->Pending = Scheduler.Submit(
		[Assets, ID = std::move(ID), AbsoluteMetadata]() mutable
		{
			try
			{
				(void)Assets->ReloadAssetByID(ID);
				const std::optional<resource::AssetRecordSnapshot> Snapshot = Assets->SnapshotRecord(ID);
				if (!Snapshot.has_value())
					throw std::logic_error("Reloaded asset did not publish an immutable record snapshot");
				string MetadataDiagnostic;
				std::optional<AssetMetadata> Metadata = AssetMetadataStore::TryLoad(AbsoluteMetadata, MetadataDiagnostic);
				if (!Metadata.has_value())
					throw std::runtime_error("Could not load asset metadata after reload: " + MetadataDiagnostic);
				Metadata->Dependencies = Snapshot->Dependencies;
				AssetMetadataStore::Save(*Metadata, AbsoluteMetadata);
				return AssetReloadResult{.ID = std::move(ID), .Succeeded = true, .Diagnostic = "Asset reloaded successfully"};
			}
			catch (const std::exception &Exception)
			{
				return AssetReloadResult{.ID = std::move(ID), .Succeeded = false, .Diagnostic = Exception.what()};
			}
			catch (...)
			{
				return AssetReloadResult{
					.ID = std::move(ID), .Succeeded = false, .Diagnostic = "Asset reload failed with a non-standard exception"};
			}
		});
}

void AssetReloadService::BeginChanged(core::threading::TaskScheduler &Scheduler)
{
	if (this->Pending.valid())
		throw std::logic_error("An asset reload is already in progress");
	resource::AssetManager *Assets = this->Assets;
	this->Result.reset();
	this->Pending = Scheduler.Submit(
		[Assets]()
		{
			try
			{
				const usize Reloaded = Assets->ReloadChangedAssets();
				return AssetReloadResult{.Succeeded = true, .Diagnostic = "Reloaded " + std::to_string(Reloaded) + " changed asset(s)"};
			}
			catch (const std::exception &Exception)
			{
				return AssetReloadResult{.Succeeded = false, .Diagnostic = Exception.what()};
			}
			catch (...)
			{
				return AssetReloadResult{.Succeeded = false, .Diagnostic = "Changed-asset reload failed with a non-standard exception"};
			}
		});
}

bool AssetReloadService::Poll()
{
	if (!this->Pending.valid() || this->Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return false;
	this->Result = this->Pending.get();
	return true;
}

void AssetReloadService::Wait() noexcept
{
	if (!this->Pending.valid())
		return;
	try
	{
		this->Pending.wait();
		this->Result = this->Pending.get();
	}
	catch (...)
	{
	}
}

bool AssetReloadService::IsBusy() const noexcept
{
	return this->Pending.valid();
}

const std::optional<AssetReloadResult> &AssetReloadService::GetResult() const noexcept
{
	return this->Result;
}
} // namespace editor::asset
