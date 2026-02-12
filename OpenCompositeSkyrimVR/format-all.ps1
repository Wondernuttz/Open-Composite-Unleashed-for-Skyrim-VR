Get-ChildItem -Recurse -File -Include *.cpp,*.h -Exclude *.gen.* .\DrvOpenXR\,.\OCOVR\,.\OpenOVR\ | %{clang-format.exe -i $_}
