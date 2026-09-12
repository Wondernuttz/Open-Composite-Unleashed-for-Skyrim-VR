# Eye tracking debug rings — 4.3.7 Hotfix 1

Open the Configurator from the enabled OCU mod folder. On Video, under Foveated
Rendering, enable **Show eye-tracking rings (debug)**, save opencomposite.ini,
and restart Skyrim. The setting is off by default (`[vrs] foveationDebugRings=false`).

- Solid red: the full-detail center boundary of the current eye-tracked profile.
- Dashed red: its configured middle boundary.
- Amber rings: OCU is using the fixed fallback profile.
- Amber X with no rings: no active profile is available.

Each eye uses its own current profile center and sizes. These sizes follow OCU's
existing UV radial convention, not degrees; headset projection can make the rings
look oval. The middle outline remains a size guide when a compatibility cap makes
the middle and outer shading rates equal. The rings show the selected profile;
they do not prove that every shader or draw uses reduced shading.

The diagnostic uses two transparent D3D11/OpenXR quad layers, one visible only
to each eye. Each quad matches its eye's asymmetric FOV and rotated pose. Skyrim
remains the sole scene projection; the earlier debug build submitted a second
projection, and puntloos reported seeing only the rings on his setup. This change
avoids that multi-projection composition path. It does
not write into Skyrim's color image, CSX/upscaler history or DAPA source texture.
DAPA synthetic frames reuse the released debug image with their eye poses.
A busy debug image is skipped; unsupported formats or layer limits skip the
overlay. When the toggle is off, there is no debug swapchain or image upload.
Turn the diagnostic off for performance measurements: enabling the additional
layers and uploading their pixels has a cost.

Local verification: Release runtime and Configurator builds; actual WARP and
hardware D3D11 texture readbacks; per-eye boundary/alpha/BGRA checks; unchanged
scene pixels and bindings; mocked OpenXR timeout recovery and session cleanup;
production gaze state, combined CSX/OCU communication, DAPA timing and Configurator
save/reload tests. OpenXR calls in the overlay tests are mocked. Visibility,
alignment and timing in a physical headset still need an in-game test of this hotfix.

OpenXR references:
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerQuad.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjection.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html
