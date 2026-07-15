# Repository Guidelines

## Project Structure & Asset Organization

This repository contains reusable 3D assets for OpenGL projects; it has no application source tree. Keep each model with the files it references:

- Root-level assets include `scene.gltf`/`scene.bin`, `backpack.obj`/`backpack.mtl`, and their JPEG/PNG maps.
- `helmet/` contains the Damaged Helmet glTF asset, its binary buffer, PBR maps, and `attribution.txt`.
- `gun/` contains the Cerberus FBX/TBScene asset, source textures in `Textures/`, raw source maps in `Textures/Raw/`, and `Disclaimer.txt`.
- `textures/` holds copies of material maps used by the root scene.

Preserve relative paths and exact filename casing: material files refer to textures by name (for example, `backpack.mtl` references `diffuse.jpg`).

## Build, Test, and Development Commands

There is no build, runtime, package manager, or automated test command in this repository. Validate changed assets in the intended OpenGL viewer/engine. For glTF assets, also open the `.gltf` file in a glTF-compatible validator or viewer and confirm that its paired `.bin` file and texture maps load without missing-resource errors.

## Asset Style & Naming

Do not reformat generated `.gltf`, `.obj`, `.mtl`, or binary files unless the edit is intentional. Use descriptive, existing-style filenames: `<material>_baseColor`, `<material>_metallicRoughness`, and `<material>_normal`; retain the original extension and case. Place all maps required by a new model beside that model or in its dedicated texture directory. Avoid duplicate textures unless a consumer requires a separate relative path.

## Validation Guidelines

After modifying a model, verify geometry, UVs, normals, material assignment, and texture channels visually. Test at least one load from a clean checkout or copied folder to catch broken relative references. Keep binary buffers synchronized with their corresponding descriptors (for example, `scene.gltf` and `scene.bin`).

## Contributions & Attribution

Keep commits small and imperative, such as `Add cabinet normal map` or `Fix helmet texture reference`. No repository commit history is available here to establish a stricter convention. In change descriptions, list affected assets, source/license details, and validation performed; include before/after screenshots when a visual result changes. Preserve existing attribution and disclaimer files, and add equivalent provenance and license information for every imported asset.
