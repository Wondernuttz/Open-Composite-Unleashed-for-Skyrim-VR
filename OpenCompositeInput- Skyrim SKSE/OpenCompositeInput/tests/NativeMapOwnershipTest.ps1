$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$repo = Split-Path (Split-Path $project -Parent) -Parent
$source = Get-Content -LiteralPath (Join-Path $project 'src/Main.cpp') -Raw
$source = $source.Replace("`r`n", "`n")
$runtime = Get-Content -LiteralPath (Join-Path $repo 'OpenCompositeSkyrimVR/OpenOVR/Reimpl/BaseOverlay.cpp') -Raw

function Section([string]$Text, [string]$Start, [string]$End) {
    $first = $Text.IndexOf($Start, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Missing section: $Start" }
    $last = $Text.IndexOf($End, $first + $Start.Length, [StringComparison]::Ordinal)
    if ($last -lt 0) { throw "Missing section end: $End" }
    return $Text.Substring($first, $last - $first)
}
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# Source-contract tests: these do not substitute for native in-game pointer QA.
$tracked = Section $source 'static constexpr std::string_view kTrackedMenus[]' '};'
Require (-not $tracked.Contains('"MapMenu"')) 'MapMenu must not be an OCU interaction target.'
$transform = Section $source "void UpdateMenuTransform()`n" 'class MenuWatcher'
$nativeExport = Section $transform 'if (g_mapMenuOpen)' 'if (!anyActive)'
Require ($nativeExport.Contains('return;')) 'Native map exclusion must exit before plane export.'
Require ($nativeExport.Contains('"MapMenu"')) 'Runtime exclusion sentinel must survive underlying-menu transitions.'
Require ($transform.Contains('!g_statsMenuOpen && !g_mapMenuOpen')) 'Underlying tracked menus must not activate on the map.'

$bypass = Section $source 'if (mapOpen) {' 'if (!raceMenuOpen)'
Require ($bypass.Contains('return;')) 'Map must exit before normal menu handling.'
Require ($bypass.Contains('s_cursorShown = false;')) 'Map must drop stale OCU cursor ownership.'
Require (-not ($bypass -match 'SetCursorVisibility|SetAppCulled|GetMenu\(|HandleEvent\(|NotifyMouseState\(')) 'Map bypass must not modify native UI.'

$console = Section $source 'void ConsoleWorldPickOnce()' '// Scheduler: posts the pump'
Require (-not ($console -match 'SetCursorVisibility|SetAppCulled')) 'Console must not leave the shared Skyrim pointer hidden.'
Require ($console.Contains('g_mapMenuOpen || !g_consoleOpen.load')) 'Console picking must also stay out of the map.'
Require ($console.Contains('publishNoHits();') -and $console.Contains('hitDistanceMeters[side]')) 'Keep OCU console hit publication and cleanup.'

$runtimeBypass = Section $runtime 'if (mapNativeOnly) {' 'if (!alwaysShow && !mapNativeOnly)'
Require ($runtimeBypass.Contains('g_menuLaserConsumesTrigger[side] = false;')) 'Map must retain native trigger input.'
Require ($runtimeBypass.Contains('g_menuLaserSuppressUntilRelease[side].store(false')) 'Map must release stale trigger suppression.'
Require ($runtime.Contains('if (menuActive && !mapNativeOnly)')) 'Runtime must never render an OCU menu laser over the map.'
Write-Output 'PASS: native MapMenu ownership source contracts (registration, export, input, visibility, runtime exclusion).'
