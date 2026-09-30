$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot
$files = @(
    'DrvOpenXR/DrvOpenXR.cpp', 'DrvOpenXR/ASWProvider.h', 'DrvOpenXR/XrBackend.cpp', 'DrvOpenXR/XrBackend.h',
    'OpenOVR/Compositor/dx11compositor.cpp', 'OpenOVR/Reimpl/BaseCompositor.cpp',
    'OpenOVR/Compositor/DlssUpscaler.h', 'OpenOVR/Compositor/DlssUpscaler.cpp',
    'OpenOVR/Compositor/Fsr3Upscaler.h', 'OpenOVR/Compositor/Fsr3Upscaler.cpp',
    'OpenOVR/Misc/Config.h', 'OpenOVR/Misc/Config.cpp',
    'OC Unleashed Configurator/MainForm.cs'
)
$retired = 'SpaceWarpProvider|g_spaceWarp|XR_FB_space_warp|aswStallCount|aswDisableWarned|ASWForceCustom|ASWFPControllerScale|ASWMVConfidence|ASWMVPixelScale|AswSplitPhase|ShouldUseAswSplitPipeline|ASWBufferEnabled|aswBufferEnabled|HasPreviousCachedFrame|SetWarpUpscaleCallback|WarpUpscaleParams|DispatchWarp|warpOutputDX|m_warpFsrContext|CachePreFPDepth|SetFPReplayPtr|SetControllerPos|TryFinishShaderCompilation|InstallFinishAccumHook|InstallOMSetDSSHook|ExtractStencilCPU|_chkAswExperimentalMode|_chkAswUpscalerReset|_chkAswUpscalerReactiveMask'
foreach ($relative in $files) {
    $text = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot $relative)
    if ($text -match $retired) { throw "Retired rendering symbol in ${relative}: $($Matches[0])" }
}
$provider = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'DrvOpenXR/ASWProvider.h')
foreach ($symbol in @('CacheFrame', 'WarpFrame', 'SubmitWarpedOutput', 'SetMotionGeometry', 'SampleLocomotion', 'SampleLocomotionYaw', 'SetWarpDisplayTime', 'SetInjectionWanted')) {
    if (!$provider.Contains($symbol)) { throw "Missing working DAPA API: $symbol" }
}
$backend = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'DrvOpenXR/XrBackend.cpp')
foreach ($symbol in @('g_aimPoses.matrix', 'oovr_laser_calibration::ApplyToPoseMatrix', 'DapaTiming::Recovery', 'g_aswProvider->WarpFrame(eye, aswCtx, views[eye].pose)')) {
    if (!$backend.Contains($symbol)) { throw "Missing retained behavior: $symbol" }
}
$compositor = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'OpenOVR/Compositor/dx11compositor.cpp')
foreach ($symbol in @('InstallSceneTargetHooks', 'RDMRenderScope::NotifyTargets', 'SyncVRSForRenderTargets', 'densityMaskManager.Arm', 'ExtractDepthToR32F', 'EnsureReactiveMaskResources')) {
    if (!$compositor.Contains($symbol)) { throw "Missing retained rendering path: $symbol" }
}
if ($compositor -match 'TryApplyPendingDensityMask|Hook_ClearDepthStencilView|ApplyDepthMask\(') { throw 'Unsafe original-depth RDM seeding remains in compositor' }
foreach ($relative in @('DrvOpenXR/SpaceWarpProvider.h', 'DrvOpenXR/SpaceWarpProvider.cpp')) {
    if (Test-Path -LiteralPath (Join-Path $sourceRoot $relative)) { throw "Retired provider source remains: $relative" }
}
$body = Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'OC Unleashed Configurator/BodyTrackingTab.cs')
if ($body -match 'OCU_LEGACY_RTMP|SendBodyOsc\(|PoseInferenceBuffers|EnumGpuAdapters|_bodyDeviceSel') {
    throw "Retired body inference path remains: $($Matches[0])"
}
foreach ($symbol in @('new MediaPipePoseWorker', 'new MediaPipe33TrackerConverter', 'SendContinuousBodyTrackerFrameOsc', 'DrawMediaPipeSkeleton', 'World3DCalibrationRecorder')) {
    if (!$body.Contains($symbol)) { throw "Missing World3D path: $symbol" }
}
Write-Output 'Retired rendering paths absent; retained DAPA, foveation and laser interfaces PASS'
