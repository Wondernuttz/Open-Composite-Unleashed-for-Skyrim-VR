"""Exercise production HMD-presence code with a fake OpenXR runtime.

No headset, loader, rendering libraries, or game process is used.
"""
from pathlib import Path
import argparse
import subprocess


def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--cmake', default='cmake')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
driver = (root / 'DrvOpenXR/DrvOpenXR.cpp').read_text(encoding='utf-8')
api = (root / 'OCOVR/openvr_api.cpp').read_text(encoding='utf-8')
fixture = (root / 'tests/HmdPresenceMock.cpp.in').read_text(encoding='utf-8')
fixture = fixture.replace('/*PRESENCE*/', function(driver, 'bool DrvOpenXR::IsHmdPresent()'))
fixture = fixture.replace('/*OPENVR_ENTRY*/', function(api, 'VR_INTERFACE bool VR_CALLTYPE VR_IsHmdPresent()'))
fixture = fixture.replace('/*SHUTDOWN*/', function(driver, 'void DrvOpenXR::FullShutdown()'))
fixture = fixture.replace('/*SHUTDOWN_GUARD*/', function(api, 'static unsigned long ShutdownInternalGuarded()'))
fixture = fixture.replace('/*OPENVR_SHUTDOWN*/', function(api, 'VR_INTERFACE void VR_CALLTYPE VR_ShutdownInternal()'))
# Verify lifecycle publication is wired to the actual backend creation path.
creation = function(driver, 'IBackend* DrvOpenXR::CreateOpenXRBackend()')
assert creation.index('HmdPresenceState::RuntimeCall') < creation.index('xrCreateInstance(')
assert creation.index('hmdPresence.Publish(true)') > creation.index('CreateSystemID();')
entry_start = creation.index('{') + 1
entry_end = creation.index('OOVR_LOG("OCU runtime build:')
fixture = fixture.replace('/*BACKEND_ENTRY*/', creation[entry_start:entry_end])
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'mock.cpp').write_text(fixture, encoding='utf-8')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCUHmdPresenceMock LANGUAGES CXX)
add_executable(HmdPresenceMock mock.cpp)
target_compile_features(HmdPresenceMock PRIVATE cxx_std_17)
target_include_directories(HmdPresenceMock PRIVATE "{root.as_posix()}" "{root.as_posix()}/libs/openxr-sdk/include")
''', encoding='utf-8')
subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build'), '-A', 'x64'], check=True)
subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
subprocess.run([str(out / 'build/Release/HmdPresenceMock.exe')], check=True, timeout=20)
