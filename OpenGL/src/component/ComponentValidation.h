#pragma once
#include "src/meta.h"
#include "src/component/object/CObjectTransformComponent.h"
//#include "src/component/object/CObjectMeshComponent.h"

// NOW these are safe to instantiate
static_assert(TypeListAllFinal<components::ComponentTypeList>::value, "");

static_assert(TypeListAllNotAbstract<components::ComponentTypeList>::value, "");

static_assert(TypeListAllDerivedFrom<components::CObjectComponent,
    components::ComponentTypeList>::value, "");

static_assert(TypeListUnique<components::ComponentTypeList>::value,
	"ComponentTypeList contains duplicate types");