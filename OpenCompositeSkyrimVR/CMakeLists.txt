# Set the Windows kit if using MSVC
# CMake will look for the newest kit that isn't newer than this
# Required to find modern D3D12 header
if (MSVC)
	set(CMAKE_SYSTEM_VERSION 10.0.17134.0)
endif ()

cmake_minimum_required(VERSION 3.16)
project(OpenComposite)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strip absolute source paths from PDBs so crash logs don't expose local user paths
if (MSVC)
	string(APPEND CMAKE_CXX_FLAGS " /d1trimfile:\"${CMAKE_SOURCE_DIR}/\"")
	set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /PDBALTPATH:%_PDB%")
	set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /PDBALTPATH:%_PDB%")
endif()

# Options

set(XR_VALIDATION_LAYER_PATH "" CACHE STRING "A path to set XR_API_LAYER_PATH to. Setting this will enable the debug layer")
set(XR_VALIDATION_FILE_NAME "" CACHE STRING "A filename to set XR_CORE_VALIDATION_FILE_NAME to.")

option(USE_SYSTEM_OPENXR "Try using system installation of OpenXR if available" OFF)
option(USE_SYSTEM_GLM "Try using system installation of glm if available" OFF)
option(OCU_DAPA_CAPTURE "Enable developer DAPA recording gestures, hotkey and desktop requests (not for shipping)" OFF)
if(OCU_DAPA_CAPTURE)
	add_compile_definitions(OCU_DAPA_CAPTURE=1)
endif()

# Directory for generated files, those being split headers and stubs
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

# Platform-dependent flags
# Support Vulkan on Linux instead of DirectX
if (WIN32)
	set(GRAPHICS_API_SUPPORT_FLAGS -DSUPPORT_DX -DSUPPORT_DX11 -DSUPPORT_DX12 -DSUPPORT_VK -DSUPPORT_GL)
elseif (ANDROID)
	set(GRAPHICS_API_SUPPORT_FLAGS -DSUPPORT_VK -DSUPPORT_GLES)
else ()
	set(GRAPHICS_API_SUPPORT_FLAGS -DSUPPORT_VK -DSUPPORT_GL)
endif()

set(FORCE_COLORED_OUTPUT            OFF CACHE BOOL "Always produce ANSI-colored output (GNU/Clang only).")

if(FORCE_COLORED_OUTPUT)
	if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
		add_compile_options(-fdiagnostics-color=always)
	elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
		add_compile_options(-fcolor-diagnostics)
	endif()
endif()

# For all projects, set the symbol visibility to private on Linux
# If we don't then if an application declares a function with the same name as an OpenComposite function
# the application's one will be used instead. This has some nasty effects - for example, when using the
# global installation both libopenvr_api.so and OpenComposite will define VR_InitInternal2. What's supposed
# to happen is that libopenvr_api calls a VRClient interface method which in turn calls OpenComposite's version
# of VR_InitInternal2. But since libopenvr_api was loaded first it's function will be used, leading to
# a stack overflow.
# In general using private symbols is a great idea, and it's something we'd want to do anyway.
# There is also -Bsymbolic set when linking the final shared library, see the bottom of this file
# FIXME the Vulkan functions still get exported, hopefully they won't fight with the application's ones.
if (WIN32)
else ()
    add_definitions(-fvisibility=hidden)
endif ()

# === Runtime Switcher ===
include_external_msproject(
	RuntimeSwitcher ${CMAKE_SOURCE_DIR}/RuntimeSwitcher/RuntimeSwitcher.csproj
	TYPE FAE04EC0-301F-11D3-BF4B-00C04F79EFBC
	PLATFORM "AnyCPU"
)

# === MinHook (x86/x64 API hooking for OVR perf stats interception) ===
if (WIN32)
	add_subdirectory(libs/minhook)
endif()

# === OpenVR Headers ===
add_library(OpenVR INTERFACE)
target_include_directories(OpenVR INTERFACE OpenVRHeaders) # TODO make this private and put the public headers elsewhere

# === Vulkan ===
add_library(Vulkan INTERFACE)

if (WIN32)
	# On Windows, use the bundled copy
	target_include_directories(Vulkan INTERFACE libs/vulkan/Include) # TODO make this private and put the public headers elsewhere
	if ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
		target_link_libraries(Vulkan INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs/vulkan/Lib/vulkan-1.lib)
	else ()
		target_link_libraries(Vulkan INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libs/vulkan/Lib32/vulkan-1.lib)
	endif ()

	target_compile_definitions(Vulkan INTERFACE -DVK_USE_PLATFORM_WIN32_KHR)
elseif (ANDROID)
	# Let the system sort out the linkage, we'll just use our included headers
	target_include_directories(Vulkan INTERFACE libs/vulkan/Include)
else ()
	# On Linux, use the system copy
	find_library(Vulkan vulkan REQUIRED)
	target_include_directories(Vulkan INTERFACE ${Vulkan_INCLUDE_DIR})

	# We have to include the vulkan library, since the linker is now set to fail if we use undefined symbols
	# On Gentoo at least, it seems there's nothing in Vulkan_LIBRARIES
	if ("${Vulkan_LIBRARIES}")
		target_link_libraries(Vulkan INTERFACE ${Vulkan_LIBRARIES})
	else ()
		target_link_libraries(Vulkan INTERFACE -lvulkan)
	endif ()

endif ()

# === OpenXR ===

if (USE_SYSTEM_OPENXR)
	find_package(OpenXR)
	set(XrLib OpenXR::openxr_loader)
endif()

if (NOT OpenXR_FOUND)
	# Building CMake subprojects is a real pain (IMO), so just build this here
	set(XrDir libs/openxr-sdk)
	set(XrDirLoader libs/openxr-sdk/src/loader)
	set(XrDirCommon libs/openxr-sdk/src/common)
	if (ANDROID)
		# Whatever consumes this library must then link to an OpenXR loader, such as the Oculus one
		add_library(OpenXR STATIC scripts/empty.c) # Doesn't do anything
	else ()
	add_library(OpenXR STATIC
		${XrDirLoader}/api_layer_interface.cpp
		${XrDirLoader}/api_layer_interface.hpp
		${XrDirLoader}/loader_core.cpp
		${XrDirLoader}/loader_instance.cpp
		${XrDirLoader}/loader_instance.hpp
		${XrDirLoader}/loader_logger.cpp
		${XrDirLoader}/loader_logger.hpp
		${XrDirLoader}/loader_logger_recorders.cpp
		${XrDirLoader}/loader_logger_recorders.hpp
		${XrDirLoader}/manifest_file.cpp
		${XrDirLoader}/manifest_file.hpp
		${XrDirLoader}/runtime_interface.cpp
		${XrDirLoader}/runtime_interface.hpp

		${XrDirLoader}/xr_generated_loader.hpp
		${XrDirLoader}/xr_generated_loader.cpp
		${XrDir}/src/xr_generated_dispatch_table.h
		${XrDir}/src/xr_generated_dispatch_table.c

		${XrDirCommon}/filesystem_utils.cpp
		${XrDirCommon}/object_info.cpp

		${XrDir}/src/external/jsoncpp/src/lib_json/json_reader.cpp
		${XrDir}/src/external/jsoncpp/src/lib_json/json_value.cpp
		${XrDir}/src/external/jsoncpp/src/lib_json/json_writer.cpp
	)
	endif()
	target_include_directories(OpenXR PRIVATE ${XrDirCommon} ${XrDir}/src ${XrDir}/src/external/jsoncpp/include)
	target_include_directories(OpenXR PUBLIC ${XrDir}/include)
	# Platform-dependent flags
	if (WIN32)
		target_compile_definitions(OpenXR PRIVATE -DXR_OS_WINDOWS -DXR_USE_PLATFORM_WIN32
		-DXR_USE_GRAPHICS_API_D3D11 -DXR_USE_GRAPHICS_API_D3D12 -DXR_USE_GRAPHICS_API_OPENGL)
		target_link_libraries(OpenXR PUBLIC advapi32 pathcch OpenGL32)
	else()
		# TODO Turtle1331 should we include -DXR_USE_PLATFORM_(XLIB|XCB|WAYLAND) here?
		target_compile_definitions(OpenXR PRIVATE -DXR_OS_LINUX
		-DXR_USE_GRAPHICS_API_OPENGL -DXR_USE_GRAPHICS_API_VULKAN)
		target_link_libraries(OpenXR PUBLIC -ldl)
	endif()
	target_link_libraries(OpenXR PUBLIC Vulkan)

	if (ANDROID)
		target_compile_definitions(OpenXR PUBLIC -DXR_USE_PLATFORM_ANDROID -DXR_USE_GRAPHICS_API_OPENGL_ES)
	endif()
	set(XrLib OpenXR)
endif()

# === glm ===
# Since we used to use LibOVR's maths library, we need a replacement
# glm is an obvious choice

if (USE_SYSTEM_GLM)
	find_package(glm)
	set(glmLib glm::glm)
endif()

if (NOT glm_FOUND)
	add_library(glm INTERFACE)
	target_include_directories(glm INTERFACE libs/glm) # No separate include directory :(
	set(glmLib glm)
endif()

# === DrvOpenXR ===
add_library(DrvOpenXR STATIC
	DrvOpenXR/DrvOpenXR.cpp

	DrvOpenXR/XrDriverPrivate.h

	DrvOpenXR/XrBackend.cpp
	DrvOpenXR/XrBackend.h
	DrvOpenXR/FoveationDebugOverlay.cpp
	DrvOpenXR/FoveationDebugOverlay.h

	DrvOpenXR/XrTrackedDevice.cpp
	DrvOpenXR/XrTrackedDevice.h

	DrvOpenXR/XrHMD.cpp
	DrvOpenXR/XrHMD.h

	DrvOpenXR/XrController.cpp
	DrvOpenXR/XrGenericTracker.cpp
	DrvOpenXR/XrNetworkTracker.cpp
	DrvOpenXR/XrNetworkTracker.h
	DrvOpenXR/XrController.h

	DrvOpenXR/ASWProvider.cpp
	DrvOpenXR/ASWProvider.h
	DrvOpenXR/DapaCapture.cpp
	DrvOpenXR/DapaCapture.h


	DrvOpenXR/tmp_gfx/TemporaryGraphics.cpp
	DrvOpenXR/tmp_gfx/TemporaryGraphics.h
	DrvOpenXR/tmp_gfx/TemporaryD3D11.cpp
	DrvOpenXR/tmp_gfx/TemporaryD3D11.h
	DrvOpenXR/tmp_gfx/TemporaryVk.cpp
	DrvOpenXR/tmp_gfx/TemporaryVk.h
	# Ensuring vrtypes.h exists should mean the rest of our generated interfaces exists
	${GENERATED_DIR}/interfaces/vrtypes.h
	${GENERATED_DIR}/static_bases.gen.h
)
target_include_directories(DrvOpenXR PUBLIC DrvOpenXR/pub ${CMAKE_BINARY_DIR})
target_include_directories(DrvOpenXR PRIVATE DrvOpenXR OpenOVR)
target_link_libraries(DrvOpenXR PUBLIC OpenVR ${XrLib} ${glmLib} d3d12.lib dxgi.lib windowscodecs.lib ole32.lib shell32.lib)
target_compile_definitions(DrvOpenXR PRIVATE ${GRAPHICS_API_SUPPORT_FLAGS} -D_CRT_SECURE_NO_WARNINGS)
source_group(Public REGULAR_EXPRESSION DrvOpenXR/pub/*)

if (NOT (XR_VALIDATION_LAYER_PATH STREQUAL ""))
	target_compile_definitions(DrvOpenXR PRIVATE -DXR_VALIDATION_LAYER_PATH=\"${XR_VALIDATION_LAYER_PATH}\")
endif ()
if (NOT (XR_VALIDATION_FILE_NAME STREQUAL ""))
	target_compile_definitions(DrvOpenXR PRIVATE -DXR_VALIDATION_FILE_NAME=\"${XR_VALIDATION_FILE_NAME}\")
endif ()

# === OCCore ===
# TODO: precompiled headers
if (WIN32)
	set(OVR_RESOURCES
		OpenOVR/resources.rc
	)
else ()
	enable_language(ASM)
	set_property(SOURCE OpenOVR/Misc/resources_linux.s APPEND PROPERTY COMPILE_OPTIONS "-x" "assembler-with-cpp")
	set_property(SOURCE OpenOVR/Misc/resources_linux.s SET PROPERTY INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
	set(OVR_RESOURCES
		OpenOVR/Misc/resources_linux.s
	)
endif ()
set(OVR_PCH_EXCLUDED
	OpenOVR/Misc/ini.c
	OpenOVR/Misc/lodepng.cpp
	OpenOVR/Misc/EffectFoveationState.cpp
)

add_library(OCCore STATIC
	# This file is used to generate the precompiled headers
	OpenOVR/stdafx.cpp

	OpenOVR/Compositor/compositor.cpp
	OpenOVR/Compositor/dx11compositor.cpp
	OpenOVR/Compositor/Fsr3Upscaler.cpp
	OpenOVR/Compositor/Fsr3Upscaler.h
	OpenOVR/Compositor/DlssUpscaler.cpp
	OpenOVR/Compositor/DlssUpscaler.h
	OpenOVR/Compositor/VRSManager.cpp
	OpenOVR/Compositor/VRSShaderGuard.cpp
	OpenOVR/Compositor/VRSShaderGuard.h
	OpenOVR/Compositor/ExactPixelShader.h
	OpenOVR/Compositor/VRSAlphaCoverageScope.cpp
	OpenOVR/Compositor/VRSAlphaCoverageScope.h
	OpenOVR/Compositor/VRSManager.h
	OpenOVR/Compositor/RDMRenderScope.cpp
	OpenOVR/Compositor/RDMRenderScope.h
	OpenOVR/Compositor/DensityMaskManager.cpp
	OpenOVR/Compositor/DensityMaskManager.h
	OpenOVR/Compositor/VRSGaze.h
	OpenOVR/Compositor/VRSPattern.h
	OpenOVR/Compositor/dx10compositor.cpp
	OpenOVR/Compositor/dx12compositor.cpp
	OpenOVR/Compositor/glcompositor.cpp
	OpenOVR/Compositor/glescompositor.cpp
	OpenOVR/Compositor/vkcompositor.cpp
	OpenOVR/convert.cpp
	OpenOVR/logging.cpp
	OpenOVR/linux_funcs.cpp
	OpenOVR/Misc/AudioOverride.cpp
	OpenOVR/Misc/smooth_input.cpp
	OpenOVR/Misc/Config.cpp
	OpenOVR/Misc/debug_helper.cpp
	OpenOVR/Misc/ExternalUpscalerConfig.h
	OpenOVR/Misc/ExternalUpscalerState.cpp
	OpenOVR/Misc/ExternalUpscalerState.h
	OpenOVR/Misc/MipBiasHook.cpp
	OpenOVR/Misc/MipBiasHook.h
	OpenOVR/Misc/OVRPerfHook.cpp
	OpenOVR/Misc/OVRPerfHook.h
	OpenOVR/Misc/NetworkTrackers.cpp
	OpenOVR/Misc/NetworkTrackers.h
	OpenOVR/Misc/CameraLegCalibration.cpp
	OpenOVR/Misc/CameraLegCalibration.h
	OpenOVR/Misc/WalkInPlace.cpp
	OpenOVR/Misc/WalkInPlace.h
	OpenOVR/Misc/xrutil.cpp
	OpenOVR/Misc/xrmoreutils.cpp
	OpenOVR/Misc/OneEuroFilterRotation.cpp
	OpenOVR/Misc/OneEuroFilterPosition.cpp
	OpenOVR/Misc/Keyboard/KeyboardLayout.cpp
	OpenOVR/Misc/Keyboard/SudoFontMeta.cpp
	OpenOVR/Misc/Keyboard/LaserRaySmoothing.cpp
	OpenOVR/Misc/Keyboard/LaserTextureAtlas.cpp
	OpenOVR/Misc/Keyboard/VRKeyboard.cpp
	OpenOVR/Misc/Keyboard/VRMenuLaser.cpp
	OpenOVR/Misc/Input/InteractionProfile.cpp
	OpenOVR/Misc/Input/OculusInteractionProfile.cpp
	OpenOVR/Misc/Input/KhrGenericInteractionProfile.cpp
	OpenOVR/Misc/Input/KhrSimpleInteractionProfile.cpp
	OpenOVR/Misc/Input/ViveInteractionProfile.cpp
	OpenOVR/Misc/Input/IndexControllerInteractionProfile.cpp
	OpenOVR/Misc/Input/HolographicInteractionProfile.cpp
	OpenOVR/Misc/Input/ReverbG2InteractionProfile.cpp
	OpenOVR/OpenOVR.cpp
	OpenOVR/Reimpl/BaseApplications.cpp
	OpenOVR/Reimpl/BaseChaperone.cpp
	OpenOVR/Reimpl/BaseChaperoneSetup.cpp
	OpenOVR/Reimpl/BaseClientCore.cpp
	OpenOVR/Reimpl/BaseCompositor.cpp
	OpenOVR/Reimpl/BaseExtendedDisplay.cpp
	OpenOVR/Reimpl/BaseInput.cpp
	OpenOVR/Reimpl/BaseInput_Hand.cpp
	OpenOVR/Reimpl/BaseInputInternal.cpp
	OpenOVR/Reimpl/BaseOverlay.cpp
	OpenOVR/Reimpl/BaseOverlayView.cpp
	OpenOVR/Reimpl/BaseRenderModels.cpp
	OpenOVR/Reimpl/BaseScreenshots.cpp
	OpenOVR/Reimpl/BaseSettings.cpp
	OpenOVR/Reimpl/BaseSystem.cpp
	OpenOVR/Reimpl/BaseMailbox.cpp
	OpenOVR/Reimpl/BaseControlPanel.cpp
	OpenOVR/Reimpl/CVRApplications.cpp
	OpenOVR/Reimpl/CVRChaperone.cpp
	OpenOVR/Reimpl/CVRChaperoneSetup.cpp
	OpenOVR/Reimpl/CVRClientCore.cpp
	OpenOVR/Reimpl/CVRCompositor.cpp
	OpenOVR/Reimpl/CVRExtendedDisplay.cpp
	OpenOVR/Reimpl/CVRInput.cpp
	OpenOVR/Reimpl/CVRInputInternal.cpp
	OpenOVR/Reimpl/CVROverlay.cpp
	OpenOVR/Reimpl/CVROverlayView.cpp
	OpenOVR/Reimpl/CVRRenderModels.cpp
	OpenOVR/Reimpl/CVRScreenshots.cpp
	OpenOVR/Reimpl/CVRSettings.cpp
	OpenOVR/Reimpl/CVRSystem.cpp
	OpenOVR/Reimpl/CVRMailbox.cpp
	OpenOVR/Reimpl/CVRControlPanel.cpp
	${GENERATED_DIR}/stubs.gen.cpp

	# Base classes
	OpenOVR/Reimpl/BaseServerDriverHost.cpp OpenOVR/Reimpl/BaseServerDriverHost.h

	# Definitions
	OpenOVR/Reimpl/CVRServerDriverHost.cpp

	# Exclude from the PCH requirement
	${OVR_PCH_EXCLUDED}

	# Headers
	OpenOVR/BaseCommon.h
	OpenOVR/Compositor/compositor.h
	OpenOVR/convert.h
	OpenOVR/custom_types.h
	OpenOVR/logging.h
	OpenOVR/Misc/AudioOverride.h
	OpenOVR/Misc/smooth_input.h
	OpenOVR/Misc/Config.h
	OpenOVR/Misc/debug_helper.h
	OpenOVR/Misc/ini.h
	OpenOVR/Misc/json/json-forwards.h
	OpenOVR/Misc/Keyboard/KeyboardLayout.h
	OpenOVR/Misc/Keyboard/SudoFontMeta.h
	OpenOVR/Misc/Keyboard/LaserRaySmoothing.h
	OpenOVR/Misc/Keyboard/VRKeyboard.h
	OpenOVR/Misc/Input/InteractionProfile.h
	OpenOVR/Misc/Input/OculusInteractionProfile.h
	OpenOVR/Misc/Input/KhrGenericInteractionProfile.h
	OpenOVR/Misc/Input/KhrSimpleInteractionProfile.h
	OpenOVR/Misc/lodepng.h
	OpenOVR/Misc/ScopeGuard.h
	OpenOVR/Reimpl/BaseApplications.h
	OpenOVR/Reimpl/BaseChaperone.h
	OpenOVR/Reimpl/BaseChaperoneSetup.h
	OpenOVR/Reimpl/BaseClientCore.h
	OpenOVR/Reimpl/BaseCompositor.h
	OpenOVR/Reimpl/BaseExtendedDisplay.h
	OpenOVR/Reimpl/BaseInput.h
	OpenOVR/Reimpl/BaseInputInternal.cpp
	OpenOVR/Reimpl/BaseOverlay.h
	OpenOVR/Reimpl/BaseOverlayView.h
	OpenOVR/Reimpl/BaseRenderModels.h
	OpenOVR/Reimpl/BaseScreenshots.h
	OpenOVR/Reimpl/BaseSettings.h
	OpenOVR/Reimpl/BaseSystem.h
	OpenOVR/Reimpl/BaseMailbox.h
	OpenOVR/Reimpl/BaseControlPanel.h
	OpenOVR/Reimpl/Interfaces.h
	${GENERATED_DIR}/static_bases.gen.h
	OpenOVR/resources.h
	OpenOVR/stdafx.h
	OpenOVR/steamvr_abi.h
	OpenOVR/targetver.h

	# Newly-added classes
	OpenOVR/Drivers/DriverManager.cpp
	OpenOVR/Drivers/DriverManager.h
	OpenOVR/Drivers/Backend.cpp
	OpenOVR/Drivers/Backend.h

	# While not actually part of OCCore, list all the OpenVR interfaces here so they are easily accessable in IDE
	OpenVRHeaders/custom_interfaces/IVRClientCore_002.h
	OpenVRHeaders/custom_interfaces/IVRClientCore_003.h
	OpenVRHeaders/custom_interfaces/IVRCompositor_017.h
	OpenVRHeaders/custom_interfaces/IVRCompositor_028.h
	OpenVRHeaders/custom_interfaces/IVROverlay_027.h
	OpenVRHeaders/custom_interfaces/IVROverlay_028.h
	OpenVRHeaders/custom_interfaces/IVRSystem_023.h
	OpenVRHeaders/custom_interfaces/IVRMailbox_001.h
	OpenVRHeaders/custom_interfaces/IVRInputInternal_002.h
	OpenVRHeaders/custom_interfaces/IVRControlPanel_006.h
	# Ensuring vrtypes.h exists should mean the rest of our generated interfaces exists
	${GENERATED_DIR}/interfaces/vrtypes.h
)
target_include_directories(OCCore PUBLIC OpenOVR ${CMAKE_BINARY_DIR})  # TODO make this private and put the public headers elsewhere
target_include_directories(OCCore PRIVATE BundledLibs OpenVRHeaders ${XrDir}/src/external/jsoncpp/include libs/eigen-3.4.0/Eigen)
target_compile_definitions(OCCore PRIVATE ${GRAPHICS_API_SUPPORT_FLAGS})

# NVAPI SDK (optional, NVIDIA VRS support)
if (WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/nvapi.h")
	target_include_directories(OCCore PRIVATE libs/nvapi)
	target_link_libraries(OCCore ${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
	target_compile_definitions(OCCore PRIVATE OC_HAS_NVAPI)
	message(STATUS "NVAPI SDK found — VRS support enabled")
else()
	message(STATUS "NVAPI SDK not found — VRS support disabled")
endif()

# AMD FidelityFX SDK (optional, FSR 3 temporal upscaling via DX12 interop)
if (WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/fidelityfx/api/include/ffx_api.h")
	target_include_directories(OCCore PRIVATE
		libs/fidelityfx/api/include
		libs/fidelityfx/upscalers/include)
	target_link_libraries(OCCore d3d12.lib dxgi.lib)
	target_compile_definitions(OCCore PRIVATE OC_HAS_FSR3)
	target_compile_definitions(DrvOpenXR PRIVATE OC_HAS_FSR3)
	message(STATUS "FidelityFX SDK found — FSR 3 temporal upscaling enabled")
else()
	message(STATUS "FidelityFX SDK not found — FSR 3 disabled (FSR 1 spatial still available)")
endif()

# NVIDIA DLSS 4 Super Resolution (optional, native DX11 NGX API)
if (WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/dlss/include/nvsdk_ngx.h")
	target_include_directories(OCCore PRIVATE libs/dlss/include)
	target_link_libraries(OCCore
		"${CMAKE_CURRENT_SOURCE_DIR}/libs/dlss/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
	target_compile_definitions(OCCore PRIVATE OC_HAS_DLSS)
	target_compile_definitions(DrvOpenXR PRIVATE OC_HAS_DLSS)
	message(STATUS "DLSS SDK found — DLSS 4 Super Resolution enabled")
else()
	message(STATUS "DLSS SDK not found — DLSS disabled")
endif()

target_link_libraries(OCCore OpenVR Vulkan ${XrLib} ${glmLib})

# Link MinHook for OVR perf stats hooking (Windows only)
if (WIN32)
	target_link_libraries(OCCore minhook shell32.lib)
	target_include_directories(OCCore PRIVATE libs/minhook/include)
endif()

if (NOT WIN32 AND NOT ANDROID)
	find_package(OpenGL REQUIRED) # for glGetError()
	target_link_libraries(OCCore ${OPENGL_LIBRARIES})
endif ()

# Set up precompiled headers for OCCore
target_precompile_headers(OCCore PRIVATE ${CMAKE_SOURCE_DIR}/OpenOVR/stdafx.h)
set_source_files_properties(${OVR_PCH_EXCLUDED} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

source_group(OpenVR REGULAR_EXPRESSION ${GENERATED_DIR}/interfaces)
source_group(OpenVR\\Drivers REGULAR_EXPRESSION ${GENERATED_DIR}/interfaces/driver_*)
source_group(OpenVR\\Custom REGULAR_EXPRESSION OpenVRHeaders/custom_interfaces/*)
source_group(Interfaces REGULAR_EXPRESSION OpenOVR/Reimpl/Base*)
source_group(Interfaces\\Declarations REGULAR_EXPRESSION OpenOVR/Reimpl/CVR*)

# === OCOVR ===
if (WIN32)
	set(OCOVR_PLAT_SRC windows.cpp)
else ()
	set(OCOVR_PLAT_SRC linux.cpp)
endif ()
set(OCOVR_SRC_ALL
	OCOVR/${OCOVR_PLAT_SRC}
	OCOVR/openvr_api.cpp
	OCOVR/openvr_api_0_9_12.cpp

	# Resources don't get nicely included in a library file
	${OVR_RESOURCES}
)

# On Android, build a static library so it can be linked into the target application
# This is required since we can't build the openxr loader on android.
if (ANDROID)
	add_library(OCOVR STATIC ${OCOVR_SRC_ALL})
else ()
	add_library(OCOVR SHARED ${OCOVR_SRC_ALL})
endif ()

# Require that all symbols are defined on Linux - this is always done on Windows
# Also on Linux, put all the static libraries in a group to avoid ordering problems
if (NOT WIN32)
	target_link_libraries(OCOVR PRIVATE -Wl,--no-undefined)
	target_link_libraries(OCOVR PUBLIC -Wl,--start-group OCCore DrvOpenXR -Wl,--end-group)
else ()
	# On Windows this isn't a problem since the linker knows how to figure this stuff out
	target_link_libraries(OCOVR OCCore DrvOpenXR)
endif ()

# If we're on Linux, compile everything as position-independent
if (WIN32)
else ()
	target_compile_options(DrvOpenXR PRIVATE -fPIC)
	if (NOT OpenXR_FOUND)
		target_compile_options(OpenXR PRIVATE -fPIC)
	endif ()
	target_compile_options(OCCore PRIVATE -fPIC)
endif ()

# The -Bsymbolic makes this work on exported functions - otherwise those will still use the first-defined one.
# See the comment at the top about -fvisibility=hidden
if (WIN32)
else ()
	target_link_options(OCOVR PRIVATE -Wl,-Bsymbolic -Wl,-Bsymbolic-functions)

	# Ensure all required symbols are actually found, don't produce a binary that can't be loaded
	target_link_options(OCOVR PRIVATE -z defs)

	# Oh, and we need dlopen
	target_link_libraries(OCOVR PUBLIC -ldl)
endif ()

# needed for generated files
target_include_directories(OCOVR PRIVATE OpenOVR/Reimpl)

# Required because the compositor header needs the platform-specific structures enabled via defines
target_compile_definitions(OCOVR PRIVATE ${GRAPHICS_API_SUPPORT_FLAGS})

find_package(Python REQUIRED Interpreter)

# glob for all openvr headers
file(GLOB openvr-headers CONFIGURE_DEPENDS LIST_DIRECTORIES false ${CMAKE_SOURCE_DIR}/OpenVRHeaders/openvr-*)

# glob for stub generator libraries
file(GLOB stub-deps CONFIGURE_DEPENDS LIST_DIRECTORIES false ${CMAKE_SOURCE_DIR}/scripts/stubs/*)

# glob for stub generator inputs
file(GLOB stub-inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false
	${CMAKE_SOURCE_DIR}/OpenOVR/Reimpl/CVR*.cpp
	${CMAKE_SOURCE_DIR}/OpenVRHeaders/custom_interfaces/*.h
	${CMAKE_SOURCE_DIR}/scripts/libparse.py
	)

# Command for running header splitting script
add_custom_command(
	OUTPUT ${GENERATED_DIR}/interfaces/vrtypes.h
	COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/split_headers.py ${CMAKE_SOURCE_DIR}/OpenVRHeaders ${GENERATED_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/scripts/split_headers.py ${openvr-headers}
	COMMENT "Splitting headers..."
	)

# Command for running stub generator
add_custom_command(
	OUTPUT ${GENERATED_DIR}/stubs.gen.cpp ${GENERATED_DIR}/static_bases.gen.h
	COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/stubs.py ${CMAKE_SOURCE_DIR}/OpenOVR/Reimpl ${CMAKE_SOURCE_DIR}/OpenVRHeaders ${GENERATED_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/scripts/stubs.py ${stub-deps} ${stub-inputs} ${GENERATED_DIR}/interfaces/vrtypes.h
	COMMENT "Generating stubs..."
	)

# Allows clean target to delete generated files too
set_property(
	TARGET OCCore
	APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${GENERATED_DIR};${CMAKE_BINARY_DIR}/bin)

if ( CMAKE_SIZEOF_VOID_P EQUAL 8) # 64 bit build - void pointer is 8 bits on 64 bit
	set(is_64bit_build true)
else()
	set(is_64bit_build false)
endif()

# Move OCOVR to appropriate directory expected by OpenVR and rename it to vrclient (or vrclient_x64)
# This enables marking the build directory as the OpenVR runtime path.
# For example, you can set the VR_OVERRIDE variable to the build dir (VR_OVERRIDE=<path to build dir>) when launching an app.
# see VR_LoadHmdSystemInternal() from openvr_api_public.cpp in the OpenVR repo for the expected paths.
if (WIN32)
	set(VRCLIENT_DIR bin)
	if ( is_64bit_build )
		set(VRCLIENT_NAME vrclient_x64)
	else()
		set(VRCLIENT_NAME vrclient)
	endif()
else ()
	set(VRCLIENT_NAME vrclient)
	if ( is_64bit_build )
		set(VRCLIENT_DIR bin/linux64)
	else()
		set(VRCLIENT_DIR bin)
	endif()
endif ()

# Windows puts DLLs in RUNTIME_OUTPUT_DIRECTORY for some reason
set_target_properties(OCOVR PROPERTIES
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${VRCLIENT_DIR}
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${VRCLIENT_DIR}
	LIBRARY_OUTPUT_NAME ${VRCLIENT_NAME}
	RUNTIME_OUTPUT_NAME ${VRCLIENT_NAME})

# ensure output directory exists
get_target_property(output_dir OCOVR LIBRARY_OUTPUT_DIRECTORY)
add_custom_command(TARGET OCOVR
	PRE_LINK COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir})

# Focused deterministic harness for the OSC frame/gait contract and WIP
# semantic transitions. It is excluded from normal builds and compiles the
# production sources through narrow test-only hooks (no UDP thread or sleeps).
if (WIN32)
	add_executable(OCUInputSessionRecoveryTest EXCLUDE_FROM_ALL tests/InputSessionRecoveryTests.cpp)
	target_include_directories(OCUInputSessionRecoveryTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR} ${XrDir}/include)
	set_target_properties(OCUInputSessionRecoveryTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCURuntimeSemanticsSelfTest EXCLUDE_FROM_ALL
		tests/RuntimeSemanticsSelfTest.cpp
		OpenOVR/Misc/NetworkTrackers.cpp
		OpenOVR/Misc/WalkInPlace.cpp)
	target_compile_definitions(OCURuntimeSemanticsSelfTest PRIVATE
		OCU_RUNTIME_SELF_TEST)
	target_include_directories(OCURuntimeSemanticsSelfTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCURuntimeSemanticsSelfTest PRIVATE ws2_32)
	set_target_properties(OCURuntimeSemanticsSelfTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

	add_executable(OCUVRSGazeSelfTest EXCLUDE_FROM_ALL
		tests/VRSGazeTests.cpp)
	target_include_directories(OCUVRSGazeSelfTest PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR})
	set_target_properties(OCUVRSGazeSelfTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

	add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/tests/FoveationConfigProduction.inl"
		COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractFoveationConfigTest.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/OpenOVR/Misc/Config.cpp" "${CMAKE_BINARY_DIR}/tests/FoveationConfigProduction.inl"
		DEPENDS tests/ExtractFoveationConfigTest.py OpenOVR/Misc/Config.cpp VERBATIM)
	add_executable(OCUFoveationConfigTest EXCLUDE_FROM_ALL tests/FoveationConfigTests.cpp
		"${CMAKE_BINARY_DIR}/tests/FoveationConfigProduction.inl")
	target_include_directories(OCUFoveationConfigTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tests")
	set_target_properties(OCUFoveationConfigTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

	add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/tests/FoveationGeometryProduction.inl"
		COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractFoveationGeometryTest.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/OpenOVR/Compositor/dx11compositor.cpp"
			"${CMAKE_BINARY_DIR}/tests/FoveationGeometryProduction.inl"
		DEPENDS tests/ExtractFoveationGeometryTest.py OpenOVR/Compositor/dx11compositor.cpp VERBATIM)
	add_executable(OCUFoveationGeometryTest EXCLUDE_FROM_ALL tests/FoveationGeometryTests.cpp
		OpenOVR/Misc/EffectFoveationState.cpp "${CMAKE_BINARY_DIR}/tests/FoveationGeometryProduction.inl")
	target_compile_definitions(OCUFoveationGeometryTest PRIVATE NOMINMAX)
	target_include_directories(OCUFoveationGeometryTest PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tests" "${XrDir}/include")
	target_link_libraries(OCUFoveationGeometryTest PRIVATE d3d11)
	set_target_properties(OCUFoveationGeometryTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

	add_executable(OCUEffectFoveationSelfTest EXCLUDE_FROM_ALL
		tests/EffectFoveationTests.cpp OpenOVR/Misc/EffectFoveationState.cpp)
	target_compile_definitions(OCUEffectFoveationSelfTest PRIVATE NOMINMAX)
	target_include_directories(OCUEffectFoveationSelfTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	set_target_properties(OCUEffectFoveationSelfTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/nvapi.h")
		add_executable(OCUDapaMaskVRSD3D11Test EXCLUDE_FROM_ALL
			tests/DapaMaskVRSInteractionTests.cpp OpenOVR/Compositor/VRSManager.cpp
			OpenOVR/Compositor/VRSShaderGuard.cpp)
		target_compile_definitions(OCUDapaMaskVRSD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUDapaMaskVRSD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi libs/minhook/include)
		target_link_libraries(OCUDapaMaskVRSD3D11Test PRIVATE OpenVR OpenXR glm d3d11 d3dcompiler dxgi minhook
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUDapaMaskVRSD3D11Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_executable(OCUVRSTerrainDepthD3D11Test EXCLUDE_FROM_ALL
			tests/VRSTerrainDepthD3D11Tests.cpp OpenOVR/Compositor/VRSManager.cpp
			OpenOVR/Compositor/VRSShaderGuard.cpp)
		target_compile_definitions(OCUVRSTerrainDepthD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUVRSTerrainDepthD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi libs/minhook/include)
		target_link_libraries(OCUVRSTerrainDepthD3D11Test PRIVATE OpenVR OpenXR glm d3d11 d3dcompiler dxgi minhook
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUVRSTerrainDepthD3D11Test PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_executable(OCUVRSAlphaCoverageD3D11Test EXCLUDE_FROM_ALL
			tests/VRSAlphaCoverageD3D11Tests.cpp OpenOVR/Compositor/VRSManager.cpp
			OpenOVR/Compositor/VRSShaderGuard.cpp OpenOVR/Compositor/VRSAlphaCoverageScope.cpp
			OpenOVR/Compositor/RDMRenderScope.cpp OpenOVR/Compositor/DensityMaskManager.cpp)
		target_compile_definitions(OCUVRSAlphaCoverageD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUVRSAlphaCoverageD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi libs/minhook/include)
		target_link_libraries(OCUVRSAlphaCoverageD3D11Test PRIVATE OpenVR OpenXR glm d3d11 d3dcompiler dxgi dxguid minhook
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUVRSAlphaCoverageD3D11Test PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_executable(OCUVRSSceneD3D11Test EXCLUDE_FROM_ALL
			tests/VRSSceneD3D11Tests.cpp OpenOVR/Compositor/VRSManager.cpp)
		target_compile_definitions(OCUVRSSceneD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUVRSSceneD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi)
		target_link_libraries(OCUVRSSceneD3D11Test PRIVATE OpenVR OpenXR glm d3d11 d3dcompiler dxgi
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUVRSSceneD3D11Test PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_executable(OCUVRSProjectionUploadD3D11Test EXCLUDE_FROM_ALL
			tests/VRSProjectionUploadD3D11Tests.cpp)
		target_compile_definitions(OCUVRSProjectionUploadD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUVRSProjectionUploadD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi)
		target_link_libraries(OCUVRSProjectionUploadD3D11Test PRIVATE OpenVR OpenXR glm d3d11 dxgi dxguid
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUVRSProjectionUploadD3D11Test PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
		add_executable(OCUVRSCoverageD3D11Test EXCLUDE_FROM_ALL
			tests/VRSCoverageD3D11Tests.cpp OpenOVR/Compositor/VRSManager.cpp
			OpenOVR/Compositor/VRSShaderGuard.cpp)
		target_compile_definitions(OCUVRSCoverageD3D11Test PRIVATE NOMINMAX OC_HAS_NVAPI)
		target_include_directories(OCUVRSCoverageD3D11Test PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR} OpenOVR ${CMAKE_BINARY_DIR} libs/nvapi libs/minhook/include)
		target_link_libraries(OCUVRSCoverageD3D11Test PRIVATE OpenVR OpenXR glm d3d11 d3dcompiler dxgi minhook
			${CMAKE_CURRENT_SOURCE_DIR}/libs/nvapi/amd64/nvapi64.lib)
		set_target_properties(OCUVRSCoverageD3D11Test PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	endif()
	add_executable(OCUVRSTerrainGuardTest EXCLUDE_FROM_ALL
		tests/VRSTerrainGuardTests.cpp OpenOVR/Compositor/VRSShaderGuard.cpp)
	target_compile_definitions(OCUVRSTerrainGuardTest PRIVATE NOMINMAX OCU_VRS_GUARD_SELF_TEST)
	target_include_directories(OCUVRSTerrainGuardTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} libs/minhook/include)
	target_link_libraries(OCUVRSTerrainGuardTest PRIVATE d3d11 d3dcompiler dxgi minhook)
	set_target_properties(OCUVRSTerrainGuardTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

    add_executable(OCURDMHandoffD3D11Test EXCLUDE_FROM_ALL
        tests/RDMHandoffD3D11Tests.cpp OpenOVR/Compositor/RDMRenderScope.cpp
        OpenOVR/Compositor/DensityMaskManager.cpp OpenOVR/Compositor/VRSShaderGuard.cpp)
    target_compile_definitions(OCURDMHandoffD3D11Test PRIVATE NOMINMAX)
    target_include_directories(OCURDMHandoffD3D11Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} libs/minhook/include)
    target_link_libraries(OCURDMHandoffD3D11Test PRIVATE d3d11 d3dcompiler dxgi dxguid minhook)
    set_target_properties(OCURDMHandoffD3D11Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	# The producer lives in the sibling SKSE project. Extract its real lifecycle
	# functions instead of duplicating a mock of their clear/publication policy.
	set(OCU_DAPA_MASK_PRODUCER_SOURCE
		"${CMAKE_CURRENT_SOURCE_DIR}/../OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/src/DapaPlayerMask.inl"
		CACHE FILEPATH "SKSE player-mask source for the DAPA lifecycle regression")
	if(EXISTS "${OCU_DAPA_MASK_PRODUCER_SOURCE}")
		add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/tests/DapaMaskLifecycleProduction.inc"
			COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractDapaMaskLifecycleTest.py"
				"${CMAKE_CURRENT_SOURCE_DIR}/OpenOVR/Compositor/dx11compositor.cpp"
				"${OCU_DAPA_MASK_PRODUCER_SOURCE}" "${CMAKE_BINARY_DIR}/tests/DapaMaskLifecycleProduction.inc"
			DEPENDS tests/ExtractDapaMaskLifecycleTest.py OpenOVR/Compositor/dx11compositor.cpp
				"${OCU_DAPA_MASK_PRODUCER_SOURCE}" VERBATIM)
		add_executable(OCUDapaMaskLifecycleD3D11Test EXCLUDE_FROM_ALL
			tests/DapaMaskLifecycleD3D11Tests.cpp "${CMAKE_BINARY_DIR}/tests/DapaMaskLifecycleProduction.inc")
		target_compile_definitions(OCUDapaMaskLifecycleD3D11Test PRIVATE NOMINMAX)
		target_include_directories(OCUDapaMaskLifecycleD3D11Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_BINARY_DIR}/tests")
		target_link_libraries(OCUDapaMaskLifecycleD3D11Test PRIVATE d3d11 d3dcompiler)
		set_target_properties(OCUDapaMaskLifecycleD3D11Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	endif()
    add_executable(OCURDMPackedResolveTest EXCLUDE_FROM_ALL tests/RDMPackedResolveTests.cpp OpenOVR/Compositor/DensityMaskManager.cpp)
    target_compile_definitions(OCURDMPackedResolveTest PRIVATE NOMINMAX)
    target_include_directories(OCURDMPackedResolveTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(OCURDMPackedResolveTest PRIVATE d3d11 d3dcompiler dxgi dxguid)
    set_target_properties(OCURDMPackedResolveTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDensityMaskD3D11Test EXCLUDE_FROM_ALL
		tests/DensityMaskD3D11Tests.cpp
		OpenOVR/Compositor/DensityMaskManager.cpp)
	target_compile_definitions(OCUDensityMaskD3D11Test PRIVATE NOMINMAX)
	target_include_directories(OCUDensityMaskD3D11Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCUDensityMaskD3D11Test PRIVATE d3d11 d3dcompiler dxgi dxguid)
	set_target_properties(OCUDensityMaskD3D11Test PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUFoveationDebugOverlayTest EXCLUDE_FROM_ALL
		tests/FoveationDebugOverlayTests.cpp DrvOpenXR/FoveationDebugOverlay.cpp)
	target_compile_definitions(OCUFoveationDebugOverlayTest PRIVATE NOMINMAX SUPPORT_DX11)
	target_include_directories(OCUFoveationDebugOverlayTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${XrDir}/include)
	target_link_libraries(OCUFoveationDebugOverlayTest PRIVATE d3d11)
	set_target_properties(OCUFoveationDebugOverlayTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/tests/EyeGazeSamplerProduction.inc"
		COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractEyeGazeSamplerTest.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/OpenOVR/Reimpl/BaseInput.cpp"
			"${CMAKE_CURRENT_SOURCE_DIR}/OpenOVR/Misc/xrutil.cpp" "${CMAKE_BINARY_DIR}/tests/EyeGazeSamplerProduction.inc"
		DEPENDS tests/ExtractEyeGazeSamplerTest.py OpenOVR/Reimpl/BaseInput.cpp OpenOVR/Misc/xrutil.cpp VERBATIM)
	add_executable(OCUEyeGazeSamplerTest EXCLUDE_FROM_ALL
		tests/EyeGazeSamplerTests.cpp "${CMAKE_BINARY_DIR}/tests/EyeGazeSamplerProduction.inc")
	target_include_directories(OCUEyeGazeSamplerTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${XrDir}/include "${CMAKE_BINARY_DIR}/tests")
	set_target_properties(OCUEyeGazeSamplerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDapaTimingTest EXCLUDE_FROM_ALL tests/DapaTimingTests.cpp)
	set(OCU_DAPA_MENU_GENERATED
		"${CMAKE_BINARY_DIR}/tests/DapaMenuBridge.inc" "${CMAKE_BINARY_DIR}/tests/DapaMenuProvider.inc"
		"${CMAKE_BINARY_DIR}/tests/DapaMenuCacheAdmission.inc" "${CMAKE_BINARY_DIR}/tests/DapaMenuScheduler.inc")
	add_custom_command(OUTPUT ${OCU_DAPA_MENU_GENERATED}
		COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractDapaMenuTest.py"
			"${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tests"
		DEPENDS tests/ExtractDapaMenuTest.py OpenOVR/Compositor/dx11compositor.cpp
			DrvOpenXR/ASWProvider.h DrvOpenXR/ASWProvider.cpp DrvOpenXR/XrBackend.cpp VERBATIM)
	add_executable(OCUDapaMenuTest EXCLUDE_FROM_ALL tests/DapaMenuTests.cpp ${OCU_DAPA_MENU_GENERATED})
	target_include_directories(OCUDapaMenuTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${XrDir}/include "${CMAKE_BINARY_DIR}/tests")
	set_target_properties(OCUDapaMenuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDapaGpuTimingTest EXCLUDE_FROM_ALL tests/DapaGpuTimingTests.cpp)
	target_compile_definitions(OCUDapaGpuTimingTest PRIVATE NOMINMAX)
	target_include_directories(OCUDapaGpuTimingTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCUDapaGpuTimingTest PRIVATE d3d11)
	set_target_properties(OCUDapaGpuTimingTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDapaDepthTransferTest EXCLUDE_FROM_ALL tests/DapaDepthTransferTests.cpp)
	target_compile_definitions(OCUDapaDepthTransferTest PRIVATE NOMINMAX)
	target_include_directories(OCUDapaDepthTransferTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCUDapaDepthTransferTest PRIVATE d3d11)
	set_target_properties(OCUDapaDepthTransferTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCULoggingPolicyTest EXCLUDE_FROM_ALL tests/LoggingPolicyTests.cpp)
	target_include_directories(OCULoggingPolicyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	set_target_properties(OCULoggingPolicyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDapaReplay EXCLUDE_FROM_ALL tests/DapaReplay.cpp
		${XrDir}/src/external/jsoncpp/src/lib_json/json_reader.cpp
		${XrDir}/src/external/jsoncpp/src/lib_json/json_value.cpp
		${XrDir}/src/external/jsoncpp/src/lib_json/json_writer.cpp)
	target_compile_definitions(OCUDapaReplay PRIVATE NOMINMAX)
	target_include_directories(OCUDapaReplay PRIVATE ${XrDir}/src/external/jsoncpp/include)
	target_link_libraries(OCUDapaReplay PRIVATE d3d11 d3dcompiler windowscodecs ole32)
	set_target_properties(OCUDapaReplay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	add_executable(OCUDapaMotionTest EXCLUDE_FROM_ALL tests/DapaMotionTests.cpp)
	add_executable(OCUDapaCaptureTest EXCLUDE_FROM_ALL tests/DapaCaptureTests.cpp DrvOpenXR/DapaCapture.cpp)
	target_compile_definitions(OCUDapaCaptureTest PRIVATE NOMINMAX OCU_DAPA_CAPTURE=1)
	target_include_directories(OCUDapaCaptureTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCUDapaCaptureTest PRIVATE d3d11 d3dcompiler windowscodecs ole32 shell32)
	set_target_properties(OCUDapaCaptureTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	if(NOT OCU_DAPA_CAPTURE)
		add_executable(OCUDapaCaptureShippingTest EXCLUDE_FROM_ALL tests/DapaCaptureShippingTests.cpp DrvOpenXR/DapaCapture.cpp)
		target_compile_definitions(OCUDapaCaptureShippingTest PRIVATE NOMINMAX)
		target_include_directories(OCUDapaCaptureShippingTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
		target_link_libraries(OCUDapaCaptureShippingTest PRIVATE d3d11 d3dcompiler windowscodecs ole32 shell32)
		set_target_properties(OCUDapaCaptureShippingTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	endif()
	target_compile_definitions(OCUDapaMotionTest PRIVATE NOMINMAX)
	target_include_directories(OCUDapaMotionTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(OCUDapaMotionTest PRIVATE d3d11 d3dcompiler)
	set_target_properties(OCUDapaMotionTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
	target_compile_definitions(OCUDapaTimingTest PRIVATE NOMINMAX)
	target_include_directories(OCUDapaTimingTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${XrDir}/include)
	set_target_properties(OCUDapaTimingTest PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()
