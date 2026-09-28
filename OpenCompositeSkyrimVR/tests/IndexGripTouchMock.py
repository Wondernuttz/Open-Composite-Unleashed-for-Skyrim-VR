"""Compile current production input functions against an OpenXR mock.

Usage: python tests/IndexGripTouchMock.py --build-dir <scratch directory>
The fixture substitutes the runtime/devices, not OCU's binding or state code.
--spellwheel-source accepts an author-source SpellWheelVR.cpp for an optional
consumer-contract check without copying that implementation into this repository.
It cannot validate physical Index sensor thresholds or mods inside Skyrim.
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
args.add_argument('--spellwheel-source', type=Path)
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
    # Index face/trackpad and grip suggestions come from production profiles.
    fields = r'grip(?:Click|Touch)?'
    if key == 'index':
        fields += r'|menu(?:Touch)?|btnA(?:Touch)?|trackPad(?:X|Y|Click|Touch)|stick(?:X|Y|Btn|BtnTouch)'
    assignments = re.findall(r'(?:this->bindingsLegacy|bindings)\.(' + fields + r')\s*=\s*("[^"]+");', content)
    assert assignments, filename
    profiles += f'if (kind == "{key}") {{\n'
    profiles += ''.join(f'paths.{field} = {value};\n' for field, value in assignments)
    profiles += '}\n'
fixture = fixture.replace('/*ACTION_STRUCT*/', block(legacy, 'struct LegacyControllerActions')+';')
fixture = fixture.replace('/*BINDING_STRUCT*/', block(schema, 'struct LegacyBindings')+';')
fixture = fixture.replace('/*PROFILE_ASSIGNMENTS*/', profiles)
frame = (repo/'OpenOVR/Misc/Input/FrameInteractionProfile.cpp').read_text()
fixture = fixture.replace('/*FRAME_BINDINGS*/', block(frame, 'const InteractionProfile::LegacyBindings* FrameInteractionProfile::GetLegacyBindings').replace('FrameInteractionProfile::GetLegacyBindings', 'InteractionProfile::GetFrameLegacyBindings'))
fixture = fixture.replace('/*FRAME_TESTS*/', (repo/'tests/FrameControllerTests.inc').read_text())
fixture = fixture.replace('/*BINDINGS*/', block(profile, 'void InteractionProfile::AddLegacyBindings'))
fixture = fixture.replace('/*CREATE_ACTIONS*/', block(base, 'void BaseInput::CreateLegacyActions'))
fixture = fixture.replace('/*CONTROLLER_STATE*/', block(base, 'XrResult BaseInput::SuggestBindingsWithIndexGrip')+'\n'+block(base, 'bool BaseInput::GetLegacyControllerState'))
fixture = fixture.replace('/*DEVICE_ROUTING*/', block(base, 'int BaseInput::DeviceIndexToHandId'))
fixture = fixture.replace('/*HAPTIC_PULSE*/', block(base, 'void BaseInput::TriggerLegacyHapticPulse'))
consumer = ''
if options.spellwheel_source:
    source = options.spellwheel_source.read_text(encoding='utf-8-sig')
    assignments = []
    for name in ('buttonMask', 'buttonCombinationMask'):
        matches = re.findall(r'^\s*' + name + r'\s*=\s*HoldButton[^;]+;', source, re.M)
        assert len(matches) == 1, f'Expected one {name} assignment in Spell Wheel source'
        assignments.append(matches[0])
    consumer = '''#define OCU_TEST_SPELLWHEEL_SOURCE 1
namespace SpellWheelConsumer {
int HoldButton=99, HoldButtonCombination=-1;
uint64_t buttonMask=0, buttonCombinationMask=0;
uint64_t GetButtonMaskFromId(int id) { return uint64_t{1} << id; }
void Configure(int button, int combination) {
    HoldButton=button; HoldButtonCombination=combination;
''' + '\n'.join(assignments) + '\n}\n' + block(source, 'bool MainHandButtonsArePressed') + '\n}\n'
fixture = fixture.replace('/*SPELLWHEEL_PREDICATE*/', consumer)
smooth = (repo/'OpenOVR/Misc/smooth_input.cpp').read_text()
fixture += '\n' + re.sub(r'^#include[^\n]*', '', smooth, flags=re.M)
(out/'mock.cpp').write_text(fixture)
(out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCUIndexGripMock LANGUAGES CXX)
add_executable(IndexGripMock mock.cpp)
target_compile_features(IndexGripMock PRIVATE cxx_std_17)
target_compile_definitions(IndexGripMock PRIVATE NOMINMAX)
target_include_directories(IndexGripMock PRIVATE "{repo.as_posix()}" "{repo.as_posix()}/tests" "{repo.as_posix()}/build" "{repo.as_posix()}/libs/openxr-sdk/include")
''')
subprocess.run([options.cmake, '-S', str(out), '-B', str(out/'build'), '-A', 'x64'], check=True)
subprocess.run([options.cmake, '--build', str(out/'build'), '--config', 'Release'], check=True)
subprocess.run([str(out/'build/Release/IndexGripMock.exe')], check=True)
