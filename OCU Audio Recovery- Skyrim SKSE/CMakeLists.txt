cmake_minimum_required(VERSION 3.21)

project(
    OCUAudioRecovery
    VERSION 0.2.1
    DESCRIPTION "Skyrim VR audio endpoint recovery after headset reconnect"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /d1trimfile:\"${CMAKE_SOURCE_DIR}/\"")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /PDBALTPATH:%_PDB%")
    set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /PDBALTPATH:%_PDB%")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)

set(COMMONLIBSSE_ROOT "" CACHE PATH "Path to CommonLibVR/CommonLibSSE-NG")
if(NOT COMMONLIBSSE_ROOT AND DEFINED ENV{COMMONLIBSSE_ROOT})
    set(COMMONLIBSSE_ROOT "$ENV{COMMONLIBSSE_ROOT}")
endif()
if(NOT COMMONLIBSSE_ROOT OR NOT EXISTS "${COMMONLIBSSE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibVR was not found. Set COMMONLIBSSE_ROOT.")
endif()

add_subdirectory("${COMMONLIBSSE_ROOT}" CommonLibSSE EXCLUDE_FROM_ALL)
include("${COMMONLIBSSE_ROOT}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES src/Main.cpp
    NAME "OCU Audio Recovery"
    AUTHOR "Wondernuttz"
    VERSION ${PROJECT_VERSION}
    USE_ADDRESS_LIBRARY
)

target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h")
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(${PROJECT_NAME} PRIVATE ole32)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /permissive- /Zc:preprocessor /EHsc /W4 /bigobj)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "OCUAudioRecovery"
    SUFFIX ".dll"
)
