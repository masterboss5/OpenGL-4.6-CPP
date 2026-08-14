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

## Styling Standards

The OpenFrame editor uses one coherent, dark, modern visual language. The supplied Explorer, ribbon, and Properties references define the target character: compact and information-dense, visually quiet, clearly structured, rounded where interaction benefits from it, and free of ornamental clutter. Use the reference images as visual direction, not as permission to copy another product's branding or inconsistent legacy details.

### Styling Authority

- `OpenFrame/Source/editor/ui/EditorTheme.*` owns global design tokens and application-wide Dear ImGui style configuration.
- Reusable OpenFrame controls own component styling. Feature panels consume those controls instead of recreating buttons, search fields, rows, property grids, popups, or icon treatments locally.
- Do not call `ImGui::StyleColorsDark()` and stop there. Stock Dear ImGui appearance is not an acceptable finished UI.
- Do not scatter literal colors, radii, padding, or animation timings through feature code. Add a semantic token to the theme and consume it by name.
- `PushStyleColor` and `PushStyleVar` are for a documented, temporary semantic state only. They are not substitutes for a reusable component.
- The editor is dark-only. Every new surface and state must be designed and verified against the dark palette below.

### Color Tokens

Use these exact sRGB values as the default OpenFrame dark theme. Alpha is `FF` unless shown otherwise.

| Token | Hex | Required use |
| --- | --- | --- |
| `Canvas` | `#0B0D10` | Main editor background behind docked surfaces |
| `Panel` | `#111318` | Explorer, Properties, Asset Browser, and ordinary panel background |
| `PanelRaised` | `#171A20` | Headers, toolbars, section headers, cards, and raised regions |
| `Surface` | `#1C1F26` | Inputs, compact buttons, dropdowns, and inactive tabs |
| `SurfaceHover` | `#252A33` | Hovered controls and rows |
| `SurfacePressed` | `#2D333E` | Pressed controls and active manipulation |
| `SurfaceSelected` | `#263A55` | Selected rows and inactive selected items |
| `SurfaceSelectedHover` | `#304B6E` | Selected item while hovered |
| `BorderSubtle` | `#252932` | Default separators and surface outlines |
| `BorderStrong` | `#363C48` | Deliberate boundaries, resizers, and emphasized input outlines |
| `TextPrimary` | `#F1F3F7` | Titles, values, selected labels, and primary content |
| `TextSecondary` | `#B8BEC9` | Ordinary labels and secondary content |
| `TextMuted` | `#818A99` | Hints, metadata, placeholders, and inactive icons |
| `TextDisabled` | `#555E6D` | Unavailable controls; never use opacity alone to communicate state |
| `Accent` | `#3D82E6` | Primary selection, focus, and the one principal action in a region |
| `AccentHover` | `#5798F0` | Hovered accent control |
| `AccentPressed` | `#2E68B8` | Pressed accent control |
| `FocusRing` | `#73ACFF` | Keyboard focus outline |
| `Success` | `#43B581` | Successful state and confirmation |
| `Warning` | `#E5A84B` | Recoverable warning and pending attention |
| `Danger` | `#E05A61` | Destructive action and error |
| `Info` | `#55A7E8` | Informational state |
| `Overlay` | `#050609B8` | Modal backdrop |
| `Shadow` | `#00000066` | Floating-surface shadow only |

- Never use pure black for a primary surface or pure white for body text.
- Accent color is scarce. Do not paint whole toolbars, panels, or large empty regions blue.
- Use semantic colors only for their semantic purpose. Do not use warning yellow merely as decoration.
- Body text must maintain at least `4.5:1` contrast against its background. Large text, focus outlines, meaningful icons, and control boundaries must maintain at least `3:1`.
- Disabled state uses `TextDisabled` plus a state cue such as a disabled cursor, unavailable icon, or tooltip. Do not reduce an entire control below `60%` opacity.

### Spacing and Layout Tokens

All measurements are logical pixels before DPI scaling. Use a `4 px` base grid.

| Token | Value | Required use |
| --- | ---: | --- |
| `SpaceXXS` | `2 px` | Optical correction only |
| `SpaceXS` | `4 px` | Tight internal grouping |
| `SpaceS` | `8 px` | Icon gaps, compact control padding, row inset |
| `SpaceM` | `12 px` | Standard control padding and panel content inset |
| `SpaceL` | `16 px` | Section separation |
| `SpaceXL` | `24 px` | Major panel group separation |
| `SpaceXXL` | `32 px` | Empty-state and page-level separation |

- Default panel content inset is `12 px` horizontally and `8 px` vertically.
- Default control padding is `10 px` horizontally and `6 px` vertically.
- Adjacent label-to-control gap is `8 px`. Icon-to-label gap is exactly `8 px`.
- Related controls use an `8 px` gap; unrelated groups use `16 px` or a labeled section.
- Never position UI by arbitrary one-off offsets when a token or measured component bound can express it.
- Align text baselines, icon centers, and control edges. A layout that is numerically spaced but optically misaligned is not finished.
- Panels must remain usable at their declared minimum size. Use clipping, scrolling, elision, or responsive reflow instead of overlap.

### Radius, Border, and Elevation Tokens

| Element | Radius | Border |
| --- | ---: | --- |
| Tree row, menu row, compact toggle | `6 px` | None until focused or selected |
| Input, search field, button, dropdown | `8 px` | `1 px BorderSubtle` |
| Tab or segmented control | `8 px` | `1 px BorderSubtle` when separated from its container |
| Card, popup, context menu | `10 px` | `1 px BorderStrong` |
| Modal dialog and home-page project card | `12 px` | `1 px BorderStrong` |
| Tooltip | `6 px` | `1 px BorderStrong` |

- Borders are exactly `1 px` at 100% scaling and must be snapped to physical pixels. Keyboard focus is a `2 px` `FocusRing` drawn outside or inset without changing layout.
- Do not stack a panel border, child border, and table border on the same edge. One boundary gets one line.
- Docked panels remain structurally square at shared dock edges. Round only exposed floating-window corners or contained controls.
- Floating popups and modals use a restrained shadow: `0 8 px 24 px Shadow`. Ordinary docked panels and rows have no drop shadow.
- Do not use gradients, bevels, glass effects, neon glows, or decorative outlines.

### Typography

- Use `Segoe UI Variable` when available and `Segoe UI` as the fallback. Use the icon font only for icons, never body text.
- Default body text is `14 px`, regular weight, with an approximately `20 px` line height.
- Compact metadata and keyboard hints are `12 px`, regular weight, with a `16 px` line height.
- Panel titles and section labels are `14 px`, semibold, with a `20 px` line height.
- Dialog and page titles are `20 px`, semibold, with a `28 px` line height.
- Home-page hero titles may use `28 px`, semibold, with a `36 px` line height.
- Use sentence case for labels and commands. Do not use all caps for section headings.
- Use no more than regular and semibold weights in ordinary editor chrome. Hierarchy comes from spacing, color, and placement rather than many font sizes.
- Truncate overflowing single-line labels with an ellipsis and show the complete value in a tooltip after a `400 ms` hover.
- Numeric values use tabular figures when the active font provides them.

### Icons

- Standard interface icons render at `16 px`. Primary toolbar icons may render at `18 px`; empty-state illustrations may render at `24 px` or `32 px`.
- Every tree or list icon occupies a fixed `24 px`-wide cell, is optically centered inside it, and has an `8 px` gap before its label.
- Icon-only buttons are at least `28 x 28 px`; ordinary toolbar icon buttons are `32 x 32 px`.
- Use one coherent outline style with consistent apparent stroke weight. Do not mix emoji, filled clip art, unrelated font styles, and outline icons in the same surface.
- Color-coded type icons are allowed in the Explorer, but their colors must remain legible on `Panel` and must not replace the selected, unavailable, or error state.
- Every icon-only control requires an accessible label and a tooltip. The tooltip appears after `400 ms` and includes the shortcut when one exists.
- Unknown or plugin-defined types use a deliberate generic fallback icon; they never render as missing glyph boxes.

### Core Component Contracts

#### Docked panels

- A panel header is `36 px` high with `12 px` horizontal inset, `PanelRaised` background, and one `1 px BorderSubtle` bottom separator.
- Center a short panel title only when the panel has symmetric header actions, as in the Explorer reference. Otherwise left-align it.
- Header action buttons are `28 x 28 px`, separated by `4 px`, and use transparent idle backgrounds with `SurfaceHover` and `SurfacePressed` interaction fills.
- Panel content uses `Panel`; nested child regions must not introduce a visibly different background without a semantic reason.

#### Explorer and hierarchical lists

- The search field has `8 px` outer horizontal margin, `6 px` outer vertical margin, `32 px` height, `8 px` radius, and a `1 px BorderSubtle` outline.
- Tree rows are `28 px` high with a `4 px` horizontal inset from the panel edge and a `6 px` selection radius.
- Reserve `18 px` for each hierarchy depth. Ancestor guide lines are `1 px BorderSubtle` and stop where the hierarchy ends.
- Reserve an `18 px` disclosure cell, then the fixed `24 px` icon cell, then an `8 px` icon-to-label gap.
- Row labels use `TextPrimary` when selected and `TextSecondary` otherwise. Inactive metadata uses `TextMuted`.
- Hover uses `SurfaceHover`; selection uses `SurfaceSelected`; selected hover uses `SurfaceSelectedHover`. Selection must remain visible when the panel loses focus.
- The insertion button appears on row hover or selection, is `24 x 24 px`, has a `6 px` radius, and sits `4 px` from the trailing edge. Its appearance must not move or resize the label.
- Drag destinations show a `2 px Accent` insertion line or a clearly bounded parent highlight. Do not rely on cursor shape alone.

#### Properties and inspectors

- Property section headers are `32 px` high, `PanelRaised`, semibold, and separated from adjacent sections by one `1 px BorderSubtle` line.
- Property rows are at least `32 px` high. Labels use `12 px` left padding; editors use `8 px` horizontal internal padding.
- The default label/value split is `48% / 52%`, user-resizable, with a `1 px BorderSubtle` divider. Persist the user-adjusted split per panel.
- Alternating row colors are forbidden. Use alignment and subtle separators rather than striping.
- Read-only values use `TextMuted` but remain readable. Invalid values retain their text and receive a `Danger` outline plus a concrete diagnostic.
- Compound values such as transforms expand inline with the same label/value grid and an additional `18 px` hierarchy indentation.
- Continuous edits produce live feedback but commit as one undoable gesture.

#### Buttons and controls

- Standard controls are `32 px` high. Compact controls may be `28 px`; primary dialog actions may be `36 px`.
- Primary buttons use `Accent`, `AccentHover`, and `AccentPressed` with `TextPrimary`. Secondary buttons use `Surface`, `SurfaceHover`, and `SurfacePressed`.
- Destructive buttons remain neutral until hovered or confirmed; use `Danger` for the destructive emphasis, not as a permanent large fill.
- Checkboxes are `18 x 18 px` with a `5 px` radius, `1 px BorderStrong`, and a high-contrast checkmark. Their full label row is clickable.
- Sliders and drags must provide keyboard entry and direct numeric input. Do not force precision work through pointer movement alone.
- Search fields include a leading search icon, clear button when nonempty, placeholder in `TextMuted`, and keyboard focus ring.

#### Menus, popups, and dialogs

- Menus and popups use `PanelRaised`, `10 px` radius, `1 px BorderStrong`, `8 px` outer padding, and the defined floating shadow.
- Menu rows are `28 px` high with `8 px` horizontal padding. Reserve fixed columns for icon, label, shortcut, and submenu arrow.
- Context menus open adjacent to the invoking item and remain completely inside the current monitor work area.
- Modal dialogs use `12 px` radius, `16 px` content padding, `24 px` major-section spacing, and a dimmed `Overlay` backdrop.
- Place the primary dialog action at the trailing edge. Keep destructive and cancel actions visually distinct and keyboard reachable.

#### Toolbars and tabs

- Primary toolbar height is `40 px`; icon buttons are `32 x 32 px` with `4 px` gaps and `12 px` between command groups.
- Separate command groups with spacing first. Use a `1 px BorderSubtle` separator only when grouping remains ambiguous.
- Tabs are `32 px` high with `12 px` horizontal padding. Active tabs use `TextPrimary` and a `2 px Accent` indicator; do not fill the entire tab with a saturated accent.
- Tool labels may sit below icons only in a deliberate ribbon group. Do not mix labeled and unlabeled tools randomly.

#### Scrollbars and splitters

- Scrollbar track is transparent. The thumb is `8 px` wide, `#3A404C`, rounded to `4 px`, and becomes `#525A68` on hover.
- Splitter visual thickness is `1 px BorderSubtle`; its invisible interaction target is at least `6 px` wide.
- Scrolling must not shift content horizontally when the scrollbar appears.

### Interaction and Motion

- Every interactive component defines idle, hover, pressed, focused, selected, disabled, and error states where applicable.
- Use `80 ms` for hover color transitions, `120 ms` for press/selection transitions, and `160 ms` for popup or disclosure transitions.
- Use cubic ease-out for entrances and cubic ease-in for exits. Animations must be interruptible and keyed by stable UI identity.
- Do not animate layout during continuous scene manipulation. Never delay input, selection, or viewport feedback for decoration.
- Respect the system reduced-motion preference by removing nonessential interpolation while preserving state changes.
- Pointer targets are at least `28 x 28 px`; preferred targets are `32 x 32 px`. Small glyphs receive a larger invisible hit region.
- Keyboard navigation and focus order must follow visual order. Focus may never be indicated by color change alone; draw the `2 px FocusRing`.

### DPI, Rendering, and Responsiveness

- Treat every measurement above as a logical pixel and scale it once using the active viewport DPI. Never mix scaled and unscaled coordinates in one component.
- Snap one-pixel lines, icons, and rectangular boundaries to physical pixel centers so they remain crisp at 100%, 125%, 150%, and 200% scaling.
- Rebuild or select the correct font atlas for the active DPI rather than scaling a low-resolution rasterized font.
- Verify every panel at its minimum supported size and at 100%, 150%, and 200% scaling. Text may elide; controls may reflow; controls may not overlap.
- UI rendering must not block the editor thread on asset loading. Display a stable placeholder or progress state while immutable data is prepared asynchronously.

### Implementation and Review Requirements

- Add or extend semantic theme tokens before styling a new component. Names describe purpose, not a literal color such as `DarkGray3`.
- Reusable widgets must accept content and semantic state; feature code must not duplicate their drawing and interaction behavior.
- Custom drawing must use measured text/icon bounds, fixed layout cells, clipping rectangles, and stable IDs. Do not align glyphs with guessed offsets.
- A component is incomplete unless its hover, pressed, focus, disabled, error, empty, loading, and overflow behavior has been considered.
- Do not communicate state solely by hue. Pair color with shape, icon, text, or placement.
- Do not silently fall back to stock Dear ImGui presentation when a custom OpenFrame component exists.
- Any intentional exception to these standards must be localized, documented next to the owning component, and justified by usability rather than convenience.
- Visual changes require before/after screenshots at identical size and DPI. Review spacing, alignment, clipping, contrast, focus, hover, disabled state, and long-text behavior.
- Add deterministic layout or interaction validation where feasible, then manually smoke-test the real editor surface. A successful compile alone does not prove visual quality.
