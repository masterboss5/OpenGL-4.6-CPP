#include "src/component/object/CObjectTransformComponent.h"
#include "src/core/app/Application.h"
#include "src/core/layers/RenderLayer.h"
#include "src/scene/Object.h"
#include "src/util/logger.h"
#include "src/util/memory/TypedPoolAllocator.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

#define FAR_PLANE 600.0f
#define NEAR_PLANE 0.1f

#define RESOLUTION_1080P(spec)                                                                                                             \
	spec.width = 1920;                                                                                                                     \
	spec.height = 1080;

#define RESOLUTION_1440P(spec)                                                                                                             \
	spec.width = 2560;                                                                                                                     \
	spec.height = 1440;

#define RESOLUTION_4K(spec)                                                                                                                \
	spec.width = 3840;                                                                                                                     \
	spec.height = 2160;

// #define LOG
int main(const int ArgumentCount, char *Arguments[])
{
	try
	{
		LOG_INFO("[Starting application]");
		bool ValidateRenderCoreOnly = false;
		for (int ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
		{
			const std::string_view Argument = Arguments[ArgumentIndex];
			if (Argument == "--validate-render-core")
				ValidateRenderCoreOnly = true;
			else
				throw std::invalid_argument("Unknown command-line argument: " + std::string(Argument));
		}

#ifndef DEBUG

		core::WindowSpecification WindowSpecification;
		WindowSpecification.Title = "OpenGL 4.6";
		WindowSpecification.Extent = {1920, 1080};

		core::Application App = core::Application(core::ApplicationSpecification{.Window = std::move(WindowSpecification),
																				 .DeterministicRenderValidation = ValidateRenderCoreOnly});
		if (ValidateRenderCoreOnly)
			return EXIT_SUCCESS;
		App.PushLayer<core::RenderLayer>();
		App.Main();

#else
		// resource::AssetManager resourceManager {};
		// resourceManager.getModel("objects/helmet/DamagedHelmet.gltf");
#endif

		return EXIT_SUCCESS;
	}
	catch (const std::exception &Exception)
	{
		std::cerr << "Fatal engine exception: " << Exception.what() << '\n';
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Fatal engine exception: unknown non-standard exception\n";
		return EXIT_FAILURE;
	}
}
