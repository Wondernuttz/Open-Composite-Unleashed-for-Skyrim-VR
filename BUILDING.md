# Building OCU on Windows

The runtime, SKSE plugin and desktop tools are separate projects in this repository. Run the commands below from the repository root in a Visual Studio developer PowerShell with the x64 C++ tools available.

| Component | Current project directory | Build target / project |
| --- | --- | --- |
| OpenVR-to-OpenXR runtime | `OpenCompositeSkyrimVR` | CMake target `OCOVR` |
| Skyrim VR SKSE plugin | `OpenCompositeInput- Skyrim SKSE/OpenCompositeInput` | CMake target `OpenCompositeInput` |
| Configurator | `OpenCompositeSkyrimVR/OC Unleashed Configurator` | `OpenCompositeConfigurator.csproj` |
| Keyboard Studio | `OpenCompositeSkyrimVR/OCU Keyboard Studio` | `OCUKeyboardStudio.csproj` |

The outer `OpenCompositeInput- Skyrim SKSE/CMakeLists.txt` and its `src` directory are a legacy copy. Use the **nested** `OpenCompositeInput` project for the current plugin. The repository root is not itself a CMake project.

## Toolchain and dependency setup

The local Windows builds inspected for this documentation use CMake 4.2.1, the `Visual Studio 18 2026` x64 generator, MSVC v145 and Windows SDK 10.0.26100.0. Runtime header generation uses Python 3.12. The runtime requests C++20; the plugin and its CommonLib dependency request C++23. Install the Desktop development with C++ workload and a suitable Windows SDK. The commands below use that inspected Visual Studio generator; a different toolchain needs its own build validation.

The Configurator and Keyboard Studio target `net9.0-windows` and publish self-contained `win-x64` applications. Install a .NET SDK capable of targeting .NET 9 (the local environment has SDKs 9.0.310 and 10.0.301). NuGet restore supplies the Configurator's OpenCvSharp packages.

### Runtime SDK inputs

Keep the checked-in `OpenCompositeSkyrimVR/libs`, `OpenVRHeaders`, `scripts`, `BundledLibs` and asset directories in place. CMake builds its bundled OpenXR loader and uses these relative paths; header and stub generation runs through Python during the build.

Some vendor SDK import libraries exist in the local development setup but are **not tracked in Git**. Before configuring a fresh checkout, supply the following files from the corresponding vendor SDK distributions, preserving their license terms:

| Expected path under `OpenCompositeSkyrimVR` | Use |
| --- | --- |
| `libs/nvapi/amd64/nvapi64.lib` | NVIDIA VRS and NVIDIA GPU regression targets. Use the matching NVIDIA NVAPI SDK import library for the headers in `libs/nvapi`. |
| `libs/vulkan/Lib/vulkan-1.lib` | x64 Vulkan loader import library. The Windows runtime links it even when testing D3D11 rendering. |
| `libs/vulkan/Lib32/vulkan-1.lib` | x86 Vulkan loader import library selected by a 32-bit configuration; not used by the Skyrim VR x64 commands below. |

Vulkan headers belong in `libs/vulkan/Include`. Do not substitute the x86 import library for the x64 file. The current CMake NVAPI check tests for `nvapi.h`, **not** the import library: with the checked-in header present, a missing `nvapi64.lib` causes a link failure rather than automatically disabling VRS.

Optional upscaler support is selected from SDK headers at configuration time:

- FidelityFX: `libs/fidelityfx/api/include/ffx_api.h`, with the companion upscaler headers in `libs/fidelityfx/upscalers/include`.
- DLSS: `libs/dlss/include/nvsdk_ngx.h` and `libs/dlss/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`.

These compile-time inputs do not replace the upscalers' runtime DLLs in a complete mod package. Keep the package's runtime dependencies and license notices when assembling a test drop.

### SKSE dependencies

The plugin requires an external CommonLibSSE-NG/CommonLibVR **source tree** with VR support. Set `COMMONLIBSSE_ROOT` to that tree, which must contain `CMakeLists.txt`, `cmake/CommonLibSSE.cmake`, its headers and sources, and populated dependencies such as `extern/openvr`. The inspected local dependency reports CommonLibSSE version 4.7.1. This repository does not pin or automatically fetch that external checkout; arbitrary CommonLib revisions are not guaranteed compatible.

Set `VCPKG_ROOT` to a bootstrapped vcpkg checkout. The plugin's `vcpkg.json` requests `spdlog`, `fmt`, `rapidcsv`, `directxtk` and `directxmath`. The inspected build uses the `x64-windows-static-md` target and host triplets. The plugin CMake configuration enables Skyrim VR and disables SE/AE and CommonLib's own tests.

For example, replace these paths with your dependency locations:

```powershell
$env:VCPKG_ROOT = 'C:/Dependencies/vcpkg'
$env:COMMONLIBSSE_ROOT = 'C:/Dependencies/CommonLibVR'
```

The cache variable is spelled `COMMONLIBSSE_ROOT` in uppercase. Passing `-DCOMMONLIBSSE_ROOT=...` overrides its environment/default lookup. Do not rely on the fallback sibling staging-directory path in a fresh checkout.

## Build the runtime

After supplying the SDK inputs:

```powershell
cmake -S OpenCompositeSkyrimVR -B OpenCompositeSkyrimVR/build -G "Visual Studio 18 2026" -A x64 -DOCU_DAPA_CAPTURE=OFF
cmake --build OpenCompositeSkyrimVR/build --config Release --target OCOVR --parallel
```

The resulting DLL is `OpenCompositeSkyrimVR/build/bin/Release/vrclient_x64.dll`. For the Skyrim VR mod, copy it as **`root/openvr_api.dll`** in the mod package. The `OCOVR` target does not build or deploy the Configurator or SKSE plugin.

Keep `OCU_DAPA_CAPTURE=OFF` for tester packages. Enabling it compiles developer DAPA recording controls. Use a fresh build directory if changing the generator or architecture rather than reusing an incompatible CMake cache.

## Build the current SKSE plugin

```powershell
cmake -S "OpenCompositeInput- Skyrim SKSE/OpenCompositeInput" -B "OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/build/vs-release" -G "Visual Studio 18 2026" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" "-DCOMMONLIBSSE_ROOT=$env:COMMONLIBSSE_ROOT" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DVCPKG_HOST_TRIPLET=x64-windows-static-md
cmake --build "OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/build/vs-release" --config Release --target OpenCompositeInput --parallel
```

The resulting DLL is `OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/build/vs-release/Release/OpenCompositeInput.dll`. Its mod-package destination is **`SKSE/Plugins/OpenCompositeInput.dll`**.

The nested project's `release-vr` preset is an alternative Ninja configuration. It requires Ninja, the same external dependencies and an x64 MSVC developer environment, and writes to the nested `build/release` directory. Do not confuse that output with `build/vs-release/Release` from the commands above.

## Publish the desktop tools

These commands choose explicit output directories so the files to package are unambiguous:

```powershell
dotnet publish "OpenCompositeSkyrimVR/OC Unleashed Configurator/OpenCompositeConfigurator.csproj" -c Release -r win-x64 --self-contained true -o out/Configurator
dotnet publish "OpenCompositeSkyrimVR/OCU Keyboard Studio/OCUKeyboardStudio.csproj" -c Release -r win-x64 --self-contained true -o out/KeyboardStudio
```

The executables are `out/Configurator/OC Unleashed Configurator for Skyrim VR.exe` and `out/KeyboardStudio/OCU Keyboard Studio.exe`. Preserve the other published files too: the Configurator uses native camera/MediaPipe dependencies, model files and documentation; Keyboard Studio uses its external `Assets` directory. Single-file publishing does not make these companion files optional. Consult the projects' content entries for their source paths.

Publishing a desktop tool produces that component's files, not a complete OCU mod ZIP. A full test drop also needs the runtime, SKSE plugin, configuration examples, bindings/assets and dependency/license files. Use the existing mod layout; do not replace users' saved INI files as part of a binary-only update.

## Validation scope

These instructions were checked against the current project files, local configured build caches and output locations. They do **not** claim a clean-clone build with newly acquired SDK/CommonLib dependencies, native Linux compilation, or headset performance qualification. CMake regression targets are generally `EXCLUDE_FROM_ALL` and must be built and run explicitly; building `OCOVR` alone does not run them.
