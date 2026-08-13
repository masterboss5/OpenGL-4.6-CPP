# Repository Guidelines

## Project Structure & Module Organization

`OpenFrame/` is the application tree. Its C++ sources live in `OpenFrame/Source/`, organized by responsibility: `core/` contains application, window, input, and layers; `pipeline/` owns GPU buffers, shaders, textures, render graphs, and rendering; `scene/` defines cameras, objects, and lights; `resource/` imports and manages assets; `runtime/` owns packaged-project execution; `editor/` owns project authoring; and `component/` contains object components. Keep declarations in `.h` files beside their `.cpp` implementations.

Runtime shaders and application assets are in `OpenFrame/Shaders/` and `OpenFrame/Assets/`. Visual Studio project definitions live in `OpenFrame/Build/VisualStudio/`. `ThirdParty/` contains the build-used portions of vendored GLFW, GLEW, GLM, STB, Assimp, Dear ImGui, nlohmann/json, and Zstandard distributions; retain each dependency's license notice. The root `OpenFrame.sln` contains the OpenFrameEngine, OpenFrameTools, OpenFrameEditor, OpenFrameGame, and OpenFrameValidation targets. `Projects/Baseplate/` is the bundled sample project. Build products belong only under `Artifacts/`.

## Build, Test, and Development Commands

Use Visual Studio 2022 with the v143 toolset and Windows SDK 10.0.26100.0. Select **Debug | x64** for local development, then build `OpenFrame.sln` (`Ctrl+Shift+B`) and run OpenFrameEditor with `F5`. The editor and packaged game accept no command-line project arguments; project creation and opening are GUI workflows. From a Developer PowerShell, the equivalent developer build is:

```powershell
msbuild OpenFrame.sln /p:Configuration=Debug /p:Platform=x64
```

Use `Release | x64` for an optimized build. The project targets C++20 and links GLFW, GLEW, OpenGL, Assimp, Dear ImGui, nlohmann/json, and Zstandard. Run `OpenFrameValidation.exe --all` from `Artifacts/<Configuration>/` for deterministic validation, then perform relevant editor/game GUI smoke tests.

## Coding Style & Naming Conventions

Format project-owned C++ with the root `.clang-format`: tabs at width 4, Allman braces, and a 140-column limit. Pass the files reported by `rg --files OpenFrame/Source -g "*.h" -g "*.cpp"` to `clang-format -i`. Keep `#pragma once` include guards and order includes with the local header first.

Use PascalCase for classes, structs, unions, concepts, functions, methods, constants, enumerators, parameters, fields, and local variables. Keep namespaces lowercase and retain the lowercase fundamental aliases in `OpenFrame/Source/types.h`. Preserve established initialisms (`OpenGLRenderer`, `GPUBuffer`, `CPUData`, `ObjectID`). When a parameter and field have the same name, qualify the field explicitly, for example `this->BufferSize = BufferSize`.

Never introduce raw engine numeric types when an alias exists in `OpenFrame/Source/types.h`. Exact native, OpenGL, callback, entry-point, and textual boundary signatures may retain their required types. Put reusable semantic template constraints in `OpenFrame/Source/concepts.h` whenever doing so does not create a dependency cycle. Mark concrete leaf classes `final`, use `override` on every override, and apply standard attributes only where their semantics improve correctness.

Keep GPU-resource ownership explicit and pair new headers/sources in the appropriate module. Add shader files with descriptive PascalCase names such as `LightingFragment.glsl`.

## Testing & Review Guidelines

For rendering or asset changes, test the relevant scene manually and verify that shader and asset paths resolve from the working directory. Include reproduction/verification notes and screenshots for visible rendering changes. Keep commits focused and use concise imperative subjects, for example `Add spot light attenuation`. In pull requests, describe the behavior change, configuration tested, affected assets, and any follow-up work.

Place code in the module that owns its responsibility. Keep resource-specific behavior with that resource, shared OpenGL runtime/context/error handling in `OpenFrame/Source/pipeline/device/`, and reusable pipeline abstractions in their dedicated pipeline module. Do not place cross-cutting helpers in a feature or resource `.cpp` merely because that is where they are first needed; create or extend the appropriate shared module and use it from callers.

## Production Engine Standard

This is not a hobbyist engine. Implement C++ and OpenGL systems to a production standard: handle errors, ownership, lifetime, invalid states, and relevant edge cases explicitly. Do not settle for thin or incomplete abstractions over OpenGL features; expose and use the capabilities needed for a robust engine implementation. Target OpenGL 4.6 exclusively, with no compatibility requirement for older versions. Prefer the newest applicable core APIs and techniques, including direct state access and other modern OpenGL functionality, and configure or validate the runtime context accordingly when the task requires it.

## Agent-Specific Instructions

This repository is not for broad “vibe coding” or codebase-wide rewrites. Work only on specific, orchestrated tasks such as implementing a named function, completing a class, or adding a narrowly defined code block. Before editing, obtain all relevant context and the exact requested outcome. Do not infer missing requirements, change adjacent code “for consistency,” or expand the scope. When the task, expected behavior, affected files, constraints, or acceptance criteria are unclear, stop and ask for the needed information before making changes.

Treat the codebase primarily as an extension point. Prefer additive, self-contained implementations in clearly appropriate files so the original authors can easily identify what was introduced. When existing code must change to complete the task, keep the edit local and directly related to the requested behavior. Avoid broad refactors, formatting churn, renames, relocations, or incidental changes that spread beyond the task or make the original implementation difficult to follow.

Follow the engine’s current architecture when extending it. Preserve the established object model and asset-management systems, and integrate new features through their existing patterns where applicable. If a materially better architecture would suit the task, explain the option and its trade-offs to the user before changing the established approach.

Windows 10/11 is the only target, disregard any other platform. Never ever prefix or label anything with the platform, its assumed to be Windows and OpenGL always.

Always use multi threaded, most performant option.

Always go big, when we implement an feature, always plan for adding the full feature and AAA like capabaility.

Never ever use raw types such as int, char, long or etc. Always use the ones aliased in OpenFrame/Source/types.h.

This engine is not a hobbyist level, we are going to push for max graphics such as full on PBR materials and most demanding lights. We are not just building simple gl wrappers, we are defining our own engine systems.

Never ever do "first we will implement this and add XYZ later", unacceptable, we get it all done now and go for the best possible.
