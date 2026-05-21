#include "src/core/input/Window.h"
#include "src/core/app/Application.h"
#include "src/core/layers/RenderLayer.h"


#include "src/util/logger.h"
#include "src/scene/Object.h"
#include "src/component/object/CObjectTransformComponent.h"

#define FAR_PLANE 600.0f
#define NEAR_PLANE 0.1f

#define RESOLUTION_1080P(spec) \
    spec.width = 1920;         \
    spec.height = 1080;

#define RESOLUTION_1440P(spec) \
    spec.width = 2560;         \
    spec.height = 1440;

#define RESOLUTION_4K(spec)    \
    spec.width = 3840;         \
    spec.height = 2160;


//#define LOG
int main()
{
	LOG_INFO("[Starting application]");

#ifndef DEBUG
	core::WindowSpecification windowSpecification;
	windowSpecification.windowTitle = "OpenGL 4.6";
	windowSpecification.width = 1920;
	windowSpecification.height = 1080;

	core::Application app = core::Application(windowSpecification);
	app.pushLayer<core::RenderLayer>();
	app.main();



#else
	//resource::AssetManager resourceManager {};
	//resourceManager.getModel("objects/helmet/DamagedHelmet.gltf");
#endif



	return 0;
}