"""Exercise reordered snapshots through actual receiver Read/calibration methods."""
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
source = (repo / 'OpenOVR/Misc/Input/OscLocomotion.cpp').read_text(encoding='utf-8-sig')
start = source.index('locomotion::Output OscLocomotion::Read(')
end = source.index('\nvoid OscLocomotion::Receive()', start)
fixture = (repo / 'tests/LocomotionReferenceReadTests.cpp.in').read_text(encoding='utf-8')
(out / 'reference.cpp').write_text(fixture.replace('/*PRODUCTION_READ*/', source[start:end]), encoding='utf-8')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCULocomotionReferenceRead LANGUAGES CXX)
add_executable(LocomotionReferenceRead reference.cpp)
target_compile_features(LocomotionReferenceRead PRIVATE cxx_std_20)
target_include_directories(LocomotionReferenceRead PRIVATE "{repo.as_posix()}")
''', encoding='utf-8')
subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build'), '-A', 'x64'], check=True)
subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
subprocess.run([str(out / 'build/Release/LocomotionReferenceRead.exe')], check=True)
