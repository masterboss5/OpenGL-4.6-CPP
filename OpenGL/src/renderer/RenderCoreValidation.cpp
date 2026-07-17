#include "RenderCoreValidation.h"

#include <stdexcept>

#include <ext/matrix_clip_space.hpp>

#include "RenderGraph.h"
#include "RendererGpuTypes.h"
#include "ScenePreparation.h"
#include "StaticMesh.h"
#include "src/pipeline/device/OpenGLRuntime.h"
#include "src/pipeline/shader/ShaderLibrary.h"
#include "src/resource/asset/AssetManager.h"
#include "src/renderer/PBRMaterial.h"
#include "src/scene/SceneCollection.h"
#include "src/scene/StaticMeshObject.h"

namespace renderer::validation
{
	namespace
	{
		void require(bool condition, string_view diagnostic)
		{
			if (!condition) throw std::runtime_error("Render-core deterministic validation failed: " + string(diagnostic));
		}

		void validateGraphDependencies()
		{
			graph::RenderGraph invalidGraph;
			invalidGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle unwrittenTexture = invalidGraph.createTexture({ .debugName = "UnwrittenTexture", .extent = { 16, 16 } });
			(void)invalidGraph.addPass({ .name = "InvalidRead", .queue = graph::PassQueue::Compute, .readTextures = { unwrittenTexture }, .execute = [](graph::RenderGraphContext&) {} });
			bool rejectedUnwrittenRead = false;
			try { invalidGraph.compile(); }
			catch (const std::logic_error&) { rejectedUnwrittenRead = true; }
			require(rejectedUnwrittenRead, "a render graph read-before-write dependency was accepted");

			graph::RenderGraph validGraph;
			validGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle producedTexture = validGraph.createTexture({ .debugName = "ProducedTexture", .extent = { 16, 16 } });
			(void)validGraph.addPass({ .name = "Produce", .queue = graph::PassQueue::Compute, .writeTextures = { producedTexture }, .execute = [](graph::RenderGraphContext&) {} });
			(void)validGraph.addPass({ .name = "Consume", .queue = graph::PassQueue::Compute, .readTextures = { producedTexture }, .execute = [](graph::RenderGraphContext&) {} });
			validGraph.compile();
			require(validGraph.isCompiled(), "a valid producer-consumer graph did not compile");
		}

		void validateGraphResourceLifetimes()
		{
			// Load/store discard declarations must execute as real framebuffer
			// invalidations. A complete no-op graphics pass is sufficient here:
			// RenderGraph checks all OpenGL errors after that pass.
			graph::RenderGraph discardGraph;
			discardGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle discardTexture = discardGraph.createTexture({ .debugName = "DiscardAttachment", .extent = { 16, 16 } });
			(void)discardGraph.addPass({ .name = "DiscardAttachmentPass", .queue = graph::PassQueue::Graphics, .colorAttachments = { { .texture = discardTexture, .load = graph::LoadOperation::Discard, .store = graph::StoreOperation::Discard } }, .execute = [](graph::RenderGraphContext&) {} });
			discardGraph.compile();
			discardGraph.execute();

			// The graph must also allocate and attach a native multisample target;
			// a later resolve pass can consume it without falling back to a hidden
			// single-sample allocation.
			graph::RenderGraph multisampleGraph;
			multisampleGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle multisampleColor = multisampleGraph.createTexture({ .debugName = "MultisampleColor", .extent = { 16, 16 }, .dimension = graph::TextureDimension::Texture2DMultisample, .sampleCount = 4 });
			(void)multisampleGraph.addPass({ .name = "ClearMultisampleColor", .queue = graph::PassQueue::Graphics, .colorAttachments = { { .texture = multisampleColor, .load = graph::LoadOperation::Clear } }, .execute = [](graph::RenderGraphContext&) {} });
			multisampleGraph.compile();
			multisampleGraph.execute();

			// Transient resources whose lifetimes do not overlap should share a
			// physical allocation. This is the graph's transient-memory aliasing
			// contract, not merely a cache optimization.
			graph::RenderGraph aliasedGraph;
			aliasedGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle first = aliasedGraph.createTexture({ .debugName = "AliasedFirst", .extent = { 16, 16 } });
			const graph::TextureHandle second = aliasedGraph.createTexture({ .debugName = "AliasedSecond", .extent = { 16, 16 } });
			GLuint firstPhysicalTexture = 0;
			GLuint secondPhysicalTexture = 0;
			(void)aliasedGraph.addPass({ .name = "WriteAliasedFirst", .queue = graph::PassQueue::Compute, .writeTextures = { first }, .execute = [&firstPhysicalTexture, first](graph::RenderGraphContext& context) { firstPhysicalTexture = context.getTexture(first); } });
			(void)aliasedGraph.addPass({ .name = "WriteAliasedSecond", .queue = graph::PassQueue::Compute, .writeTextures = { second }, .execute = [&secondPhysicalTexture, second](graph::RenderGraphContext& context) { secondPhysicalTexture = context.getTexture(second); } });
			aliasedGraph.compile();
			aliasedGraph.execute();
			require(firstPhysicalTexture != 0 && firstPhysicalTexture == secondPhysicalTexture, "non-overlapping transient textures did not alias");

			// A consumer extends the first texture's lifetime through the second
			// pass, so reusing its allocation for the second texture would corrupt
			// the read. The graph must keep them physically distinct.
			graph::RenderGraph overlappingGraph;
			overlappingGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle producer = overlappingGraph.createTexture({ .debugName = "OverlappingProducer", .extent = { 16, 16 } });
			const graph::TextureHandle concurrent = overlappingGraph.createTexture({ .debugName = "OverlappingConcurrent", .extent = { 16, 16 } });
			GLuint producerPhysicalTexture = 0;
			GLuint concurrentPhysicalTexture = 0;
			(void)overlappingGraph.addPass({ .name = "WriteProducer", .queue = graph::PassQueue::Compute, .writeTextures = { producer }, .execute = [&producerPhysicalTexture, producer](graph::RenderGraphContext& context) { producerPhysicalTexture = context.getTexture(producer); } });
			(void)overlappingGraph.addPass({ .name = "ReadProducerWriteConcurrent", .queue = graph::PassQueue::Compute, .readTextures = { producer }, .writeTextures = { concurrent }, .execute = [&concurrentPhysicalTexture, concurrent](graph::RenderGraphContext& context) { concurrentPhysicalTexture = context.getTexture(concurrent); } });
			overlappingGraph.compile();
			overlappingGraph.execute();
			require(producerPhysicalTexture != 0 && concurrentPhysicalTexture != 0 && producerPhysicalTexture != concurrentPhysicalTexture, "overlapping transient textures aliased unsafely");

			// Persistent history survives equivalent frames but an extent change
			// receives a distinct allocation. OpenGLRenderer compares this extent
			// before enabling Hi-Z/TAA history, so resize cannot sample old data.
			graph::RenderGraph historyGraph;
			historyGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle initialHistory = historyGraph.createTexture({ .debugName = "History", .format = graph::TextureFormat::R32Float, .persistent = true });
			GLuint initialHistoryTexture = 0;
			(void)historyGraph.addPass({ .name = "WriteInitialHistory", .queue = graph::PassQueue::Compute, .writeTextures = { initialHistory }, .execute = [&initialHistoryTexture, initialHistory](graph::RenderGraphContext& context) { initialHistoryTexture = context.getTexture(initialHistory); } });
			historyGraph.compile();
			historyGraph.execute();
			historyGraph.beginFrame({ 16, 16 });
			const graph::TextureHandle matchingHistory = historyGraph.createTexture({ .debugName = "History", .format = graph::TextureFormat::R32Float, .persistent = true });
			GLuint matchingHistoryTexture = 0;
			(void)historyGraph.addPass({ .name = "ReuseHistory", .queue = graph::PassQueue::Compute, .writeTextures = { matchingHistory }, .execute = [&matchingHistoryTexture, matchingHistory](graph::RenderGraphContext& context) { matchingHistoryTexture = context.getTexture(matchingHistory); } });
			historyGraph.compile();
			historyGraph.execute();
			require(initialHistoryTexture != 0 && initialHistoryTexture == matchingHistoryTexture, "persistent history was not retained across equivalent frames");
			historyGraph.beginFrame({ 32, 32 });
			const graph::TextureHandle resizedHistory = historyGraph.createTexture({ .debugName = "History", .format = graph::TextureFormat::R32Float, .persistent = true });
			GLuint resizedHistoryTexture = 0;
			(void)historyGraph.addPass({ .name = "WriteResizedHistory", .queue = graph::PassQueue::Compute, .writeTextures = { resizedHistory }, .execute = [&resizedHistoryTexture, resizedHistory](graph::RenderGraphContext& context) { resizedHistoryTexture = context.getTexture(resizedHistory); } });
			historyGraph.compile();
			historyGraph.execute();
			require(resizedHistoryTexture != 0 && resizedHistoryTexture != initialHistoryTexture, "resized persistent history reused an incompatible allocation");
		}

		void validateGpuVisibilityCompaction()
		{
			// Exercise the actual runtime shaders with 512 one-instance batches.
			// This crosses the 256-entry first-level scan block boundary and proves
			// cull -> block scan -> block-prefix -> finalize -> scatter produces
			// contiguous DrawElementsIndirectCommand base instances without CPU
			// readback between phases.
			constexpr uint32 scratchCapacity = 65'536;
			constexpr uint32 candidateCount = 512;
			constexpr uint32 scanBlockCount = (candidateCount + 255U) / 256U;
			GLuint frameBuffer = 0;
			GLuint candidateBuffer = 0;
			GLuint visibleBuffer = 0;
			GLuint commandBuffer = 0;
			GLuint scratchBuffer = 0;
			GLuint hierarchyTexture = 0;
			try
			{
				resource::AssetManager assets;
				pipeline::shader::ShaderLibrary shaders(assets);
				const uint32 cullPipeline = shaders.createComputePipeline({ .compute = { .path = "shader/Visibility.comp", .stage = pipeline::shader::ShaderStage::Compute } });
				const uint32 prefixPipeline = shaders.createComputePipeline({ .compute = { .path = "shader/VisibilityPrefixScan.comp", .stage = pipeline::shader::ShaderStage::Compute } });
				const uint32 blockPrefixPipeline = shaders.createComputePipeline({ .compute = { .path = "shader/VisibilityBlockPrefixScan.comp", .stage = pipeline::shader::ShaderStage::Compute } });
				const uint32 finalizePipeline = shaders.createComputePipeline({ .compute = { .path = "shader/VisibilityFinalize.comp", .stage = pipeline::shader::ShaderStage::Compute } });
				const uint32 scatterPipeline = shaders.createComputePipeline({ .compute = { .path = "shader/VisibilityScatter.comp", .stage = pipeline::shader::ShaderStage::Compute } });

				std::vector<PreparedInstance> candidates(candidateCount);
				std::vector<RenderCommand> commands(candidateCount);
				for (uint32 index = 0; index < candidateCount; ++index)
				{
					candidates[index] = { .transform = glm::mat4(1.0f), .previousTransform = glm::mat4(1.0f), .worldBounds = glm::vec4(0.0f, 0.0f, 0.0f, 0.1f), .materialIndex = 0, .objectID = index, .batchIndex = index, .flags = 0 };
					commands[index] = { .indexCount = 3, .instanceCount = 0, .firstIndex = 0, .baseVertex = 0, .baseInstance = 0 };
				}
				GpuFrameConstants frameConstants {};
				frameConstants.projection = glm::mat4(1.0f);
				frameConstants.view = glm::mat4(1.0f);
				frameConstants.viewProjection = glm::mat4(1.0f);
				frameConstants.previousViewProjection = glm::mat4(1.0f);
				frameConstants.inverseViewProjection = glm::mat4(1.0f);
				frameConstants.renderExtentAndFar = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
				glCreateBuffers(1, &frameBuffer);
				glNamedBufferStorage(frameBuffer, sizeof(GpuFrameConstants), &frameConstants, 0);
				glCreateBuffers(1, &candidateBuffer);
				glNamedBufferStorage(candidateBuffer, static_cast<GLsizeiptr>(candidates.size() * sizeof(PreparedInstance)), candidates.data(), 0);
				glCreateBuffers(1, &visibleBuffer);
				glNamedBufferStorage(visibleBuffer, static_cast<GLsizeiptr>(candidates.size() * sizeof(PreparedInstance)), nullptr, GL_DYNAMIC_STORAGE_BIT);
				glCreateBuffers(1, &commandBuffer);
				glNamedBufferStorage(commandBuffer, static_cast<GLsizeiptr>(commands.size() * sizeof(RenderCommand)), commands.data(), GL_DYNAMIC_STORAGE_BIT);
				glCreateBuffers(1, &scratchBuffer);
				glNamedBufferStorage(scratchBuffer, static_cast<GLsizeiptr>((scratchCapacity * 4U + 512U) * sizeof(uint32)), nullptr, GL_DYNAMIC_STORAGE_BIT);
				glCreateTextures(GL_TEXTURE_2D, 1, &hierarchyTexture);
				glTextureStorage2D(hierarchyTexture, 1, GL_R32F, 1, 1);

				const uint32 zero = 0;
				glClearNamedBufferData(scratchBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
				glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(RendererBinding::FrameConstants), frameBuffer);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Candidates), candidateBuffer);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::Instances), visibleBuffer);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::VisibilityScratch), scratchBuffer);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(RendererBinding::IndirectCommands), commandBuffer);
				glBindTextureUnit(0, hierarchyTexture);

				auto& cull = shaders.getComputePipeline(cullPipeline);
				cull.setUniformUInt("candidateCount", candidateCount);
				cull.setUniformUInt("pyramidMipCount", 1);
				cull.setUniformUInt("historyValid", 0);
				cull.setUniformUInt("scratchCapacity", scratchCapacity);
				cull.bind();
				glDispatchCompute((candidateCount + 63U) / 64U, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

				auto& prefix = shaders.getComputePipeline(prefixPipeline);
				prefix.setUniformUInt("batchCount", candidateCount);
				prefix.setUniformUInt("scratchCapacity", scratchCapacity);
				prefix.bind();
				glDispatchCompute(scanBlockCount, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

				auto& blockPrefix = shaders.getComputePipeline(blockPrefixPipeline);
				blockPrefix.setUniformUInt("blockCount", scanBlockCount);
				blockPrefix.setUniformUInt("scratchCapacity", scratchCapacity);
				blockPrefix.bind();
				glDispatchCompute(1, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

				auto& finalize = shaders.getComputePipeline(finalizePipeline);
				finalize.setUniformUInt("batchCount", candidateCount);
				finalize.setUniformUInt("scratchCapacity", scratchCapacity);
				finalize.bind();
				glDispatchCompute(scanBlockCount, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

				auto& scatter = shaders.getComputePipeline(scatterPipeline);
				scatter.setUniformUInt("candidateCount", candidateCount);
				scatter.setUniformUInt("scratchCapacity", scratchCapacity);
				scatter.bind();
				glDispatchCompute((candidateCount + 63U) / 64U, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
				pipeline::device::throwPendingOpenGLErrors("deterministic GPU visibility validation");

				std::vector<RenderCommand> compactCommands(candidateCount);
				std::vector<PreparedInstance> compactInstances(candidateCount);
				glGetNamedBufferSubData(commandBuffer, 0, static_cast<GLsizeiptr>(compactCommands.size() * sizeof(RenderCommand)), compactCommands.data());
				glGetNamedBufferSubData(visibleBuffer, 0, static_cast<GLsizeiptr>(compactInstances.size() * sizeof(PreparedInstance)), compactInstances.data());
				pipeline::device::throwPendingOpenGLErrors("GPU visibility validation readback");
				for (uint32 index = 0; index < candidateCount; ++index)
				{
					require(compactCommands[index].instanceCount == 1 && compactCommands[index].baseInstance == index, "parallel prefix scan produced an invalid compact indirect range");
					require(compactInstances[index].batchIndex == index && compactInstances[index].objectID == index, "parallel visibility scatter produced an invalid compact instance stream");
				}
			}
			catch (...)
			{
				if (hierarchyTexture != 0) glDeleteTextures(1, &hierarchyTexture);
				if (scratchBuffer != 0) glDeleteBuffers(1, &scratchBuffer);
				if (commandBuffer != 0) glDeleteBuffers(1, &commandBuffer);
				if (visibleBuffer != 0) glDeleteBuffers(1, &visibleBuffer);
				if (candidateBuffer != 0) glDeleteBuffers(1, &candidateBuffer);
				if (frameBuffer != 0) glDeleteBuffers(1, &frameBuffer);
				throw;
			}
			if (hierarchyTexture != 0) glDeleteTextures(1, &hierarchyTexture);
			if (scratchBuffer != 0) glDeleteBuffers(1, &scratchBuffer);
			if (commandBuffer != 0) glDeleteBuffers(1, &commandBuffer);
			if (visibleBuffer != 0) glDeleteBuffers(1, &visibleBuffer);
			if (candidateBuffer != 0) glDeleteBuffers(1, &candidateBuffer);
			if (frameBuffer != 0) glDeleteBuffers(1, &frameBuffer);
		}

		void validateScenePreparation()
		{
			Texture emptyTexture("");
			Material material(emptyTexture, emptyTexture, emptyTexture, emptyTexture, emptyTexture, emptyTexture, emptyTexture, 1.0f);
			const std::vector<float32> positions { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
			const std::vector<float32> normals { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
			const std::vector<float32> textureCoordinates { 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
			const std::vector<uint32> indices { 0, 1, 2 };
			StaticMesh mesh("RenderCoreValidationMesh", material, positions, indices, textureCoordinates, normals);
			StaticMeshObject visibleObject(&mesh, 0.0f, 0.0f, -5.0f);
			SceneCollection visibleScene;
			visibleScene.beginFrame(1);
			visibleScene.submit(visibleObject, 17);
			visibleScene.seal();
			ScenePreparation preparation;
			const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
			const RenderPreparationResult visible = preparation.prepare(visibleScene, projection, 0, 1);
			require(visible.candidateInstances.size() == 1 && visible.batches.size() == 1 && visible.candidateCommands.size() == 1, "visible mesh did not produce one batch and one indirect command");
			require(visible.candidateCommands.front().instanceCount == 1 && visible.candidateCommands.front().indexCount == 3, "indirect command metadata is invalid");
			require(visible.candidateInstances.front().objectID == 17, "prepared instance lost its stable object identifier");

			StaticMeshObject culledObject(&mesh, 0.0f, 0.0f, 5.0f);
			SceneCollection culledScene;
			culledScene.beginFrame(2);
			culledScene.submit(culledObject, 18);
			culledScene.seal();
			const RenderPreparationResult culled = preparation.prepare(culledScene, projection, 0, 1);
			require(culled.candidateInstances.empty() && culled.batches.empty() && culled.candidateCommands.empty(), "frustum culling retained geometry behind the camera");
			const RenderPreparationResult shadowCasters = preparation.prepare(culledScene, projection, 0, 1, false);
			require(shadowCasters.candidateInstances.size() == 1 && shadowCasters.batches.size() == 1, "all-caster shadow preparation discarded an off-camera shadow caster");
		}
	}

	void runDeterministicRenderCoreChecks()
	{
		validateGraphDependencies();
		validateGraphResourceLifetimes();
		validateScenePreparation();
		validateGpuVisibilityCompaction();
	}
}
