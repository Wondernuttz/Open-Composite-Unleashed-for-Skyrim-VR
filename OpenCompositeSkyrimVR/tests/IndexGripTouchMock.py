"""Compile current production input functions against an OpenXR mock.

Usage: python tests/IndexGripTouchMock.py --build-dir <scratch directory>
The fixture substitutes the runtime/devices, not OCU's binding or state code.
It cannot validate physical Index sensor thresholds or HIGGS inside Skyrim.
"""
from pathlib import Path
import argparse, re, subprocess

repo = Path(__file__).resolve().parents[1]

def block(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    for end in range(opening + 1, len(text)):
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        if not depth: return text[start:end+1]
    raise ValueError(signature)

args = argparse.ArgumentParser()
args.add_argument('--build-dir', required=True, type=Path)
args.add_argument('--cmake', default=r'C:\Program Files\CMake\bin\cmake.exe')
options = args.parse_args()
out = options.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
base = (repo/'OpenOVR/Reimpl/BaseInput.cpp').read_text()
profile = (repo/'OpenOVR/Misc/Input/InteractionProfile.cpp').read_text()
schema = (repo/'OpenOVR/Misc/Input/InteractionProfile.h').read_text()
legacy = (repo/'OpenOVR/Misc/Input/LegacyControllerActions.h').read_text()
fixture = (repo/'tests/IndexGripTouchMock.cpp.in').read_text()
profiles = ''
for key, filename in [('index', 'IndexControllerInteractionProfile.cpp'), ('touch', 'OculusInteractionProfile.cpp'), ('vive', 'ViveInteractionProfile.cpp')]:
    content = (repo/'OpenOVR/Misc/Input'/filename).read_text()
    # The tested grip suggestions come from each production profile, not a test copy.
    assignments = re.findall(r'(?:this->bindingsLegacy|bindings)\.(grip(?:Click|Touch)?)\s*=\s*("[^"]+");', content)
    assert assignments, filename
    profiles += f'if (kind == "{key}") {{\n'
    profiles += ''.join(f'paths.{field} = {value};\n' for field, value in assignments)
    profiles += '}\n'
fixture = fixture.replace('/*ACTION_STRUCT*/', block(legacy, 'struct LegacyControllerActions')+';')
fixture = fixture.replace('/*BINDING_STRUCT*/', block(schema, 'struct LegacyBindings')+';')
fixture = fixture.replace('/*PROFILE_ASSIGNMENTS*/', profiles)
fixture = fixture.replace('/*BINDINGS*/', block(profile, 'void InteractionProfile::AddLegacyBindings'))
fixture = fixture.replace('/*CREATE_ACTIONS*/', block(base, 'void BaseInput::CreateLegacyActions'))
fixture = fixture.replace('/*CONTROLLER_STATE*/', block(base, 'bool BaseInput::GetLegacyControllerState'))
fixture = fixture.replace('/*DEVICE_ROUTING*/', block(base, 'int BaseInput::DeviceIndexToHandId'))
fixture = fixture.replace('/*HAPTIC_PULSE*/', block(base, 'void BaseInput::TriggerLegacyHapticPulse'))
smooth = (repo/'OpenOVR/Misc/smooth_input.cpp').read_text()
fixture += '\n' + re.sub(r'^#include[^\n]*', '', smooth, flags=re.M)
(out/'mock.cpp').write_text(fixture)
(out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCUIndexGripMock LANGUAGES CXX)
add_executable(IndexGripMock mock.cpp)
target_compile_features(IndexGripMock PRIVATE cxx_std_17)
target_compile_definitions(IndexGripMock PRIVATE NOMINMAX)
target_include_directories(IndexGripMock PRIVATE "{repo.as_posix()}" "{repo.as_posix()}/build" "{repo.as_posix()}/libs/openxr-sdk/include")
''')
subprocess.run([options.cmake, '-S', str(out), '-B', str(out/'build'), '-A', 'x64'], check=True)
subprocess.run([options.cmake, '--build', str(out/'build'), '--config', 'Release'], check=True)
subprocess.run([str(out/'build/Release/IndexGripMock.exe')], check=True)
