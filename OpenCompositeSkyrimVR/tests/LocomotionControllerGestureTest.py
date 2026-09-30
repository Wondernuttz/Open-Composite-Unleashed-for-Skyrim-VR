"""Compile the production KAT controller shortcut with OpenXR action mocks."""
from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--cmake', default=r'C:\Program Files\CMake\bin\cmake.exe')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (repo / 'OpenOVR/Reimpl/BaseInput.cpp').read_text(encoding='utf-8-sig')
start = source.index('void BaseInput::UpdateTreadmillCalibrationGesture(')
end = source.index('\nvoid BaseInput::UpdateDapaCaptureGesture(', start)
fixture = (repo / 'tests/LocomotionControllerGestureTests.cpp.in').read_text(encoding='utf-8')
(out / 'controller.cpp').write_text(fixture.replace('/*PRODUCTION_GESTURE*/', source[start:end]), encoding='utf-8')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCULocomotionControllerGesture LANGUAGES CXX)
add_executable(LocomotionControllerGesture controller.cpp)
target_compile_features(LocomotionControllerGesture PRIVATE cxx_std_20)
target_include_directories(LocomotionControllerGesture PRIVATE "{repo.as_posix()}" "{repo.as_posix()}/libs/openxr-sdk/include")
''', encoding='utf-8')
subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build'), '-A', 'x64'], check=True)
subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
subprocess.run([str(out / 'build/Release/LocomotionControllerGesture.exe')], check=True)
