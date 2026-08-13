#pragma once

#include "RenderPassPipelineSet.h"
#include "Source/pipeline/shader/ShaderLibrary.h"
#include "Source/types.h"

namespace core::threading
{
class TaskScheduler;
}

namespace pipeline::render
{
class ENGINE_API RenderPipelineLibrary final
{
  public:
	RenderPipelineLibrary(pipeline::device::Device &Device, resource::AssetManager &Assets, bool ManualSRGBEncode);
	static void PreloadShaderSources(resource::AssetManager &Assets, core::threading::TaskScheduler &Scheduler);

	void BeginFrame();
	[[nodiscard]] RenderPassPipelineSet GetPipelineSet();
	[[nodiscard]] const string &GetLastDiagnostic() const noexcept;

  private:
	pipeline::shader::ShaderLibrary Shaders;
	uint32 ShadowDepthPipeline = 0;
	uint32 DepthPrepassPipeline = 0;
	uint32 GBufferPipeline = 0;
	uint32 TransparentOITPipeline = 0;
	uint32 AccuratePickingPipeline = 0;
	uint32 EditorWireframePipeline = 0;
	uint32 EditorGridPipeline = 0;
	uint32 EditorDebugLinePipeline = 0;
	uint32 GizmoOverlayPipeline = 0;
	uint32 ToneMapPipeline = 0;
	uint32 VisibilityCullPipeline = 0;
	uint32 VisibilityPrefixScanPipeline = 0;
	uint32 VisibilityBlockPrefixScanPipeline = 0;
	uint32 VisibilityFinalizePipeline = 0;
	uint32 VisibilityScatterPipeline = 0;
	uint32 HierarchicalDepthPipeline = 0;
	uint32 ClusteredLightsPipeline = 0;
	uint32 DeferredLightingPipeline = 0;
	uint32 OITCompositionPipeline = 0;
	uint32 TemporalAAPipeline = 0;
	uint32 ViewportVisualizationPipeline = 0;
	uint32 SelectionOutlinePipeline = 0;
	uint32 AutoExposurePipeline = 0;
	uint32 BloomPipeline = 0;
};
} // namespace pipeline::render
