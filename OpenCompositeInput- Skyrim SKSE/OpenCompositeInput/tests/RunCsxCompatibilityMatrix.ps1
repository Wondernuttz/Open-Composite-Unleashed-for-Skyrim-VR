param(
    [Parameter(Mandatory=$true)][string]$TestExecutable,
    [Parameter(Mandatory=$true)][string[]]$DllPaths
)
$ErrorActionPreference = 'Stop'
# Supply retained, independently obtained release DLLs. Never load their entry
# points or download/update mod files here. C++ enforces every current contract
# exactly once, so adding a contract also requires adding its test binary.
if (!(Test-Path -LiteralPath $TestExecutable -PathType Leaf)) { throw 'Build DapaCsxDrawTest first' }
foreach ($dll in $DllPaths) {
    if (!(Test-Path -LiteralPath $dll -PathType Leaf)) { throw "Missing fixture: $dll" }
}
$before = @{}
foreach ($dll in $DllPaths) { $before[$dll] = (Get-FileHash -LiteralPath $dll).Hash }
& $TestExecutable --matrix @DllPaths
if ($LASTEXITCODE -ne 0) { throw 'CSX compatibility matrix failed; do not publish this candidate' }
foreach ($dll in $DllPaths) {
    if ((Get-FileHash -LiteralPath $dll).Hash -ne $before[$dll]) { throw "Fixture modified: $dll" }
}
Write-Output 'PASS: all supported contracts exercised; original DLL files unchanged.'
