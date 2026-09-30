"""Execute the production generic-interface startup guard with optional errors.

The fixture extracts the function prefix through its initialized-state error
handling. Interface creation and return-address diagnostics are not under test.
Only the C++ standard library and Windows API are used by the generated build.
"""
from pathlib import Path
import argparse
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--cmake', default=r'C:\Program Files\CMake\bin\cmake.exe')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = (repo / 'OCOVR/openvr_api.cpp').read_text(encoding='utf-8')
start = source.index('VR_INTERFACE void* VR_CALLTYPE VR_GetGenericInterface(')
end = source.index("\t// First check if they're getting the 'FnTable'", start)
prefix = source[start:end]
fixture = r'''
#include <windows.h>
#include <intrin.h>
#include <iostream>
#include <stdexcept>
#define VR_INTERFACE
#define VR_CALLTYPE
#define OOVR_LOGF(...) do {} while (0)
enum EVRInitError { VRInitError_None=0, VRInitError_Init_NotInitialized=109 };
bool running=false;
int initializedContinuation=0;
/*PRODUCTION_PREFIX*/
    ++initializedContinuation;
    return &initializedContinuation;
}
void Check(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char* version="IVRSystem_019";
    EVRInitError error=VRInitError_None;
    Check(VR_GetGenericInterface(version,&error)==nullptr,
        "before initialization an interface must be null");
    Check(error==VRInitError_Init_NotInitialized,
        "before initialization the provided error must be set");
    Check(VR_GetGenericInterface(version,nullptr)==nullptr,
        "before initialization the optional error may be omitted");
    Check(initializedContinuation==0,
        "before initialization no interface creation may execute");
    running=true;
    Check(VR_GetGenericInterface(version,&error)==&initializedContinuation,
        "initialized calls must continue normally");
    Check(error==VRInitError_None,
        "initialized calls must clear a provided error");
    Check(VR_GetGenericInterface(version,nullptr)==&initializedContinuation,
        "initialized calls must still permit an omitted error");
    Check(initializedContinuation==2,
        "both initialized calls must reach normal interface processing");
    std::cout << "PASS: 8 generic-interface startup/optional-error checks\n";
}
'''.replace('/*PRODUCTION_PREFIX*/', prefix)
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'mock.cpp').write_text(fixture, encoding='utf-8')
(out / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(OCUGenericInterfaceStartupMock LANGUAGES CXX)
add_executable(GenericInterfaceStartupMock mock.cpp)
target_compile_features(GenericInterfaceStartupMock PRIVATE cxx_std_17)
''', encoding='utf-8')
subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build'), '-A', 'x64'], check=True)
subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
subprocess.run([str(out / 'build/Release/GenericInterfaceStartupMock.exe')], check=True)
