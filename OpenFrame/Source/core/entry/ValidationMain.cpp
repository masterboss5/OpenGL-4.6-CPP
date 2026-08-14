#include "Source/core/app/Application.h"
#include "Source/core/layers/EditorLayer.h"
#include "Source/core/layers/GameLayer.h"
#include "Source/core/threading/RenderThread.h"
#include "Source/core/window/Context.h"
#include "Source/core/window/WindowManager.h"
#include "Source/component/object/CObjectCameraComponent.h"
#include "Source/component/object/CObjectLightComponents.h"
#include "Source/component/object/CObjectMeshComponent.h"
#include "Source/component/object/CObjectTransformComponent.h"
#include "Source/editor/document/SceneDocument.h"
#include "Source/editor/reflection/ComponentReflection.h"
#include "Source/editor/reflection/ReflectionRegistry.h"
#include "Source/editor/serialization/ProjectDescriptorSerializer.h"
#include "Source/editor/serialization/SceneDocumentSerializer.h"
#include "Source/editor/validation/EditorCoreValidation.h"
#include "Source/pipeline/validation/RenderCoreValidation.h"
#include "Source/resource/asset/AssetManager.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#ifdef CreateWindow
#undef CreateWindow
#endif

namespace
{
class StopValidationLayer final : public ApplicationLayer
{
  public:
	StopValidationLayer(core::Window *, pipeline::device::Device &, std::atomic<uint32> &RunCount) : RunCount(RunCount)
	{
	}

	void Run(const core::ApplicationFrame &Frame) override
	{
		this->RunCount.fetch_add(1, std::memory_order_relaxed);
		Frame.Services.RequestStop();
	}

  private:
	std::atomic<uint32> &RunCount;
};

class StopSentinelLayer final : public ApplicationLayer
{
  public:
	StopSentinelLayer(core::Window *, pipeline::device::Device &, std::atomic<uint32> &RunCount) : RunCount(RunCount)
	{
	}

	void Run(const core::ApplicationFrame &) override
	{
		this->RunCount.fetch_add(1, std::memory_order_relaxed);
	}

  private:
	std::atomic<uint32> &RunCount;
};

void ValidateDetachedWindowLifecycle(core::WindowManager &Manager)
{
	core::Window &PrimaryWindow = Manager.GetPrimaryWindow();
	const core::MonitorInfo PrimaryMonitor = Manager.GetPrimaryMonitor();
	if (!std::isfinite(PrimaryMonitor.ContentScaleX) || !std::isfinite(PrimaryMonitor.ContentScaleY) ||
		PrimaryMonitor.ContentScaleX <= 0.0f || PrimaryMonitor.ContentScaleY <= 0.0f)
	{
		throw std::runtime_error("Detached-window validation received invalid monitor DPI scaling");
	}

	core::WindowSpecification Specification;
	Specification.Title = "Detached Window Lifecycle Validation";
	Specification.Extent = {640, 480};
	Specification.ContextGroup = PrimaryWindow.GetContext().GetShareGroupName();
	Specification.Visible = false;
	Specification.Focused = false;
	Specification.HeadlessValidation = true;
	core::Window &DetachedWindow = Manager.CreateWindow(Specification);
	const core::WindowID DetachedWindowID = DetachedWindow.GetID();
	if (DetachedWindowID == PrimaryWindow.GetID() || Manager.FindManagedWindow(DetachedWindowID) != &DetachedWindow ||
		!std::isfinite(DetachedWindow.GetContentScale()) || DetachedWindow.GetContentScale() <= 0.0f)
	{
		throw std::runtime_error("Detached window was not registered with a valid independent identity and DPI scale");
	}
	std::atomic<bool> ForeignLookupRejected = false;
	std::thread ForeignLookup(
		[&Manager, DetachedWindowID, &ForeignLookupRejected]()
		{
			try
			{
				(void)Manager.FindManagedWindow(DetachedWindowID);
			}
			catch (const core::WindowException &)
			{
				ForeignLookupRejected.store(true, std::memory_order_release);
			}
		});
	ForeignLookup.join();
	if (!ForeignLookupRejected.load(std::memory_order_acquire))
		throw std::runtime_error("WindowManager allowed a borrowed Window pointer to escape onto a foreign thread");

	core::Context &PrimaryContext = PrimaryWindow.GetContext();
	core::Context &DetachedContext = DetachedWindow.GetContext();
	DetachedContext.PrepareThreadTransfer();
	core::threading::RenderThread RenderThread({.QueueCapacity = 16});
	RenderThread.Start(PrimaryContext);
	const bool Adopted = RenderThread
							 .Submit(
								 [&PrimaryContext, &DetachedContext, DetachedWindowID]()
								 {
									 DetachedContext.AdoptCurrentThread();
									 const bool Result = DetachedContext.IsCurrent() && DetachedContext.GetWindowID() == DetachedWindowID;
									 DetachedContext.PrepareThreadTransfer();
									 PrimaryContext.MakeCurrent();
									 return Result;
								 })
							 .get();
	RenderThread.Stop();
	if (!Adopted || !PrimaryContext.IsCurrent() || !DetachedContext.IsThreadTransferPending())
		throw std::runtime_error("Detached window context did not return from a completed render frame");

	// Destroying a platform window after its final frame must adopt the pending
	// transfer on the owner thread before unregistering its debug callback.
	Manager.DestroyWindow(DetachedWindowID);
	if (Manager.FindManagedWindow(DetachedWindowID) != nullptr)
		throw std::runtime_error("Detached window survived its owner-thread shutdown");

	core::Window &ResetWindow = Manager.CreateWindow(Specification);
	core::Context &ResetContext = ResetWindow.GetContext();
	const core::WindowID ResetWindowID = ResetWindow.GetID();
	ResetContext.MarkReset();
	bool ResetRejected = false;
	try
	{
		ResetContext.RequireCurrentThread();
	}
	catch (const core::ContextException &)
	{
		ResetRejected = true;
	}
	if (!ResetRejected)
		throw std::runtime_error("Reset detached context remained usable after a simulated device failure");

	Manager.DestroyWindow(ResetWindowID);
}

class TemporaryEditorProject final
{
  public:
	TemporaryEditorProject()
	{
		this->Root = std::filesystem::temp_directory_path() / ("EditorLayerValidation-" + util::UUID::GenerateRandomUUID().ToString());
		std::filesystem::create_directories(this->Root / "Content");
		this->DescriptorPath = this->Root / "Validation.engineproject";
		std::ofstream Descriptor(this->DescriptorPath, std::ios::binary | std::ios::trunc);
		if (!Descriptor)
			throw std::runtime_error("Could not create the editor-layer validation descriptor");
		Descriptor << "{}";
		if (!Descriptor)
			throw std::runtime_error("Could not write the editor-layer validation descriptor");
	}

	~TemporaryEditorProject()
	{
		std::error_code Error;
		std::filesystem::remove_all(this->Root, Error);
	}

	[[nodiscard]] const std::filesystem::path &GetDescriptorPath() const noexcept
	{
		return this->DescriptorPath;
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path DescriptorPath;
};

class TemporaryGameProject final
{
  public:
	TemporaryGameProject()
	{
		this->Root = std::filesystem::temp_directory_path() / ("GameLayerValidation-" + util::UUID::GenerateRandomUUID().ToString());
		const std::filesystem::path ContentRoot = this->Root / "Content";
		const std::filesystem::path ScenePath = ContentRoot / "Scenes" / "Startup.enginelevel";
		const std::filesystem::path RoundedScenePath = ContentRoot / "Scenes" / "Rounded.enginelevel";
		const std::filesystem::path ModelPath = ContentRoot / "Models" / "ValidationCube.obj";
		const std::filesystem::path CurvedCasterPath = ContentRoot / "Models" / "ValidationCurvedCaster.obj";
		const std::filesystem::path SpherePath = ContentRoot / "Models" / "ValidationSphere.obj";
		this->DescriptorPath = this->Root / "Validation.engineproject";
		std::filesystem::create_directories(ScenePath.parent_path());
		std::filesystem::create_directories(ModelPath.parent_path());
		std::ofstream Model(ModelPath, std::ios::binary | std::ios::trunc);
		if (!Model)
			throw std::runtime_error("Could not create the game-layer validation model");
		Model << "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
				 "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
				 "f 1 4 3 2\nf 5 6 7 8\nf 1 5 8 4\nf 2 3 7 6\nf 1 2 6 5\nf 4 8 7 3\n";
		Model.flush();
		if (!Model)
			throw std::runtime_error("Could not write the game-layer validation model");
		Model.close();
		std::ofstream CurvedCaster(CurvedCasterPath, std::ios::binary | std::ios::trunc);
		if (!CurvedCaster)
			throw std::runtime_error("Could not create the curved shadow validation model");
		constexpr uint32 CurvedCasterSegments = 64U;
		for (uint32 Segment = 0U; Segment < CurvedCasterSegments; ++Segment)
		{
			const float32 Angle =
				static_cast<float32>(Segment) * (2.0f * std::numbers::pi_v<float32>) / static_cast<float32>(CurvedCasterSegments);
			const float32 X = std::cos(Angle);
			const float32 Z = std::sin(Angle);
			CurvedCaster << "v " << X << " -1 " << Z << "\nv " << X << " 1 " << Z << "\n";
		}
		CurvedCaster << "v 0 -1 0\nv 0 1 0\n";
		const uint32 BottomCenter = CurvedCasterSegments * 2U + 1U;
		const uint32 TopCenter = BottomCenter + 1U;
		for (uint32 Segment = 0U; Segment < CurvedCasterSegments; ++Segment)
		{
			const uint32 Next = (Segment + 1U) % CurvedCasterSegments;
			const uint32 Bottom = Segment * 2U + 1U;
			const uint32 Top = Bottom + 1U;
			const uint32 NextBottom = Next * 2U + 1U;
			const uint32 NextTop = NextBottom + 1U;
			CurvedCaster << "f " << Bottom << ' ' << NextTop << ' ' << NextBottom << "\nf " << Bottom << ' ' << Top << ' ' << NextTop
						 << "\n";
			CurvedCaster << "f " << BottomCenter << ' ' << Bottom << ' ' << NextBottom << "\nf " << TopCenter << ' ' << NextTop << ' '
						 << Top << "\n";
		}
		CurvedCaster.flush();
		if (!CurvedCaster)
			throw std::runtime_error("Could not write the curved shadow validation model");
		CurvedCaster.close();
		std::ofstream Sphere(SpherePath, std::ios::binary | std::ios::trunc);
		if (!Sphere)
			throw std::runtime_error("Could not create the rounded shadow validation sphere");
		constexpr uint32 SphereSegments = 1024U;
		constexpr uint32 SphereRings = 512U;
		for (uint32 Ring = 0U; Ring <= SphereRings; ++Ring)
		{
			const float32 V = static_cast<float32>(Ring) / static_cast<float32>(SphereRings);
			const float32 Latitude = -std::numbers::pi_v<float32> * 0.5f + V * std::numbers::pi_v<float32>;
			const float32 CosLatitude = std::cos(Latitude);
			const float32 SinLatitude = std::sin(Latitude);
			for (uint32 Segment = 0U; Segment <= SphereSegments; ++Segment)
			{
				const float32 U = static_cast<float32>(Segment) / static_cast<float32>(SphereSegments);
				const float32 Longitude = U * std::numbers::pi_v<float32> * 2.0f;
				Sphere << "v " << CosLatitude * std::cos(Longitude) - 1.2f << ' ' << SinLatitude << ' '
					   << CosLatitude * std::sin(Longitude) + 0.8f << "\n";
			}
		}
		const uint32 GroundVertex = (SphereRings + 1U) * (SphereSegments + 1U) + 1U;
		Sphere << "v -5 -1.25 5\nv 5 -1.25 5\nv 5 -1.25 -5\nv -5 -1.25 -5\n";
		Sphere << "f " << GroundVertex << ' ' << GroundVertex + 1U << ' ' << GroundVertex + 2U << ' ' << GroundVertex + 3U << "\n";
		for (uint32 Ring = 0U; Ring < SphereRings; ++Ring)
		{
			for (uint32 Segment = 0U; Segment < SphereSegments; ++Segment)
			{
				const uint32 First = Ring * (SphereSegments + 1U) + Segment + 1U;
				const uint32 Second = First + SphereSegments + 1U;
				Sphere << "f " << First << ' ' << Second << ' ' << First + 1U << "\nf " << First + 1U << ' ' << Second << ' ' << Second + 1U
					   << "\n";
			}
		}
		Sphere.flush();
		if (!Sphere)
			throw std::runtime_error("Could not write the rounded shadow validation sphere");
		Sphere.close();

		resource::AssetManager Assets(ContentRoot);
		editor::reflection::ReflectionRegistry Reflection;
		editor::reflection::RegisterCoreComponentReflection(Reflection);
		editor::document::SceneDocument Document("Game Layer Validation");
		const world::ObjectHandle CameraObject = Document.CreateObject("Primary Camera");
		const world::ComponentHandle<components::CObjectCameraComponent> Camera =
			Document.GetScene().AddComponent<components::CObjectCameraComponent>(CameraObject);
		const world::ComponentHandle<components::CObjectTransformComponent> CameraTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(CameraObject);
		const world::ObjectHandle ModelObject = Document.CreateObject("Validation Cube");
		(void)Document.GetScene().AddComponent<components::CObjectMeshComponent>(
			ModelObject, Assets.GetAsset<resource::ModelAsset>("Models/ValidationCurvedCaster.obj"));
		const world::ComponentHandle<components::CObjectTransformComponent> ModelTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(ModelObject);
		const world::ObjectHandle GroundObject = Document.CreateObject("Validation Ground");
		(void)Document.GetScene().AddComponent<components::CObjectMeshComponent>(
			GroundObject, Assets.GetAsset<resource::ModelAsset>("Models/ValidationCube.obj"));
		const world::ComponentHandle<components::CObjectTransformComponent> GroundTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(GroundObject);
		const world::ObjectHandle LightObject = Document.CreateObject("Validation Light");
		const world::ComponentHandle<components::CObjectDirectionalLightComponent> Light =
			Document.GetScene().AddComponent<components::CObjectDirectionalLightComponent>(LightObject);
		const world::ComponentHandle<components::CObjectTransformComponent> LightTransform =
			Document.GetScene().GetComponent<components::CObjectTransformComponent>(LightObject);
		{
			auto Access = Document.GetScene().Write();
			Access.Resolve(Camera).SetPrimary(true);
			Access.Resolve(Camera).SetProjection(components::CameraProjection::Orthographic);
			Access.Resolve(Camera).SetOrthographicHeight(4.0f);
			Access.Resolve(CameraTransform).SetPosition({0.0f, 10.0f, 0.0f});
			Access.Resolve(CameraTransform).LookAt({0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
			// Keep the tall caster immediately outside the camera while its long,
			// oblique directional shadow crosses the visible ground. This isolates
			// the shadow contour from the caster's own rasterized silhouette and
			// magnifies one 2048-map texel to several presentation pixels.
			Access.Resolve(ModelTransform).SetPosition({-4.0f, 2.0f, 2.0f});
			Access.Resolve(ModelTransform).SetScale({0.3f, 3.0f, 0.3f});
			Access.Resolve(GroundTransform).SetPosition({0.0f, -1.25f, 0.0f});
			Access.Resolve(GroundTransform).SetScale({5.0f, 0.25f, 5.0f});
			Access.Resolve(Light).SetIlluminanceLux(25'000.0f);
			Access.Resolve(Light).GetShadowSettings().CastShadows = true;
			Access.Resolve(LightTransform).SetRotationEuler({-55.0f, -35.0f, 0.0f});
		}
		editor::serialization::SceneDocumentSerializer::Save(Document, Reflection, Assets, ScenePath);

		editor::document::SceneDocument RoundedDocument("Rounded Shadow Validation");
		const world::ObjectHandle RoundedCameraObject = RoundedDocument.CreateObject("Rounded Shadow Camera");
		const world::ComponentHandle<components::CObjectCameraComponent> RoundedCamera =
			RoundedDocument.GetScene().AddComponent<components::CObjectCameraComponent>(RoundedCameraObject);
		const world::ComponentHandle<components::CObjectTransformComponent> RoundedCameraTransform =
			RoundedDocument.GetScene().GetComponent<components::CObjectTransformComponent>(RoundedCameraObject);
		const world::ObjectHandle SphereObject = RoundedDocument.CreateObject("Validation Sphere");
		(void)RoundedDocument.GetScene().AddComponent<components::CObjectMeshComponent>(
			SphereObject, Assets.GetAsset<resource::ModelAsset>("Models/ValidationSphere.obj"));
		const world::ComponentHandle<components::CObjectTransformComponent> SphereTransform =
			RoundedDocument.GetScene().GetComponent<components::CObjectTransformComponent>(SphereObject);
		const world::ObjectHandle RoundedLightObject = RoundedDocument.CreateObject("Rounded Shadow Sun");
		const world::ComponentHandle<components::CObjectDirectionalLightComponent> RoundedLight =
			RoundedDocument.GetScene().AddComponent<components::CObjectDirectionalLightComponent>(RoundedLightObject);
		const world::ComponentHandle<components::CObjectTransformComponent> RoundedLightTransform =
			RoundedDocument.GetScene().GetComponent<components::CObjectTransformComponent>(RoundedLightObject);
		{
			auto Access = RoundedDocument.GetScene().Write();
			Access.Resolve(RoundedCamera).SetPrimary(true);
			Access.Resolve(RoundedCamera).SetProjection(components::CameraProjection::Orthographic);
			Access.Resolve(RoundedCamera).SetOrthographicHeight(2.6f);
			Access.Resolve(RoundedCameraTransform).SetPosition({-0.3f, 10.0f, 0.45f});
			Access.Resolve(RoundedCameraTransform).LookAt({0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
			Access.Resolve(SphereTransform).SetPosition({0.0f, 0.0f, 0.0f});
			Access.Resolve(RoundedLight).SetIlluminanceLux(25'000.0f);
			Access.Resolve(RoundedLight).GetShadowSettings().CastShadows = true;
			Access.Resolve(RoundedLight).GetShadowSettings().Resolution = components::ShadowResolution::Resolution2048;
			Access.Resolve(RoundedLight).GetShadowSettings().FilterRadius = 1.5f;
			Access.Resolve(RoundedLightTransform).SetRotationEuler({-55.0f, -35.0f, 0.0f});
		}
		editor::serialization::SceneDocumentSerializer::Save(RoundedDocument, Reflection, Assets, RoundedScenePath);
		editor::serialization::ProjectDescriptorSerializer::Save(
			{.Name = "Game Layer Validation", .DescriptorPath = this->DescriptorPath, .StartupScene = "Scenes/Startup.enginelevel"});
	}

	~TemporaryGameProject()
	{
		std::error_code Error;
		std::filesystem::remove_all(this->Root, Error);
	}

	[[nodiscard]] const std::filesystem::path &GetDescriptorPath() const noexcept
	{
		return this->DescriptorPath;
	}

	[[nodiscard]] const std::filesystem::path &GetRoot() const noexcept
	{
		return this->Root;
	}

	[[nodiscard]] static std::filesystem::path GetPresentationCapturePath()
	{
#ifdef _DEBUG
		constexpr string_view Configuration = "Debug";
#else
		constexpr string_view Configuration = "Release";
#endif
		return std::filesystem::temp_directory_path() / "OpenFrame" / "ValidationEvidence" /
			   (string(Configuration) + "-HeadlessPresentation.bmp");
	}

	[[nodiscard]] static std::filesystem::path GetRoundedPresentationCapturePath()
	{
#ifdef _DEBUG
		constexpr string_view Configuration = "Debug";
#else
		constexpr string_view Configuration = "Release";
#endif
		return std::filesystem::temp_directory_path() / "OpenFrame" / "ValidationEvidence" /
			   (string(Configuration) + "-RoundedShadowPresentation.bmp");
	}

  private:
	std::filesystem::path Root;
	std::filesystem::path DescriptorPath;
};

[[nodiscard]] core::WindowSpecification MakeValidationWindow(const string &Title)
{
	core::WindowSpecification Window;
	Window.Title = Title;
	Window.Extent = {640, 360};
	Window.Visible = false;
	Window.Focused = false;
	Window.HeadlessValidation = true;
	return Window;
}

[[nodiscard]] std::filesystem::path FindEngineContentRoot()
{
	std::filesystem::path Ancestor = std::filesystem::current_path();
	for (uint32 Depth = 0; Depth < 8 && !Ancestor.empty(); ++Depth)
	{
		const std::array Candidates{Ancestor, Ancestor / "OpenFrame"};
		for (const std::filesystem::path &Candidate : Candidates)
		{
			std::error_code Error;
			if (std::filesystem::is_directory(Candidate / "Shaders", Error) && !Error)
				return std::filesystem::absolute(Candidate).lexically_normal();
		}
		Ancestor = Ancestor.parent_path();
	}
	throw std::runtime_error("Validation could not locate the engine shader content root");
}

void ValidateCrispShadowCapture(const std::filesystem::path &CapturePath)
{
	struct ShadowContourMetrics final
	{
		uint32 LongestPlateauRows = 0;
		uint32 MaximumAdjacentJump = 0;
		float64 Slope = 0.0;
		float64 MaximumDeviation = 0.0;
		float64 RootMeanSquareDeviation = 0.0;
	};
	const auto MeasureContour = [](const std::span<const uint32> Boundary)
	{
		if (Boundary.size() < 3U)
			throw std::invalid_argument("Shadow contour measurement requires at least three rows");
		float64 SumY = 0.0;
		float64 SumX = 0.0;
		float64 SumYY = 0.0;
		float64 SumYX = 0.0;
		uint32 CurrentPlateauRows = 1U;
		ShadowContourMetrics Metrics{.LongestPlateauRows = 1U};
		for (usize Index = 0; Index < Boundary.size(); ++Index)
		{
			const float64 Y = static_cast<float64>(Index);
			const float64 X = static_cast<float64>(Boundary[Index]);
			SumY += Y;
			SumX += X;
			SumYY += Y * Y;
			SumYX += Y * X;
			if (Index == 0U)
				continue;
			const uint32 Previous = Boundary[Index - 1U];
			const uint32 Current = Boundary[Index];
			Metrics.MaximumAdjacentJump =
				std::max(Metrics.MaximumAdjacentJump, Previous > Current ? Previous - Current : Current - Previous);
			CurrentPlateauRows = Current == Previous ? CurrentPlateauRows + 1U : 1U;
			Metrics.LongestPlateauRows = std::max(Metrics.LongestPlateauRows, CurrentPlateauRows);
		}
		const float64 Count = static_cast<float64>(Boundary.size());
		const float64 Denominator = Count * SumYY - SumY * SumY;
		if (std::abs(Denominator) <= 1.0e-12)
			throw std::runtime_error("Shadow contour regression produced a degenerate line fit");
		Metrics.Slope = (Count * SumYX - SumY * SumX) / Denominator;
		const float64 Intercept = (SumX - Metrics.Slope * SumY) / Count;
		float64 SquaredDeviation = 0.0;
		for (usize Index = 0; Index < Boundary.size(); ++Index)
		{
			const float64 Expected = Metrics.Slope * static_cast<float64>(Index) + Intercept;
			const float64 Deviation = std::abs(static_cast<float64>(Boundary[Index]) - Expected);
			Metrics.MaximumDeviation = std::max(Metrics.MaximumDeviation, Deviation);
			SquaredDeviation += Deviation * Deviation;
		}
		Metrics.RootMeanSquareDeviation = std::sqrt(SquaredDeviation / Count);
		return Metrics;
	};
	// Prove that the metric distinguishes an ordinarily rasterized diagonal
	// from the nine-pixel shadow-map staircase observed in the editor capture.
	std::array<uint32, 36> ReferenceContour{};
	std::array<uint32, 36> BlockQuantizedContour{};
	for (uint32 Row = 0; Row < ReferenceContour.size(); ++Row)
	{
		ReferenceContour[Row] = 20U + Row / 2U;
		BlockQuantizedContour[Row] = 20U + (Row / 9U) * 9U;
	}
	const ShadowContourMetrics ReferenceMetrics = MeasureContour(ReferenceContour);
	const ShadowContourMetrics BlockMetrics = MeasureContour(BlockQuantizedContour);
	if (ReferenceMetrics.LongestPlateauRows > 2U || ReferenceMetrics.MaximumAdjacentJump > 2U || BlockMetrics.LongestPlateauRows != 9U ||
		BlockMetrics.MaximumAdjacentJump != 9U || BlockMetrics.MaximumDeviation <= 2.0)
	{
		throw std::runtime_error("Shadow contour regression metric does not reject nine-pixel staircase quantization");
	}

	std::ifstream Input(CapturePath, std::ios::binary | std::ios::ate);
	if (!Input || Input.tellg() < 54)
		throw std::runtime_error("Shadow presentation capture is missing its bitmap header");
	const usize Size = static_cast<usize>(Input.tellg());
	std::vector<uint8> Bytes(Size);
	Input.seekg(0);
	Input.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
	if (!Input)
		throw std::runtime_error("Shadow presentation capture could not be read");
	const auto ReadUInt16 = [&Bytes](const usize Offset)
	{ return static_cast<uint16>(Bytes.at(Offset)) | static_cast<uint16>(static_cast<uint16>(Bytes.at(Offset + 1U)) << 8U); };
	const auto ReadUInt32 = [&Bytes](const usize Offset)
	{
		return static_cast<uint32>(Bytes.at(Offset)) | static_cast<uint32>(Bytes.at(Offset + 1U)) << 8U |
			   static_cast<uint32>(Bytes.at(Offset + 2U)) << 16U | static_cast<uint32>(Bytes.at(Offset + 3U)) << 24U;
	};
	const uint32 PixelOffset = ReadUInt32(10U);
	const uint32 Width = ReadUInt32(18U);
	const uint32 Height = ReadUInt32(22U);
	if (Width < 64U || Height < 64U || ReadUInt16(28U) != 24U || ReadUInt32(30U) != 0U)
		throw std::runtime_error("Shadow presentation capture has an unsupported bitmap layout");
	const uint64 RowBytes = (static_cast<uint64>(Width) * 3U + 3U) & ~uint64{3U};
	if (static_cast<uint64>(PixelOffset) + RowBytes * Height > Bytes.size())
		throw std::runtime_error("Shadow presentation capture is truncated");
	const auto Luminance = [&Bytes, PixelOffset, RowBytes, Height](const uint32 X, const uint32 Y)
	{
		const uint32 StorageRow = Height - 1U - Y;
		const usize Offset = static_cast<usize>(static_cast<uint64>(PixelOffset) + RowBytes * StorageRow + static_cast<uint64>(X) * 3U);
		return (static_cast<float32>(Bytes[Offset]) + static_cast<float32>(Bytes[Offset + 1U]) + static_cast<float32>(Bytes[Offset + 2U])) /
			   3.0f;
	};
	const uint32 SearchBeginX = std::max(2U, static_cast<uint32>(static_cast<float32>(Width) * 0.01f));
	const uint32 SearchEndX = static_cast<uint32>(static_cast<float32>(Width) * 0.55f);
	const uint32 SearchBeginY = static_cast<uint32>(static_cast<float32>(Height) * 0.05f);
	const uint32 SearchEndY = static_cast<uint32>(static_cast<float32>(Height) * 0.95f);
	std::vector<uint32> CurrentContour;
	std::vector<uint32> LongestContour;
	std::vector<uint32> CurrentTransitionWidths;
	std::vector<uint32> LongestTransitionWidths;
	CurrentContour.reserve(SearchEndY - SearchBeginY);
	LongestContour.reserve(SearchEndY - SearchBeginY);
	CurrentTransitionWidths.reserve(SearchEndY - SearchBeginY);
	LongestTransitionWidths.reserve(SearchEndY - SearchBeginY);
	float64 AccumulatedRise = 0.0;
	float64 LongestContourRise = 0.0;
	for (uint32 Y = SearchBeginY; Y < SearchEndY; ++Y)
	{
		uint32 Boundary = SearchBeginX;
		float32 MaximumRise = 0.0f;
		for (uint32 X = SearchBeginX; X + 1U < SearchEndX; ++X)
		{
			const float32 Rise = Luminance(X + 1U, Y) - Luminance(X, Y);
			if (Rise > MaximumRise)
			{
				MaximumRise = Rise;
				Boundary = X;
			}
		}
		if (MaximumRise >= 5.0f)
		{
			const uint32 SampleBegin = Boundary > 10U ? Boundary - 10U : 0U;
			const uint32 SampleEnd = std::min(Width - 1U, Boundary + 11U);
			const float32 DarkReference = Luminance(Boundary > 6U ? Boundary - 6U : 0U, Y);
			const float32 LightReference = Luminance(std::min(Width - 1U, Boundary + 7U), Y);
			const float32 Range = LightReference - DarkReference;
			uint32 TransitionWidth = SampleEnd - SampleBegin + 1U;
			if (Range > 5.0f)
			{
				const float32 LowerThreshold = DarkReference + Range * 0.1f;
				const float32 UpperThreshold = DarkReference + Range * 0.9f;
				std::optional<uint32> LowerCrossing;
				std::optional<uint32> UpperCrossing;
				for (uint32 X = SampleBegin; X <= SampleEnd; ++X)
				{
					const float32 Value = Luminance(X, Y);
					if (!LowerCrossing && Value >= LowerThreshold)
						LowerCrossing = X;
					if (Value >= UpperThreshold)
					{
						UpperCrossing = X;
						break;
					}
				}
				if (LowerCrossing && UpperCrossing && *UpperCrossing >= *LowerCrossing)
					TransitionWidth = *UpperCrossing - *LowerCrossing + 1U;
			}
			if (!CurrentContour.empty())
			{
				const uint32 Previous = CurrentContour.back();
				const uint32 Jump = Previous > Boundary ? Previous - Boundary : Boundary - Previous;
				if (Jump > 2U)
				{
					if (CurrentContour.size() > LongestContour.size())
					{
						LongestContour = CurrentContour;
						LongestTransitionWidths = CurrentTransitionWidths;
						LongestContourRise = AccumulatedRise;
					}
					CurrentContour.clear();
					CurrentTransitionWidths.clear();
					AccumulatedRise = 0.0;
				}
			}
			CurrentContour.push_back(Boundary);
			CurrentTransitionWidths.push_back(TransitionWidth);
			AccumulatedRise += MaximumRise;
			continue;
		}
		if (CurrentContour.size() > LongestContour.size())
		{
			LongestContour = CurrentContour;
			LongestTransitionWidths = CurrentTransitionWidths;
			LongestContourRise = AccumulatedRise;
		}
		CurrentContour.clear();
		CurrentTransitionWidths.clear();
		AccumulatedRise = 0.0;
	}
	if (CurrentContour.size() > LongestContour.size())
	{
		LongestContour = CurrentContour;
		LongestTransitionWidths = CurrentTransitionWidths;
		LongestContourRise = AccumulatedRise;
	}
	const usize MinimumContourRows = static_cast<usize>(Height / 4U);
	if (LongestContour.size() < MinimumContourRows)
		throw std::runtime_error("Shadow presentation capture did not contain the expected long oblique cast-shadow contour");
	const float64 AverageRise = LongestContourRise / static_cast<float64>(LongestContour.size());
	if (AverageRise < 8.0)
		throw std::runtime_error("Shadow presentation contour lacks sufficient cast-shadow contrast");
	std::ranges::sort(LongestTransitionWidths);
	const uint32 RepresentativeTransitionWidth =
		LongestTransitionWidths[std::min(LongestTransitionWidths.size() - 1U, LongestTransitionWidths.size() * 95U / 100U)];
	if (RepresentativeTransitionWidth > 5U)
		throw std::runtime_error("Cast-shadow edge exceeded the five-pixel 95th-percentile 10-90% transition-width budget");
	ShadowContourMetrics Metrics{};
	bool HasObliqueSegment = false;
	float64 BestSegmentScore = std::numeric_limits<float64>::max();
	for (usize Start = 0; Start + MinimumContourRows <= LongestContour.size(); ++Start)
	{
		const ShadowContourMetrics Candidate = MeasureContour(std::span<const uint32>(LongestContour).subspan(Start, MinimumContourRows));
		const float64 AbsoluteSlope = std::abs(Candidate.Slope);
		if (AbsoluteSlope < 0.35 || AbsoluteSlope > 2.5)
			continue;
		const float64 Score = Candidate.MaximumDeviation + Candidate.RootMeanSquareDeviation;
		if (!HasObliqueSegment || Score < BestSegmentScore)
		{
			Metrics = Candidate;
			BestSegmentScore = Score;
			HasObliqueSegment = true;
		}
	}
	if (!HasObliqueSegment)
		throw std::runtime_error("Shadow presentation contour contains no sufficiently long oblique segment for staircase validation");
	if (Metrics.LongestPlateauRows > 2U || Metrics.MaximumAdjacentJump > 2U || Metrics.MaximumDeviation > 2.0 ||
		Metrics.RootMeanSquareDeviation > 1.0)
	{
		std::cerr << "Headless cast-shadow contour failure: rows=" << LongestContour.size() << ", plateau=" << Metrics.LongestPlateauRows
				  << " px (maximum 2), jump=" << Metrics.MaximumAdjacentJump
				  << " px (maximum 2), maximum deviation=" << Metrics.MaximumDeviation
				  << " px (maximum 2), RMS deviation=" << Metrics.RootMeanSquareDeviation << " px (maximum 1)\n";
		throw std::runtime_error("Cast-shadow contour exceeded the two-pixel staircase/deviation budget");
	}
	std::cerr << "Headless cast-shadow contour validation: rows=" << LongestContour.size() << ", plateau=" << Metrics.LongestPlateauRows
			  << " px, jump=" << Metrics.MaximumAdjacentJump << " px, maximum deviation=" << Metrics.MaximumDeviation
			  << " px, RMS deviation=" << Metrics.RootMeanSquareDeviation
			  << " px, 95th-percentile 10-90% transition=" << RepresentativeTransitionWidth << " px\n";
}

void ValidateRoundedShadowCapture(const std::filesystem::path &CapturePath)
{
	std::ifstream Input(CapturePath, std::ios::binary | std::ios::ate);
	if (!Input || Input.tellg() < 54)
		throw std::runtime_error("Rounded-shadow capture is missing its bitmap header");
	const usize Size = static_cast<usize>(Input.tellg());
	std::vector<uint8> Bytes(Size);
	Input.seekg(0);
	Input.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
	if (!Input)
		throw std::runtime_error("Rounded-shadow capture could not be read");
	const auto ReadUInt16 = [&Bytes](const usize Offset)
	{ return static_cast<uint16>(Bytes.at(Offset)) | static_cast<uint16>(static_cast<uint16>(Bytes.at(Offset + 1U)) << 8U); };
	const auto ReadUInt32 = [&Bytes](const usize Offset)
	{
		return static_cast<uint32>(Bytes.at(Offset)) | static_cast<uint32>(Bytes.at(Offset + 1U)) << 8U |
			   static_cast<uint32>(Bytes.at(Offset + 2U)) << 16U | static_cast<uint32>(Bytes.at(Offset + 3U)) << 24U;
	};
	const uint32 PixelOffset = ReadUInt32(10U);
	const uint32 Width = ReadUInt32(18U);
	const uint32 Height = ReadUInt32(22U);
	if (Width < 64U || Height < 64U || ReadUInt16(28U) != 24U || ReadUInt32(30U) != 0U)
		throw std::runtime_error("Rounded-shadow capture has an unsupported bitmap layout");
	const uint64 RowBytes = (static_cast<uint64>(Width) * 3U + 3U) & ~uint64{3U};
	if (static_cast<uint64>(PixelOffset) + RowBytes * Height > Bytes.size())
		throw std::runtime_error("Rounded-shadow capture is truncated");
	const auto Luminance = [&Bytes, PixelOffset, RowBytes, Height](const uint32 X, const uint32 Y)
	{
		const uint32 StorageRow = Height - 1U - Y;
		const usize Offset = static_cast<usize>(static_cast<uint64>(PixelOffset) + RowBytes * StorageRow + static_cast<uint64>(X) * 3U);
		return (static_cast<float32>(Bytes[Offset]) + static_cast<float32>(Bytes[Offset + 1U]) + static_cast<float32>(Bytes[Offset + 2U])) /
			   3.0f;
	};

	const uint32 SearchBeginX = static_cast<uint32>(static_cast<float32>(Width) * 0.4f);
	const uint32 SearchEndX = static_cast<uint32>(static_cast<float32>(Width) * 0.7f);
	const uint32 SearchBeginY = static_cast<uint32>(static_cast<float32>(Height) * 0.25f);
	const uint32 SearchEndY = static_cast<uint32>(static_cast<float32>(Height) * 0.6f);
	std::vector<uint32> Boundaries;
	std::vector<uint32> TransitionWidths;
	Boundaries.reserve(SearchEndY - SearchBeginY);
	TransitionWidths.reserve(SearchEndY - SearchBeginY);
	for (uint32 Y = SearchBeginY; Y < SearchEndY; ++Y)
	{
		uint32 Boundary = SearchBeginX;
		float32 MaximumRise = 0.0f;
		for (uint32 X = SearchBeginX; X + 1U < SearchEndX; ++X)
		{
			const float32 Rise = Luminance(X + 1U, Y) - Luminance(X, Y);
			if (Rise > MaximumRise)
			{
				MaximumRise = Rise;
				Boundary = X;
			}
		}
		const auto AverageRange = [&Luminance, Y](const uint32 Begin, const uint32 End)
		{
			float32 Sum = 0.0f;
			for (uint32 X = Begin; X < End; ++X)
				Sum += Luminance(X, Y);
			return Sum / static_cast<float32>(End - Begin);
		};
		const float32 DarkReference = AverageRange(Boundary - 12U, Boundary - 4U);
		const float32 LightReference = AverageRange(Boundary + 4U, Boundary + 12U);
		// Horizontal transition width is meaningful only where the curved
		// perimeter normal has a strong horizontal component. Near its top tangent
		// the same narrow edge spans many X pixels by geometry alone.
		if (MaximumRise < 3.0f || LightReference - DarkReference < 12.0f)
			continue;
		const float32 Range = LightReference - DarkReference;
		const float32 LowerThreshold = DarkReference + Range * 0.1f;
		const float32 UpperThreshold = DarkReference + Range * 0.9f;
		std::optional<uint32> LowerCrossing;
		std::optional<uint32> UpperCrossing;
		for (uint32 X = Boundary - 16U; X <= Boundary + 16U; ++X)
		{
			const float32 Value = Luminance(X, Y);
			if (!LowerCrossing && Value >= LowerThreshold)
				LowerCrossing = X;
			if (Value >= UpperThreshold)
			{
				UpperCrossing = X;
				break;
			}
		}
		if (!LowerCrossing || !UpperCrossing || *UpperCrossing < *LowerCrossing)
			throw std::runtime_error("Rounded-shadow edge did not contain ordered 10-90% crossings");
		Boundaries.push_back(Boundary);
		TransitionWidths.push_back(*UpperCrossing - *LowerCrossing + 1U);
	}
	if (Boundaries.size() < static_cast<usize>(Height / 8U))
		throw std::runtime_error("Rounded-shadow capture did not contain the expected sphere-shadow perimeter");
	const uint32 MaximumBoundary = *std::max_element(Boundaries.begin(), Boundaries.end());
	if (MaximumBoundary - Boundaries.front() < Width / 55U || MaximumBoundary - Boundaries.back() < Width / 100U)
		throw std::runtime_error("Rounded-shadow regression did not exercise a genuinely curved silhouette");
	std::ranges::sort(TransitionWidths);
	const uint32 RepresentativeTransitionWidth =
		TransitionWidths[std::min(TransitionWidths.size() - 1U, TransitionWidths.size() * 95U / 100U)];
	if (RepresentativeTransitionWidth > 6U)
		throw std::runtime_error("Rounded sphere shadow exceeded the six-pixel 95th-percentile 10-90% transition-width budget");
	const usize AntialiasedRows =
		static_cast<usize>(std::ranges::count_if(TransitionWidths, [](const uint32 Width) { return Width >= 2U; }));
	if (AntialiasedRows * 10U < TransitionWidths.size() * 7U)
		throw std::runtime_error("Rounded sphere shadow regressed to a binary pixel-stepped perimeter");
	std::cerr << "Headless rounded sphere-shadow validation: perimeter rows=" << Boundaries.size()
			  << ", horizontal curvature=" << MaximumBoundary - std::min(Boundaries.front(), Boundaries.back())
			  << " px, 95th-percentile 10-90% transition=" << RepresentativeTransitionWidth
			  << " px, antialiased-row coverage=" << AntialiasedRows * 100U / TransitionWidths.size() << "%\n";
}
} // namespace

int wmain(const int ArgumentCount, wchar_t *Arguments[])
{
	try
	{
		bool RunEditorCore = ArgumentCount == 1;
		bool RunRenderCore = ArgumentCount == 1;
		bool RunEditorLayer = false;
		bool RunGameLayer = false;
		std::filesystem::path ValidGameModule;
		std::filesystem::path InvalidGameModule;
		std::filesystem::path MSBuild;
		std::filesystem::path BuildSolution;
		std::filesystem::path BuiltGameModule;
		string BuildConfiguration;

		for (int32 ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
		{
			const std::wstring_view Argument = Arguments[ArgumentIndex];
			if (Argument == L"--all")
			{
				RunEditorCore = true;
				RunRenderCore = true;
				RunEditorLayer = true;
				RunGameLayer = true;
			}
			else if (Argument == L"--editor-core")
				RunEditorCore = true;
			else if (Argument == L"--render-core")
				RunRenderCore = true;
			else if (Argument == L"--editor-layer")
				RunEditorLayer = true;
			else if (Argument == L"--game-layer")
				RunGameLayer = true;
			else if (Argument == L"--game-module")
			{
				if (ArgumentIndex + 2 >= ArgumentCount)
					throw std::invalid_argument("--game-module requires valid and invalid DLL paths");
				ValidGameModule = Arguments[++ArgumentIndex];
				InvalidGameModule = Arguments[++ArgumentIndex];
			}
			else if (Argument == L"--project-build")
			{
				if (ArgumentIndex + 4 >= ArgumentCount)
					throw std::invalid_argument("--project-build requires MSBuild, solution, built DLL, and configuration arguments");
				MSBuild = Arguments[++ArgumentIndex];
				BuildSolution = Arguments[++ArgumentIndex];
				BuiltGameModule = Arguments[++ArgumentIndex];
				const std::wstring_view WideConfiguration = Arguments[++ArgumentIndex];
				if (!std::ranges::all_of(WideConfiguration, [](const wchar_t Character) { return Character >= 0 && Character <= 0x7f; }))
					throw std::invalid_argument("--project-build configuration must be ASCII");
				BuildConfiguration.resize(WideConfiguration.size());
				std::ranges::transform(WideConfiguration, BuildConfiguration.begin(),
									   [](const wchar_t Character) { return static_cast<char>(Character); });
			}
			else
				throw std::invalid_argument("Unknown Validation argument");
		}
		const std::filesystem::path EngineContentRoot = FindEngineContentRoot();
		std::filesystem::current_path(EngineContentRoot);

		if (RunEditorCore)
			editor::validation::RunDeterministicEditorCoreChecks();
		if (!ValidGameModule.empty())
			editor::validation::RunDeterministicGameModuleChecks(ValidGameModule, InvalidGameModule);
		if (!MSBuild.empty())
			editor::validation::RunDeterministicProjectBuildChecks(MSBuild, BuildSolution, BuiltGameModule, std::move(BuildConfiguration));
		if (RunRenderCore)
		{
			core::Application Application({.Window = MakeValidationWindow("Render Core Validation"), .MaximumFrameCount = 1});
			pipeline::device::Device &Device = Application.GetWindowManager().GetDevice(Application.GetPrimaryWindow());
			pipeline::validation::RunDeterministicRenderCoreChecks(Device);
			std::cerr << "[Validation] Detached window, DPI, and reset-context shutdown\n";
			ValidateDetachedWindowLifecycle(Application.GetWindowManager());
			std::atomic<uint32> StopLayerRuns = 0;
			std::atomic<uint32> StopSentinelRuns = 0;
			Application.PushLayer<StopValidationLayer>(StopLayerRuns);
			Application.PushLayer<StopSentinelLayer>(StopSentinelRuns);
			Application.Main();
			if (StopLayerRuns.load(std::memory_order_acquire) != 1 || StopSentinelRuns.load(std::memory_order_acquire) != 0)
				throw std::runtime_error("application stop request did not terminate layer dispatch immediately");
		}
		if (RunEditorLayer)
		{
			std::cerr << "[Validation] Editor Home with no project or command-line project argument\n";
			{
				core::Application HomeApplication({.Window = MakeValidationWindow("Editor Home Validation")});
				HomeApplication.PushLayer<editor::EditorLayer>(
					editor::EditorLayerSpecification{.EngineContentRoot = EngineContentRoot, .MaximumRenderedFrames = 2});
				HomeApplication.Main();
			}
			std::cerr << "[Validation] Editor layer project setup\n";
			TemporaryEditorProject Project;
			std::cerr << "[Validation] Editor layer application setup\n";
			core::Application Application({.Window = MakeValidationWindow("Editor Layer Validation")});
			std::cerr << "[Validation] Editor layer construction\n";
			editor::EditorLayer &Layer = Application.PushLayer<editor::EditorLayer>(editor::EditorLayerSpecification{
				.InitialProject = editor::project::ProjectDescriptor{.Name = "Validation", .DescriptorPath = Project.GetDescriptorPath()},
				.EngineContentRoot = EngineContentRoot,
				.MaximumRenderedFrames = 2});
			(void)Layer.GetSession().GetDocument().CreateObject("Gizmo validation object");
			(void)Layer.GetSession().QueueViewportPick({.Value = 2}, 0.5f, 0.5f, editor::viewport::SelectionOperation::Replace);
			std::cerr << "[Validation] Editor layer frames\n";
			Application.Main();
			std::cerr << "[Validation] Editor layer shutdown\n";
		}
		if (RunGameLayer)
		{
			TemporaryGameProject Project;
			const std::filesystem::path CapturePath = TemporaryGameProject::GetPresentationCapturePath();
			std::error_code RemoveError;
			std::filesystem::remove(CapturePath, RemoveError);
			{
				core::Application Application({.Window = MakeValidationWindow("Game Layer Validation"), .MaximumFrameCount = 32});
				Application.PushLayer<core::GameLayer>(core::GameLayerSpecification{.ProjectName = "Game Layer Validation",
																					.ContentRoot = Project.GetRoot() / "Content",
																					.EngineContentRoot = EngineContentRoot,
																					.StartupScene = "Scenes/Startup.enginelevel",
																					.CacheRoot = Project.GetRoot() / "Intermediate",
																					.HeadlessPresentationValidation = true,
																					.HeadlessPresentationCapturePath = CapturePath});
				Application.Main();
			}
			std::error_code CaptureError;
			if (!std::filesystem::is_regular_file(CapturePath, CaptureError) || CaptureError ||
				std::filesystem::file_size(CapturePath) <= 32U)
				throw std::runtime_error("Game-layer rendering smoke test did not produce a validated presentation capture");
			ValidateCrispShadowCapture(CapturePath);

			const std::filesystem::path RoundedCapturePath = TemporaryGameProject::GetRoundedPresentationCapturePath();
			std::filesystem::remove(RoundedCapturePath, RemoveError);
			{
				core::WindowSpecification RoundedWindow = MakeValidationWindow("Rounded Shadow Validation");
				RoundedWindow.Extent = {1'024U, 612U};
				core::Application Application({.Window = std::move(RoundedWindow), .MaximumFrameCount = 64});
				Application.PushLayer<core::GameLayer>(core::GameLayerSpecification{.ProjectName = "Rounded Shadow Validation",
																					.ContentRoot = Project.GetRoot() / "Content",
																					.EngineContentRoot = EngineContentRoot,
																					.StartupScene = "Scenes/Rounded.enginelevel",
																					.CacheRoot = Project.GetRoot() / "IntermediateRounded",
																					.HeadlessPresentationValidation = true,
																					.HeadlessPresentationCapturePath = RoundedCapturePath});
				Application.Main();
			}
			if (!std::filesystem::is_regular_file(RoundedCapturePath, CaptureError) || CaptureError ||
				std::filesystem::file_size(RoundedCapturePath) <= 32U)
				throw std::runtime_error("Rounded-shadow rendering test did not produce a validated presentation capture");
			ValidateRoundedShadowCapture(RoundedCapturePath);
		}

		std::cout << "Validation completed successfully\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		std::cerr << "Validation failed: " << Exception.what() << '\n';
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Validation failed: unknown non-standard exception\n";
		return EXIT_FAILURE;
	}
}
