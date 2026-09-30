cmake_minimum_required(VERSION 3.21)

project(
    OpenCompositeInput
    VERSION 5.0.3
    DESCRIPTION "SKSE VR plugin for OpenComposite - bridges keyboard input to Scaleform"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force VR-only build for CommonLibSSE (no SE/AE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)

# Locate CommonLibVR without embedding a developer-specific absolute path.
set(COMMONLIBSSE_ROOT "" CACHE PATH "Path to the CommonLibSSE-NG/CommonLibVR source tree")
if(NOT COMMONLIBSSE_ROOT AND DEFINED ENV{COMMONLIBSSE_ROOT})
    set(COMMONLIBSSE_ROOT "$ENV{COMMONLIBSSE_ROOT}")
endif()
if(NOT COMMONLIBSSE_ROOT)
    get_filename_component(
        COMMONLIBSSE_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../VR-Project-Staging/external/commonlibsse-ng"
        ABSOLUTE
    )
endif()
if(NOT EXISTS "${COMMONLIBSSE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "CommonLibSSE-NG/CommonLibVR was not found. Set COMMONLIBSSE_ROOT "
        "with -DCOMMONLIBSSE_ROOT=<path> or the COMMONLIBSSE_ROOT environment variable."
    )
endif()

# Add CommonLibVR as a subdirectory
add_subdirectory(
    "${COMMONLIBSSE_ROOT}"
    CommonLibSSE
    EXCLUDE_FROM_ALL
)

# Include the plugin helper macro from CommonLibVR
include("${COMMONLIBSSE_ROOT}/cmake/CommonLibSSE.cmake")

# MinHook for the standalone existing-inline-hook compatibility test.
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../OpenCompositeSkyrimVR/libs/minhook")
add_library(minhook_skse STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(minhook_skse PUBLIC ${MINHOOK_DIR}/include)

# Plugin source files
set(SOURCES
    src/Main.cpp
    src/SkyrimMenuTargets.cpp
    src/DapaHiggs.cpp
    src/DapaSpellWheel.cpp
    src/DapaCrossbow.cpp
)

# Create the SKSE plugin DLL
add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES ${SOURCES}
    NAME "OpenCompositeInput"
    AUTHOR "Wondernuttz"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
)

# Use our wrapper PCH (includes CommonLibVR's PCH + global using declarations)
target_precompile_headers(${PROJECT_NAME}
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h"
)

# MSVC-specific options for the plugin
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /permissive-
        /Zc:preprocessor
        /EHsc
        /W4
        /bigobj
    )
endif()

# Explicitly invoked diagnostic; never shipped as part of the plugin.
add_executable(DapaHookStartupTest EXCLUDE_FROM_ALL tests/DapaHookStartupTest.cpp)
target_link_libraries(DapaHookStartupTest PRIVATE minhook_skse d3d11 dxgi)
add_executable(DapaEngineDrawTest EXCLUDE_FROM_ALL tests/DapaEngineDrawTest.cpp)
add_executable(DapaHiggsTest EXCLUDE_FROM_ALL tests/DapaHiggsTest.cpp)
add_executable(DapaSpellWheelTest EXCLUDE_FROM_ALL tests/DapaSpellWheelTest.cpp)
add_executable(DapaGeometryOwnershipTest EXCLUDE_FROM_ALL tests/DapaGeometryOwnershipTest.cpp)
add_executable(DapaCsxDrawTest EXCLUDE_FROM_ALL tests/DapaCsxDrawTest.cpp)
add_executable(DapaCsxApiTest EXCLUDE_FROM_ALL tests/DapaCsxApiTest.cpp)
add_executable(RaceMenuKeyboardRecoveryTest EXCLUDE_FROM_ALL tests/RaceMenuKeyboardRecoveryTest.cpp)
add_executable(MCMLaserTest EXCLUDE_FROM_ALL tests/MCMLaserTest.cpp)
add_executable(LaserMenuHitTest EXCLUDE_FROM_ALL tests/LaserMenuHitTest.cpp)
add_executable(RaceMenuNativeInputTest EXCLUDE_FROM_ALL tests/RaceMenuNativeInputTest.cpp)
target_link_libraries(RaceMenuNativeInputTest PRIVATE CommonLibSSE::CommonLibSSE)
target_precompile_headers(RaceMenuNativeInputTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h")

# Exercise the actual producer helpers with instrumented engine/COM fixtures.
# Python is needed only when explicitly building these diagnostics.
foreach(diagnostic IN ITEMS BridgeRefresh MenuActivity BridgeMaintenance BridgeStartup JournalFocus)
    set(extractor "${CMAKE_CURRENT_SOURCE_DIR}/tests/Extract${diagnostic}Test.py")
    if(diagnostic STREQUAL "BridgeMaintenance")
        set(extractor "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractBridgeMaintenance.py")
    endif()
    set(extracted "${CMAKE_CURRENT_BINARY_DIR}/tests/${diagnostic}Production.inl")
    add_custom_command(OUTPUT "${extracted}"
        COMMAND ${CMAKE_COMMAND}
            -DEXTRACTOR=${extractor}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp
            -DOUTPUT=${extracted}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/RunProductionExtractor.cmake
        DEPENDS src/Main.cpp "${extractor}" tests/RunProductionExtractor.cmake
        VERBATIM)
    add_executable(${diagnostic}Test EXCLUDE_FROM_ALL "tests/${diagnostic}Test.cpp" "${extracted}")
    target_include_directories(${diagnostic}Test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tests")
endforeach()
target_link_libraries(DapaCsxDrawTest PRIVATE bcrypt d3d11 d3dcompiler dxgi)


# Standalone D3D11 ownership checks; this target does not link CommonLib.
set(menu_target_extractor "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExtractSkyrimMenuTargetsTest.py")
set(menu_target_extracted "${CMAKE_CURRENT_BINARY_DIR}/tests/SkyrimMenuTargetsProduction.inl")
add_custom_command(OUTPUT "${menu_target_extracted}"
    COMMAND ${CMAKE_COMMAND}
        -DEXTRACTOR=${menu_target_extractor}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/SkyrimMenuTargets.cpp
        -DOUTPUT=${menu_target_extracted}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/RunProductionExtractor.cmake
    DEPENDS src/SkyrimMenuTargets.cpp "${menu_target_extractor}"
    VERBATIM)
add_executable(SkyrimMenuTargetsTest EXCLUDE_FROM_ALL
    tests/SkyrimMenuTargetsTest.cpp "${menu_target_extracted}")
target_include_directories(SkyrimMenuTargetsTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tests")
target_link_libraries(SkyrimMenuTargetsTest PRIVATE d3d11)

target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "OpenCompositeInput"
    SUFFIX ".dll"
)
