# Repository Guidelines

## Project Structure & Module Organization

`OpenGL/` is the application project. Its C++ sources live in `OpenGL/src/`, organized by responsibility: `core/` contains application, window, input, and layers; `pipeline/` owns GPU buffers, shaders, and textures; `renderer/` implements rendering; `scene/` defines cameras, objects, and lights; `resource/` imports and manages assets; and `component/` contains object components. Keep declarations in `.h` files beside their `.cpp` implementations.

Runtime assets are in `OpenGL/shader/`, `OpenGL/objects/`, and `OpenGL/image/`. `Dependencies/` contains vendored GLFW, GLEW, GLM, STB, TinyObjLoader, and Assimp files; do not edit or replace these as part of normal feature work. The root `OpenGL.sln` and `OpenGL.vcxproj` are the Visual Studio build entry points. Some legacy duplicate asset folders exist at the repository root; use the copies under `OpenGL/` for new code.

## Build, Test, and Development Commands

Use Visual Studio 2022 with the v143 toolset and Windows SDK 10.0. Select **Debug | x64** for local development, then build `OpenGL.sln` (`Ctrl+Shift+B`) and run with `F5`. From a Developer PowerShell, the equivalent is:

```powershell
msbuild OpenGL.sln /p:Configuration=Debug /p:Platform=x64
```

Use `Release | x64` for an optimized build. The project targets C++20 and links GLFW, static GLEW, OpenGL, and Assimp. No automated test project is currently configured; validate changes by building the affected configuration and running the application.

## Coding Style & Naming Conventions

Follow the surrounding C++ style: four-space indentation, braces on their own line, `#pragma once` include guards, and includes ordered with the local header first. Classes use PascalCase (`OpenGLRenderer`); functions and fields use lower camel case (`getViewMatrix`, `windowSpecification`); namespaces are lowercase. Keep GPU-resource ownership explicit and pair new headers/sources in the appropriate module. Add shader files with descriptive PascalCase names such as `LightingFragment.glsl`.

## Testing & Review Guidelines

For rendering or asset changes, test the relevant scene manually and verify that shader and asset paths resolve from the working directory. Include reproduction/verification notes and screenshots for visible rendering changes. Keep commits focused and use concise imperative subjects, for example `Add spot light attenuation`. In pull requests, describe the behavior change, configuration tested, affected assets, and any follow-up work.

## Agent-Specific Instructions

This repository is not for broad “vibe coding” or codebase-wide rewrites. Work only on specific, orchestrated tasks such as implementing a named function, completing a class, or adding a narrowly defined code block. Before editing, obtain all relevant context and the exact requested outcome. Do not infer missing requirements, change adjacent code “for consistency,” or expand the scope. When the task, expected behavior, affected files, constraints, or acceptance criteria are unclear, stop and ask for the needed information before making changes.

Treat the codebase primarily as an extension point. Prefer additive, self-contained implementations in clearly appropriate files so the original authors can easily identify what was introduced. When existing code must change to complete the task, keep the edit local and directly related to the requested behavior. Avoid broad refactors, formatting churn, renames, relocations, or incidental changes that spread beyond the task or make the original implementation difficult to follow.

Follow the engine’s current architecture when extending it. Preserve the established object model and asset-management systems, and integrate new features through their existing patterns where applicable. If a materially better architecture would suit the task, explain the option and its trade-offs to the user before changing the established approach.
