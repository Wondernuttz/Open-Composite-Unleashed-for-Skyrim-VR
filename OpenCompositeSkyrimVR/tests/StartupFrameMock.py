"""Compile production startup wait/store/submit functions against OpenXR mocks.

The fixture replaces session/graphics creation and runtime calls. The tested
frame ordering and stereo projection construction are extracted from XrBackend.
DAPA's synthetic path is excluded; this test makes no headset-focus claim.
"""
from pathlib import Path
import argparse
import subprocess


def block(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    for end in range(opening + 1, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        if not depth:
            return text[start:end + 1]
    raise ValueError(signature)


parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', required=True, type=Path)
parser.add_argument('--cmake', default=r'C:\Program Files\CMake\bin\cmake.exe')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (repo / 'DrvOpenXR/XrBackend.cpp').read_text(encoding='utf-8')
fixture = (repo / 'tests/StartupFrameMock.cpp.in').read_text(encoding='utf-8')
for marker, signature in [
    ('WAIT', 'void XrBackend::WaitForTrackingData'),
    ('STORE', 'void XrBackend::StoreEyeTexture'),
    ('SUBMIT', 'void XrBackend::SubmitFrames'),
]:
    fixture = fixture.replace(f'/*{marker}*/', block(source, signature))
(out / 'mock.cpp').write_text(fixture, encoding='utf-8')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(OCUStartupFrameMock LANGUAGES CXX)
add_executable(StartupFrameMock mock.cpp)
target_compile_features(StartupFrameMock PRIVATE cxx_std_17)
target_compile_definitions(StartupFrameMock PRIVATE NOMINMAX)
target_include_directories(StartupFrameMock PRIVATE "{repo.as_posix()}/build" "{repo.as_posix()}/libs/openxr-sdk/include")
''', encoding='utf-8')
subprocess.run([args.cmake, '-S', str(out), '-B', str(out / 'build'), '-A', 'x64'], check=True)
subprocess.run([args.cmake, '--build', str(out / 'build'), '--config', 'Release'], check=True)
subprocess.run([str(out / 'build/Release/StartupFrameMock.exe')], check=True)
