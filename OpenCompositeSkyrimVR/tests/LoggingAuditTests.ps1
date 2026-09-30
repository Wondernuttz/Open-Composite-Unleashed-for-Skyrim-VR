$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot
$inputSource = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'OpenOVR/Reimpl/BaseInput.cpp')
foreach ($guard in @('wipInjected && oovr_debug_logging_enabled()', 'turnInjected && oovr_debug_logging_enabled()', '!loggedEyePoses && oovr_debug_logging_enabled()', 'Eye gaze failure:')) {
    if (!$inputSource.Contains($guard)) { throw "Missing input diagnostic policy: $guard" }
}
$files = @('OpenOVR/Reimpl/BaseOverlay.cpp','OpenOVR/Misc/Keyboard/VRKeyboard.cpp','OpenOVR/Compositor/VRSManager.cpp','OpenOVR/Compositor/DensityMaskManager.cpp')
foreach ($file in $files) {
    $source = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot $file)
    if ($source -match 'OOVR_DEBUG_LOGF?\(\s*"[^"\r\n]*(failed|CORRUPTION DETECTED)') {
        throw "Failure was hidden behind checkbox in ${file}: $($Matches[0])"
    }
}
$keyboard = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'OpenOVR/Misc/Keyboard/VRKeyboard.cpp')
foreach ($operation in @('Refresh: xrAcquireSwapchainImage','Refresh: xrWaitSwapchainImage','RefreshConsole: xrAcquireSwapchainImage','RefreshConsole: xrWaitSwapchainImage')) {
    if (!$keyboard.Contains('OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] ' + $operation)) { throw "Unbounded or hidden keyboard failure: $operation" }
}
$plugin = Get-Content -Raw -LiteralPath (Join-Path (Split-Path $sourceRoot) 'OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/src/Main.cpp')
if ($plugin.Contains('Keyboard done, text:') -or $plugin.Contains('startingText: \"{}\"')) { throw 'Keyboard text still logged' }
if ($plugin -match 'SKSE::log::info\(\s*"LASER (cursor pos|drive-direct|notify-click|gfx-click)') { throw 'Routine laser traces remain at info level' }
Write-Output 'PASS: routine input/laser logging gated; failures retained and bounded; entered keyboard text omitted.'
