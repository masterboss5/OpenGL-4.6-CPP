#pragma once

#if defined(ENGINE_STATIC)
#define ENGINE_API
#elif defined(ENGINE_BUILD)
#define ENGINE_API __declspec(dllexport)
#else
#define ENGINE_API __declspec(dllimport)
#endif

#define ENGINE_LOCAL
