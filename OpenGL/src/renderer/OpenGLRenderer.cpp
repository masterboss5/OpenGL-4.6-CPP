#include "OpenGLRenderer.h"

#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>

#include "StaticMesh.h"
#include "src/core/input/Window.h"
#include "src/pipeline/device/OpenGLRuntime.h"
#include "src/scene/Camera.h"
#include "src/scene/StaticMeshObject.h"

namespace
{
	constexpr uint32 MaximumRenderItems = 65'536;
	constexpr uint32 DirectionalShadowCascadeCount = 4;
	constexpr uint32 MaximumSpotShadowCount = 64;
	constexpr uint32 MaximumPointShadowFaceCount = 96;
	constexpr uint32 MaximumShadowRecords = DirectionalShadowCascadeCount + MaximumSpotShadowCount + MaximumPointShadowFaceCount;
	constexpr uint64 HashSeed = 1469598103934665603ULL;
	constexpr uint64 HashPrime = 1099511628211ULL;
	[[nodiscard]] uint64 hashValue(uint64 hash, uint32 value) noexcept { return (hash ^ value) * HashPrime; }
	[[nodiscard]] uint64 hashMatrix(uint64 hash, const glm::mat4& matrix) noexcept
	{
		const float32* const values = glm::value_ptr(matrix);
		for (uint32 index = 0; index < 16; ++index) hash = hashValue(hash, std::bit_cast<uint32>(values[index]));
		return hash;
	}
	[[nodiscard]] uint64 calculateShadowCasterSignature(const renderer::RenderPreparationResult& prepared) noexcept
	{
		uint64 hash = HashSeed;
		for (const renderer::PreparedInstance& instance : prepared.candidateInstances)
		{
			hash = hashValue(hash, instance.objectID);
			hash = hashMatrix(hash, instance.transform);
			hash = hashValue(hash, std::bit_cast<uint32>(instance.worldBounds.x));
			hash = hashValue(hash, std::bit_cast<uint32>(instance.worldBounds.y));
			hash = hashValue(hash, std::bit_cast<uint32>(instance.worldBounds.z));
			hash = hashValue(hash, std::bit_cast<uint32>(instance.worldBounds.w));
		}
		for (const renderer::RenderBatch& batch : prepared.batches)
		{
			hash = hashValue(hash, batch.vertexArray);
			hash = hashValue(hash, batch.indexCount);
			hash = hashValue(hash, batch.firstIndex);
		}
		return hash;
	}
}

OpenGLRenderer::OpenGLRenderer()
	: lightBufferManager(100)
{
	this->headlessPresentationValidation = pipeline::device::isHeadlessPresentationValidationEnabled();
	glCreateBuffers(1, &frameConstantsUBO);
	glNamedBufferStorage(frameConstantsUBO, sizeof(renderer::GpuFrameConstants), nullptr, GL_DYNAMIC_STORAGE_BIT);
	glCreateBuffers(1, &materialSSBO);
	glNamedBufferStorage(materialSSBO, sizeof(renderer::GpuMaterialRecord) * MaximumRenderItems, nullptr, GL_DYNAMIC_STORAGE_BIT);
	glCreateBuffers(1, &shadowDataSSBO);
	glNamedBufferStorage(shadowDataSSBO, sizeof(renderer::GpuShadowRecord) * MaximumShadowRecords, nullptr, GL_DYNAMIC_STORAGE_BIT);
	glCreateFramebuffers(1, &shadowFramebuffer);
	this->frameResources = std::make_unique<renderer::FrameResourceRing>(sizeof(renderer::PreparedInstance) * MaximumRenderItems, sizeof(renderer::PreparedInstance) * MaximumRenderItems, sizeof(renderer::RenderCommand) * MaximumRenderItems, sizeof(renderer::RenderBatch) * MaximumRenderItems, sizeof(uint32) * (MaximumRenderItems * 4U + 512U));
}

OpenGLRenderer::~OpenGLRenderer()
{
	if (shadowFramebuffer != 0) glDeleteFramebuffers(1, &shadowFramebuffer);
	if (shadowDataSSBO != 0) glDeleteBuffers(1, &shadowDataSSBO);
	if (materialSSBO != 0) glDeleteBuffers(1, &materialSSBO);
	if (frameConstantsUBO != 0) glDeleteBuffers(1, &frameConstantsUBO);
}

uint32 OpenGLRenderer::getDrawCount() const noexcept { return drawCount; }
uint32 OpenGLRenderer::getObjectsDrawn() const noexcept { return objectsDrawn; }

void OpenGLRenderer::render(const StaticMeshObject& worldObject)
{
	if (!collectingFrame)
	{
		sceneCollection.beginFrame(frameNumber);
		collectingFrame = true;
	}
	sceneCollection.submit(worldObject, objectsDrawn);
	++objectsDrawn;
}

void OpenGLRenderer::uploadFrameConstants(const Camera& camera, const Window& window)
{
	const glm::mat4 projection = camera.getProjectionMatrix(window);
	const glm::mat4 view = camera.getViewMatrix();
	const glm::mat4 viewProjection = projection * view;
	const renderer::GpuFrameConstants frame { .projection = projection, .view = view, .viewProjection = viewProjection, .previousViewProjection = previousViewProjection, .inverseViewProjection = glm::inverse(viewProjection), .cameraPositionAndNear = glm::vec4(camera.position, camera.nearPlane), .renderExtentAndFar = glm::vec4(static_cast<float32>(window.getWidth()), static_cast<float32>(window.getHeight()), camera.farPlane, 0.0f), .countsAndFrame = glm::uvec4(0, 0, 0, static_cast<uint32>(frameNumber)) };
	glNamedBufferSubData(frameConstantsUBO, 0, sizeof(renderer::GpuFrameConstants), &frame);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameConstantsUBO);
	previousViewProjection = frame.viewProjection;
}

void OpenGLRenderer::renderScene(const pipeline::shader::GraphicsPipeline& pipeline, const Camera& camera, const Window& window)
{
	if (!collectingFrame)
	{
		return;
	}
	sceneCollection.seal();
	this->uploadFrameConstants(camera, window);
	this->drawCount = 0;
	const renderer::RenderPreparationResult prepared = scenePreparation.prepare(sceneCollection, camera.getProjectionMatrix(window) * camera.getViewMatrix(), 0, 0);
	if (prepared.candidateInstances.size() > MaximumRenderItems || prepared.candidateCommands.size() > MaximumRenderItems)
	{
		throw std::runtime_error("Renderer frame capacity exceeded; increase MaximumRenderItems before submitting more geometry");
	}

	renderer::FrameResources& frame = frameResources->acquire(frameNumber);
	if (!prepared.candidateInstances.empty())
	{
		const uint64 instanceBytes = static_cast<uint64>(prepared.candidateInstances.size() * sizeof(renderer::PreparedInstance));
		std::memcpy(frame.candidateInstances.mappedMemory, prepared.candidateInstances.data(), static_cast<std::size_t>(instanceBytes));
		std::memcpy(frame.visibleInstances.mappedMemory, prepared.candidateInstances.data(), static_cast<std::size_t>(instanceBytes));
	}
	if (!prepared.candidateCommands.empty()) std::memcpy(frame.indirectCommands.mappedMemory, prepared.candidateCommands.data(), prepared.candidateCommands.size() * sizeof(renderer::RenderCommand));
	if (!prepared.batches.empty()) std::memcpy(frame.batchMetadata.mappedMemory, prepared.batches.data(), prepared.batches.size() * sizeof(renderer::RenderBatch));
	if (prepared.materials.size() > MaximumRenderItems) throw std::runtime_error("Renderer material capacity exceeded; increase MaximumRenderItems before submitting more materials");
	std::vector<renderer::GpuMaterialRecord> gpuMaterials;
	gpuMaterials.reserve(prepared.materials.size());
	for (const Material* material : prepared.materials) gpuMaterials.push_back({ .baseColorTexture = material->diffuseTexture.getHandle(), .normalTexture = material->normalTexture.getHandle(), .metallicRoughnessTexture = material->roughnessTexture.getHandle(), .occlusionTexture = material->ambientOcclusionTexture.getHandle(), .emissiveTexture = material->emissiveTexture.getHandle(), .transmissionTexture = 0, .baseColorFactor = glm::vec4(1.0f), .emissiveAndMetallic = glm::vec4(0.0f), .roughnessTransmissionIor = glm::vec4(material->shininess, 0.0f, 1.5f, 0.0f) });
	if (!gpuMaterials.empty()) glNamedBufferSubData(materialSSBO, 0, gpuMaterials.size() * sizeof(renderer::GpuMaterialRecord), gpuMaterials.data());
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, frame.visibleInstances.buffer);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirectCommands.buffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO);

	pipeline.bind();
	for (uint32 batchIndex = 0; batchIndex < prepared.batches.size(); ++batchIndex)
	{
		const renderer::RenderBatch& batch = prepared.batches[batchIndex];
		const StaticMesh* mesh = nullptr;
		for (const renderer::RenderItem& item : sceneCollection.getRenderItems()) if (item.mesh != nullptr && item.mesh->getVAO() == batch.vertexArray) { mesh = item.mesh; break; }
		if (mesh == nullptr) continue;
		const Material& material = mesh->getMaterial();
		const renderer::GpuMaterialRecord gpuMaterial { .baseColorTexture = material.diffuseTexture.getHandle(), .normalTexture = material.normalTexture.getHandle(), .metallicRoughnessTexture = material.roughnessTexture.getHandle(), .occlusionTexture = material.ambientOcclusionTexture.getHandle(), .emissiveTexture = material.emissiveTexture.getHandle(), .transmissionTexture = 0, .baseColorFactor = glm::vec4(1.0f), .emissiveAndMetallic = glm::vec4(0.0f), .roughnessTransmissionIor = glm::vec4(material.shininess, 0.0f, 1.5f, 0.0f) };
		glNamedBufferSubData(materialSSBO, 0, sizeof(renderer::GpuMaterialRecord), &gpuMaterial);
		glBindVertexArray(batch.vertexArray);
		const uintptr_t offset = static_cast<uintptr_t>(batchIndex) * sizeof(renderer::RenderCommand);
		glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(offset), 1, sizeof(renderer::RenderCommand));
		++drawCount;
	}

	frameResources->retire();
	++frameNumber;
	collectingFrame = false;
	sceneCollection.clear();
	objectsDrawn = 0;
}

void OpenGLRenderer::renderScene(const renderer::RenderPassPipelineSet& pipelines, const Camera& camera, const Window& window)
{
	if (!collectingFrame) return;
	const bool cameraCut = hasPreviousCameraState && (glm::distance(camera.position, previousCameraPosition) > 2.0f || glm::dot(glm::normalize(camera.front), glm::normalize(previousCameraFront)) < 0.95f);
	if (cameraCut)
	{
		hierarchicalDepthHistoryValid = false;
		temporalHistoryValid = false;
	}
	sceneCollection.seal();
	this->uploadFrameConstants(camera, window);
	this->drawCount = 0;
	const renderer::RenderPreparationResult prepared = scenePreparation.prepare(sceneCollection, camera.getProjectionMatrix(window) * camera.getViewMatrix(), 0, 0);
	const renderer::RenderPreparationResult shadowPrepared = scenePreparation.prepare(sceneCollection, glm::mat4(1.0f), 0, 0, false);
	const uint64 shadowCasterSignature = calculateShadowCasterSignature(shadowPrepared);
	if (prepared.candidateInstances.size() > MaximumRenderItems || prepared.candidateCommands.size() > MaximumRenderItems || shadowPrepared.candidateInstances.size() > MaximumRenderItems || shadowPrepared.candidateCommands.size() > MaximumRenderItems) throw std::runtime_error("Renderer frame capacity exceeded; increase MaximumRenderItems before submitting more geometry");

	renderer::FrameResources& frame = frameResources->acquire(frameNumber);
	const uint64 instanceBytes = static_cast<uint64>(prepared.candidateInstances.size() * sizeof(renderer::PreparedInstance));
	if (instanceBytes != 0) { std::memcpy(frame.candidateInstances.mappedMemory, prepared.candidateInstances.data(), static_cast<std::size_t>(instanceBytes)); std::memcpy(frame.visibleInstances.mappedMemory, prepared.candidateInstances.data(), static_cast<std::size_t>(instanceBytes)); }
	const uint64 shadowInstanceBytes = static_cast<uint64>(shadowPrepared.candidateInstances.size() * sizeof(renderer::PreparedInstance));
	if (shadowInstanceBytes != 0) std::memcpy(frame.visibleInstances.mappedMemory, shadowPrepared.candidateInstances.data(), static_cast<std::size_t>(shadowInstanceBytes));
	if (!prepared.candidateCommands.empty()) std::memcpy(frame.indirectCommands.mappedMemory, prepared.candidateCommands.data(), prepared.candidateCommands.size() * sizeof(renderer::RenderCommand));
	if (!prepared.batches.empty()) std::memcpy(frame.batchMetadata.mappedMemory, prepared.batches.data(), prepared.batches.size() * sizeof(renderer::RenderBatch));
	// Visibility compaction writes the final per-batch instance counts directly
	// into DrawElementsIndirectCommand. Each command starts empty every frame.
	for (uint32 commandIndex = 0; commandIndex < prepared.candidateCommands.size(); ++commandIndex)
	{
		static_cast<renderer::RenderCommand*>(frame.indirectCommands.mappedMemory)[commandIndex].instanceCount = 0;
	}
	if (prepared.materials.size() > MaximumRenderItems) throw std::runtime_error("Renderer material capacity exceeded; increase MaximumRenderItems before submitting more materials");
	std::vector<renderer::GpuMaterialRecord> gpuMaterials;
	gpuMaterials.reserve(prepared.materials.size());
	for (const Material* material : prepared.materials)
	{
		gpuMaterials.push_back({
			.baseColorTexture = material->diffuseTexture.getHandle(),
			.normalTexture = material->normalTexture.getHandle(),
			.metallicRoughnessTexture = material->roughnessTexture.getHandle(),
			.occlusionTexture = material->ambientOcclusionTexture.getHandle(),
			.emissiveTexture = material->emissiveTexture.getHandle(),
			.transmissionTexture = 0,
			.baseColorFactor = glm::vec4(1.0f),
			.emissiveAndMetallic = glm::vec4(0.0f),
			.roughnessTransmissionIor = glm::vec4(material->shininess, 0.0f, 1.5f, 0.0f)
		});
	}
	if (!gpuMaterials.empty()) glNamedBufferSubData(materialSSBO, 0, static_cast<GLsizeiptr>(gpuMaterials.size() * sizeof(renderer::GpuMaterialRecord)), gpuMaterials.data());
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Materials), materialSSBO);
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	std::vector<renderer::GpuShadowRecord> shadowRecords(MaximumShadowRecords);
	uint32 directionalCascadeCount = 0;
	uint32 spotShadowCount = 0;
	uint32 pointShadowFaceCount = 0;
	const std::vector<DirectionalLightSource>& directionalLights = lightBufferManager.getDirectionalLights();
	if (!directionalLights.empty())
	{
		constexpr std::array<float32, DirectionalShadowCascadeCount> cascadeRadii { 25.0f, 75.0f, 200.0f, 500.0f };
		const glm::vec3 direction = glm::normalize(directionalLights.front().direction);
		for (uint32 cascade = 0; cascade < DirectionalShadowCascadeCount; ++cascade)
		{
			const float32 radius = cascadeRadii[cascade];
			const glm::vec3 center = camera.position + camera.front * (radius * 0.5f);
			const glm::mat4 lightView = glm::lookAt(center - direction * (radius * 2.0f), center, glm::abs(direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f));
			shadowRecords[cascade] = { .viewProjection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.1f, radius * 4.0f) * lightView, .atlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(cascade), 0.0f), .depthBiasAndFilter = glm::vec4(0.0015f, 0.0035f, radius, 0.0f) };
		}
		directionalCascadeCount = DirectionalShadowCascadeCount;
	}
	const std::vector<SpotLightSource>& spotLights = lightBufferManager.getSpotLights();
	spotShadowCount = std::min(static_cast<uint32>(spotLights.size()), MaximumSpotShadowCount);
	for (uint32 spotIndex = 0; spotIndex < spotShadowCount; ++spotIndex)
	{
		const SpotLightSource& light = spotLights[spotIndex];
		const glm::vec3 direction = glm::normalize(light.direction);
		const glm::vec3 up = glm::abs(direction.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		const float32 range = 1000.0f;
		shadowRecords[DirectionalShadowCascadeCount + spotIndex] = { .viewProjection = glm::perspectiveRH_ZO(glm::acos(glm::clamp(light.outerCutOff, -1.0f, 1.0f)) * 2.0f, 1.0f, 0.1f, range) * glm::lookAt(light.position, light.position + direction, up), .atlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(spotIndex), 0.0f), .depthBiasAndFilter = glm::vec4(0.001f, 0.003f, range, 0.0f) };
	}
	const std::vector<PointLightSource>& pointLights = lightBufferManager.getPointLights();
	const uint32 pointShadowCount = std::min(static_cast<uint32>(pointLights.size()), MaximumPointShadowFaceCount / 6U);
	constexpr std::array<glm::vec3, 6> pointFaceDirections { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
	constexpr std::array<glm::vec3, 6> pointFaceUps { glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f) };
	for (uint32 pointIndex = 0; pointIndex < pointShadowCount; ++pointIndex)
	{
		constexpr float32 range = 1000.0f;
		for (uint32 face = 0; face < 6; ++face)
		{
			const uint32 recordIndex = DirectionalShadowCascadeCount + MaximumSpotShadowCount + pointIndex * 6U + face;
			shadowRecords[recordIndex] = { .viewProjection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, range) * glm::lookAt(pointLights[pointIndex].position, pointLights[pointIndex].position + pointFaceDirections[face], pointFaceUps[face]), .atlasScaleBias = glm::vec4(1.0f, 1.0f, static_cast<float32>(pointIndex * 6U + face), 0.0f), .depthBiasAndFilter = glm::vec4(0.002f, 0.004f, range, 0.0f) };
		}
	}
	pointShadowFaceCount = pointShadowCount * 6U;
	glNamedBufferSubData(shadowDataSSBO, 0, static_cast<GLsizeiptr>(shadowRecords.size() * sizeof(renderer::GpuShadowRecord)), shadowRecords.data());

	const renderer::graph::Extent2D extent { window.getWidth(), window.getHeight() };
	renderGraph.beginFrame(extent);
	auto import = [this](string name, const renderer::FrameBufferSlice& slice) { return renderGraph.importBuffer({ .debugName = std::move(name), .sizeInBytes = slice.capacityInBytes, .storageFlags = GL_DYNAMIC_STORAGE_BIT }, slice.buffer); };
	const renderer::HybridDeferredFrameInputs inputs { .extent = extent, .candidateInstances = import("CandidateInstances", frame.candidateInstances), .visibleInstances = import("VisibleInstances", frame.visibleInstances), .indirectCommands = import("IndirectCommands", frame.indirectCommands), .batchMetadata = import("BatchMetadata", frame.batchMetadata), .visibilityScratch = import("VisibilityScratch", frame.visibilityScratch) };

	auto dispatch = [](const pipeline::shader::ComputePipeline& pipeline, renderer::graph::RenderGraphContext& context, renderer::graph::TextureHandle output) { const renderer::graph::Extent2D size = context.getExtent(output); pipeline.bind(); glDispatchCompute((size.width + 7U) / 8U, (size.height + 7U) / 8U, 1); glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT); };
	auto drawBatches = [this, &prepared, &frame](renderer::graph::RenderGraphContext& context, const pipeline::shader::GraphicsPipeline& pipeline, renderer::RenderPassClass requiredPass) {
		context.validateGraphicsPipelineTargets(pipeline);
		pipeline.bind(); glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameConstantsUBO); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, frame.visibleInstances.buffer); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, materialSSBO); glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirectCommands.buffer);
		for (uint32 batchIndex = 0; batchIndex < prepared.batches.size(); ++batchIndex) { const renderer::RenderBatch& batch = prepared.batches[batchIndex]; if (batch.passClass != requiredPass) continue; if (batch.vertexDescriptor == nullptr) throw std::logic_error("RenderBatch is missing its VertexDescriptor"); pipeline.validateVertexDescriptor(*batch.vertexDescriptor); glBindVertexArray(batch.vertexArray); const uintptr_t offset = static_cast<uintptr_t>(batchIndex) * sizeof(renderer::RenderCommand); glMultiDrawElementsIndirect(pipeline.getGLTopology(), GL_UNSIGNED_INT, reinterpret_cast<const void*>(offset), 1, sizeof(renderer::RenderCommand)); ++drawCount; }
	};

	const auto renderShadowLayers = [this, &pipelines, &frame, &shadowPrepared, &shadowRecords, shadowCasterSignature](GLuint texture, uint32 firstLayer, uint32 layerCount, uint32 firstRecord, renderer::graph::Extent2D shadowExtent) {
		if (layerCount == 0) return;
		pipelines.shadowDepth.bind();
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Instances), frame.visibleInstances.buffer);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ShadowData), shadowDataSSBO);
		glNamedFramebufferDrawBuffer(shadowFramebuffer, GL_NONE);
		glNamedFramebufferReadBuffer(shadowFramebuffer, GL_NONE);
		auto* const shadowVisibleInstances = static_cast<renderer::PreparedInstance*>(frame.visibleInstances.mappedMemory);
		for (uint32 layer = 0; layer < layerCount; ++layer)
		{
			const renderer::GpuShadowRecord& shadowView = shadowRecords.at(firstRecord + layer);
			uint64 shadowSignature = hashMatrix(hashValue(shadowCasterSignature, static_cast<uint32>(texture)), shadowView.viewProjection);
			shadowSignature = hashValue(shadowSignature, firstLayer + layer);
			const uint32 cacheKey = firstRecord + layer;
			ShadowLayerCacheEntry& cachedLayer = this->shadowLayerCache[cacheKey];
			if (cachedLayer.valid && cachedLayer.texture == texture && cachedLayer.layer == firstLayer + layer && cachedLayer.signature == shadowSignature) continue;
			glNamedFramebufferTextureLayer(shadowFramebuffer, GL_DEPTH_ATTACHMENT, texture, 0, static_cast<GLint>(firstLayer + layer));
			if (glCheckNamedFramebufferStatus(shadowFramebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("Shadow framebuffer is incomplete");
			glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer);
			glViewport(0, 0, static_cast<GLsizei>(shadowExtent.width), static_cast<GLsizei>(shadowExtent.height));
			glClearDepth(1.0); glClear(GL_DEPTH_BUFFER_BIT);
			pipelines.shadowDepth.setVertexUniformUInt("shadowViewIndex", firstRecord + layer);
			for (const renderer::RenderBatch& batch : shadowPrepared.batches)
			{
				if (batch.passClass == renderer::RenderPassClass::Transparency) continue;
				if (batch.vertexDescriptor == nullptr) throw std::logic_error("RenderBatch is missing its VertexDescriptor");
				pipelines.shadowDepth.validateVertexDescriptor(*batch.vertexDescriptor);
				uint32 visibleCount = 0;
				for (uint32 candidateOffset = 0; candidateOffset < batch.candidateCount; ++candidateOffset)
				{
					const renderer::PreparedInstance& candidate = shadowPrepared.candidateInstances[batch.firstCandidate + candidateOffset];
					if (!renderer::ScenePreparation::intersectsFrustum(candidate.worldBounds, shadowView.viewProjection)) continue;
					shadowVisibleInstances[batch.firstCandidate + visibleCount] = candidate;
					++visibleCount;
				}
				if (visibleCount == 0) continue;
				glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
				glBindVertexArray(batch.vertexArray);
				glDrawElementsInstancedBaseVertexBaseInstance(pipelines.shadowDepth.getGLTopology(), static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, reinterpret_cast<const void*>(static_cast<uintptr_t>(batch.firstIndex) * sizeof(uint32)), static_cast<GLsizei>(visibleCount), batch.baseVertex, batch.firstCandidate);
			}
			cachedLayer = { .texture = texture, .layer = firstLayer + layer, .signature = shadowSignature, .valid = true };
		}
	};

	const renderer::HybridDeferredPassCallbacks callbacks {
		.directionalShadows = [&renderShadowLayers, directionalCascadeCount](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { renderShadowLayers(context.getTexture(resources.directionalShadowAtlas), 0, directionalCascadeCount, 0, context.getExtent(resources.directionalShadowAtlas)); },
		.spotShadows = [&renderShadowLayers, spotShadowCount](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { renderShadowLayers(context.getTexture(resources.spotShadowAtlas), 0, spotShadowCount, DirectionalShadowCascadeCount, context.getExtent(resources.spotShadowAtlas)); },
		.pointShadows = [&renderShadowLayers, pointShadowFaceCount](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { renderShadowLayers(context.getTexture(resources.pointShadowArray), 0, pointShadowFaceCount, DirectionalShadowCascadeCount + MaximumSpotShadowCount, context.getExtent(resources.pointShadowArray)); },
		.mainVisibility = [this, &pipelines, &frame, &prepared](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) {
			const uint32 zero = 0;
			const uint32 candidateCount = static_cast<uint32>(prepared.candidateInstances.size());
			const uint32 batchCount = static_cast<uint32>(prepared.batches.size());
			const uint32 pyramidMipCount = static_cast<uint32>(std::bit_width(std::max(context.getExtent(resources.hierarchicalDepth).width, context.getExtent(resources.hierarchicalDepth).height)));
			const bool historyMatchesExtent = hierarchicalDepthHistoryValid && hierarchicalDepthHistoryExtent.width == context.getExtent(resources.hierarchicalDepth).width && hierarchicalDepthHistoryExtent.height == context.getExtent(resources.hierarchicalDepth).height;
			glClearNamedBufferData(frame.visibilityScratch.buffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Candidates), frame.candidateInstances.buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::Instances), frame.visibleInstances.buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::VisibilityScratch), frame.visibilityScratch.buffer);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::IndirectCommands), frame.indirectCommands.buffer);
			glBindTextureUnit(0, context.getTexture(resources.hierarchicalDepth));
			if (candidateCount == 0 || batchCount == 0) return;
			pipelines.visibilityCull.setUniformUInt("candidateCount", candidateCount);
			pipelines.visibilityCull.setUniformUInt("pyramidMipCount", pyramidMipCount);
			pipelines.visibilityCull.setUniformUInt("historyValid", historyMatchesExtent ? 1U : 0U);
			pipelines.visibilityCull.setUniformUInt("scratchCapacity", MaximumRenderItems);
			pipelines.visibilityCull.bind();
			glDispatchCompute((candidateCount + 63U) / 64U, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
			const uint32 visibilityScanBlockCount = (batchCount + 255U) / 256U;
			pipelines.visibilityPrefixScan.setUniformUInt("batchCount", batchCount);
			pipelines.visibilityPrefixScan.setUniformUInt("scratchCapacity", MaximumRenderItems);
			pipelines.visibilityPrefixScan.bind();
			glDispatchCompute(visibilityScanBlockCount, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			pipelines.visibilityBlockPrefixScan.setUniformUInt("blockCount", visibilityScanBlockCount);
			pipelines.visibilityBlockPrefixScan.setUniformUInt("scratchCapacity", MaximumRenderItems);
			pipelines.visibilityBlockPrefixScan.bind();
			glDispatchCompute(1, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			pipelines.visibilityFinalize.setUniformUInt("batchCount", batchCount);
			pipelines.visibilityFinalize.setUniformUInt("scratchCapacity", MaximumRenderItems);
			pipelines.visibilityFinalize.bind();
			glDispatchCompute(visibilityScanBlockCount, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
			pipelines.visibilityScatter.setUniformUInt("candidateCount", candidateCount);
			pipelines.visibilityScatter.setUniformUInt("scratchCapacity", MaximumRenderItems);
			pipelines.visibilityScatter.bind();
			glDispatchCompute((candidateCount + 63U) / 64U, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
		},
		.depthPrepass = [&pipelines, &drawBatches](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources&) { drawBatches(context, pipelines.depthPrepass, renderer::RenderPassClass::GBuffer); },
		.hierarchicalDepth = [&pipelines, extent](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) {
			const GLuint depthTexture = context.getTexture(resources.depth);
			const GLuint pyramidTexture = context.getTexture(resources.hierarchicalDepth);
			const uint32 mipCount = static_cast<uint32>(std::bit_width(std::max(extent.width, extent.height)));
			pipelines.hierarchicalDepth.bind();
			for (uint32 mip = 0; mip < mipCount; ++mip)
			{
				const uint32 outputWidth = std::max(1U, extent.width >> mip);
				const uint32 outputHeight = std::max(1U, extent.height >> mip);
				const uint32 sourceWidth = mip == 0 ? extent.width : std::max(1U, extent.width >> (mip - 1U));
				const uint32 sourceHeight = mip == 0 ? extent.height : std::max(1U, extent.height >> (mip - 1U));
				glBindTextureUnit(0, mip == 0 ? depthTexture : pyramidTexture);
				glBindImageTexture(0, pyramidTexture, static_cast<GLint>(mip), GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
				pipelines.hierarchicalDepth.setUniformUInt2("sourceExtent", sourceWidth, sourceHeight);
				pipelines.hierarchicalDepth.setUniformUInt("sourceMip", mip == 0 ? 0U : mip - 1U);
				pipelines.hierarchicalDepth.setUniformUInt("sourceScale", mip == 0 ? 1U : 2U);
				glDispatchCompute((outputWidth + 7U) / 8U, (outputHeight + 7U) / 8U, 1);
				glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			}
		},
		.gbuffer = [this, &pipelines, &drawBatches](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { drawBatches(context, pipelines.gbuffer, renderer::RenderPassClass::GBuffer); if (this->headlessPresentationValidation && !this->presentationValidated) { this->validateHeadlessDepthCoverage(context.getTexture(resources.depth), context.getExtent(resources.depth)); this->validateHeadlessColorCoverage(context.getTexture(resources.gbufferBaseColor), context.getExtent(resources.gbufferBaseColor), "G-buffer base color"); this->validateHeadlessColorCoverage(context.getTexture(resources.gbufferNormalRoughness), context.getExtent(resources.gbufferNormalRoughness), "G-buffer normal"); } },
		.clusteredLights = [this, &pipelines](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { constexpr uint32 clusterCount = 32U * 18U * 24U; glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants), frameConstantsUBO); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterHeaders), context.getBuffer(resources.clusterHeaders)); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterIndices), context.getBuffer(resources.clusterIndices)); pipelines.clusteredLights.setUniformUInt("lightCount", static_cast<uint32>(this->lightBufferManager.getTotalLightSourceCount())); pipelines.clusteredLights.setUniformUInt("clusterCount", clusterCount); pipelines.clusteredLights.bind(); glDispatchCompute((clusterCount + 63U) / 64U, 1, 1); glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); },
		.deferredLighting = [this, &pipelines, &dispatch](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { constexpr uint32 clusterCount = 32U * 18U * 24U; glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(renderer::RendererBinding::FrameConstants), frameConstantsUBO); glBindTextureUnit(0, context.getTexture(resources.gbufferBaseColor)); glBindTextureUnit(1, context.getTexture(resources.gbufferNormalRoughness)); glBindTextureUnit(2, context.getTexture(resources.gbufferMaterial)); glBindTextureUnit(3, context.getTexture(resources.depth)); glBindTextureUnit(4, context.getTexture(resources.directionalShadowAtlas)); glBindTextureUnit(5, context.getTexture(resources.spotShadowAtlas)); glBindTextureUnit(6, context.getTexture(resources.pointShadowArray)); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ShadowData), shadowDataSSBO); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterHeaders), context.getBuffer(resources.clusterHeaders)); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(renderer::RendererBinding::ClusterIndices), context.getBuffer(resources.clusterIndices)); pipelines.deferredLighting.setUniformUInt("lightCount", static_cast<uint32>(this->lightBufferManager.getTotalLightSourceCount())); pipelines.deferredLighting.setUniformUInt("clusterCount", clusterCount); glBindImageTexture(0, context.getTexture(resources.hdrLighting), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F); dispatch(pipelines.deferredLighting, context, resources.hdrLighting); if (this->headlessPresentationValidation) this->validateHeadlessColorCoverage(context.getTexture(resources.hdrLighting), context.getExtent(resources.hdrLighting), "deferred HDR"); },
		.weightedOIT = [&pipelines, &drawBatches](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources&) { drawBatches(context, pipelines.transparentOIT, renderer::RenderPassClass::Transparency); },
		.oitComposition = [&pipelines, &dispatch](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { glBindTextureUnit(0, context.getTexture(resources.hdrLighting)); glBindTextureUnit(1, context.getTexture(resources.transparencyAccumulation)); glBindTextureUnit(2, context.getTexture(resources.transparencyRevealage)); glBindImageTexture(0, context.getTexture(resources.compositedHDR), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F); dispatch(pipelines.oitComposition, context, resources.compositedHDR); },
		.temporalAA = [this, &pipelines, &dispatch](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { const renderer::graph::Extent2D extent = context.getExtent(resources.taaResolved); const bool historyMatchesExtent = temporalHistoryValid && temporalHistoryExtent.width == extent.width && temporalHistoryExtent.height == extent.height; glBindTextureUnit(0, context.getTexture(resources.compositedHDR)); glBindTextureUnit(1, context.getTexture(resources.taaHistory)); glBindTextureUnit(2, context.getTexture(resources.velocity)); pipelines.temporalAA.setUniformUInt("historyValid", historyMatchesExtent ? 1U : 0U); glBindImageTexture(0, context.getTexture(resources.taaHistory), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F); glBindImageTexture(1, context.getTexture(resources.taaResolved), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F); dispatch(pipelines.temporalAA, context, resources.taaResolved); if (this->headlessPresentationValidation) this->validateHeadlessColorCoverage(context.getTexture(resources.taaResolved), extent, "TAA resolve"); },
		.exposureAndBloom = [this, &pipelines](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { const renderer::graph::Extent2D extent = context.getExtent(resources.bloom); const uint32 mipCount = static_cast<uint32>(std::bit_width(std::max(extent.width, extent.height))); const GLuint bloomTexture = context.getTexture(resources.bloom); pipelines.autoExposure.bind(); glBindTextureUnit(0, context.getTexture(resources.taaResolved)); glBindImageTexture(0, context.getTexture(resources.exposure), 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F); pipelines.autoExposure.setUniformUInt("historyValid", exposureHistoryValid ? 1U : 0U); glDispatchCompute(1, 1, 1); glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT); pipelines.bloom.bind(); for (uint32 mip = 0; mip < mipCount; ++mip) { const uint32 width = std::max(1U, extent.width >> mip); const uint32 height = std::max(1U, extent.height >> mip); glBindTextureUnit(0, mip == 0 ? context.getTexture(resources.taaResolved) : bloomTexture); glBindImageTexture(0, bloomTexture, static_cast<GLint>(mip), GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F); pipelines.bloom.setUniformUInt("sourceMip", mip == 0 ? 0U : mip - 1U); pipelines.bloom.setUniformUInt("operation", 0U); glDispatchCompute((width + 7U) / 8U, (height + 7U) / 8U, 1); glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT); } for (uint32 mip = mipCount - 1U; mip > 0U; --mip) { const uint32 outputMip = mip - 1U; const uint32 width = std::max(1U, extent.width >> outputMip); const uint32 height = std::max(1U, extent.height >> outputMip); glBindTextureUnit(0, bloomTexture); glBindImageTexture(0, bloomTexture, static_cast<GLint>(outputMip), GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F); pipelines.bloom.setUniformUInt("sourceMip", mip); pipelines.bloom.setUniformUInt("operation", 1U); glDispatchCompute((width + 7U) / 8U, (height + 7U) / 8U, 1); glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT); } },
		.toneMapAndPresent = [&pipelines](renderer::graph::RenderGraphContext& context, const renderer::HybridDeferredFrameResources& resources) { glBindFramebuffer(GL_FRAMEBUFFER, 0); glBindTextureUnit(0, context.getTexture(resources.taaResolved)); glBindTextureUnit(1, context.getTexture(resources.bloom)); glBindTextureUnit(2, context.getTexture(resources.exposure)); pipelines.toneMap.bind(); glDrawArrays(GL_TRIANGLES, 0, 3); }
	};
	(void)hybridFrameGraph.build(renderGraph, inputs, callbacks);
	renderGraph.compile();
	renderGraph.execute();
	this->validateHeadlessPresentation(window);
	hierarchicalDepthHistoryExtent = extent;
	hierarchicalDepthHistoryValid = true;
	temporalHistoryExtent = extent;
	temporalHistoryValid = true;
	exposureHistoryValid = true;
	previousCameraPosition = camera.position;
	previousCameraFront = camera.front;
	hasPreviousCameraState = true;
	frameResources->retire(); ++frameNumber; collectingFrame = false; sceneCollection.clear(); objectsDrawn = 0;
}

void OpenGLRenderer::enableCulling() const { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW); }
void OpenGLRenderer::disableCulling() const { glDisable(GL_CULL_FACE); }

void OpenGLRenderer::validateHeadlessDepthCoverage(GLuint depthTexture, renderer::graph::Extent2D extent) const
{
	if (this->presentationValidated) return;
	const uint64 pixelCount = static_cast<uint64>(extent.width) * static_cast<uint64>(extent.height);
	if (pixelCount == 0 || pixelCount > static_cast<uint64>(std::numeric_limits<std::size_t>::max() / sizeof(float32))) throw std::runtime_error("Headless depth validation readback exceeds addressable memory");
	std::vector<float32> depthValues(static_cast<std::size_t>(pixelCount));
	glGetTextureSubImage(depthTexture, 0, 0, 0, 0, static_cast<GLsizei>(extent.width), static_cast<GLsizei>(extent.height), 1, GL_DEPTH_COMPONENT, GL_FLOAT, static_cast<GLsizei>(pixelCount * sizeof(float32)), depthValues.data());
	pipeline::device::throwPendingOpenGLErrors("Headless G-buffer depth validation");
	uint64 coveredPixelCount = 0;
	for (const float32 depth : depthValues) if (depth > 0.0001f) ++coveredPixelCount;
	if (coveredPixelCount == 0) throw std::runtime_error("Headless validation found no opaque depth coverage after the G-buffer pass");
	std::cerr << "Headless G-buffer validation: " << coveredPixelCount << " covered pixels\n";
}

void OpenGLRenderer::validateHeadlessColorCoverage(GLuint colorTexture, renderer::graph::Extent2D extent, string_view stage) const
{
	if (this->presentationValidated) return;
	const uint64 pixelCount = static_cast<uint64>(extent.width) * static_cast<uint64>(extent.height);
	if (pixelCount == 0 || pixelCount > static_cast<uint64>(std::numeric_limits<std::size_t>::max() / (sizeof(float32) * 4U))) throw std::runtime_error("Headless color validation readback exceeds addressable memory");
	const uint64 componentCount = pixelCount * 4U;
	std::vector<float32> values(static_cast<std::size_t>(componentCount));
	glGetTextureSubImage(colorTexture, 0, 0, 0, 0, static_cast<GLsizei>(extent.width), static_cast<GLsizei>(extent.height), 1, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(componentCount * sizeof(float32)), values.data());
	pipeline::device::throwPendingOpenGLErrors("Headless " + std::string(stage) + " validation");
	uint64 litPixelCount = 0;
	for (uint64 offset = 0; offset < componentCount; offset += 4U)
	{
		if (values[static_cast<std::size_t>(offset)] > 0.001f || values[static_cast<std::size_t>(offset + 1U)] > 0.001f || values[static_cast<std::size_t>(offset + 2U)] > 0.001f) ++litPixelCount;
	}
	if (litPixelCount == 0) throw std::runtime_error("Headless validation found no visible color in " + std::string(stage));
	std::cerr << "Headless " << stage << " validation: " << litPixelCount << " lit pixels\n";
}

void OpenGLRenderer::validateHeadlessPresentation(const Window& window)
{
	if (!this->headlessPresentationValidation || this->presentationValidated) return;
	const uint32 width = window.getWidth();
	const uint32 height = window.getHeight();
	if (width == 0 || height == 0) throw std::runtime_error("Headless presentation validation requires a non-zero window extent");
	const uint64 byteCount = static_cast<uint64>(width) * static_cast<uint64>(height) * 4U;
	if (byteCount > static_cast<uint64>(std::numeric_limits<std::size_t>::max())) throw std::runtime_error("Headless presentation validation readback exceeds addressable memory");

	std::vector<uint8> pixels(static_cast<std::size_t>(byteCount));
	GLint previousReadBuffer = GL_BACK;
	GLint previousPackAlignment = 4;
	glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	glReadBuffer(previousReadBuffer);
	pipeline::device::throwPendingOpenGLErrors("Headless presentation validation");

	uint64 litPixelCount = 0;
	for (uint64 offset = 0; offset < byteCount; offset += 4U)
	{
		if (static_cast<uint32>(pixels[static_cast<std::size_t>(offset)]) + static_cast<uint32>(pixels[static_cast<std::size_t>(offset + 1U)]) + static_cast<uint32>(pixels[static_cast<std::size_t>(offset + 2U)]) > 3U) ++litPixelCount;
	}
	if (litPixelCount == 0) throw std::runtime_error("Headless presentation validation found an all-black presented frame");
	std::cerr << "Headless presentation validation: " << litPixelCount << " non-black pixels\n";
	this->presentationValidated = true;
	// In the opt-in hidden validation mode, one successfully presented frame is
	// the test result. End the application so automated validation cannot mask
	// a failure behind an externally forced timeout.
	window.closeWindow();
}
