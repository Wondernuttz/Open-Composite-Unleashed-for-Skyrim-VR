#include "ASWProvider.h"

#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/logging.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

// Global instance — accessed from XrBackend for frame injection

// ── ASW frame buffering: staging textures for decoupled game/submit pipeline ──
bool g_aswStagingActive = false;
ID3D11Texture2D* g_aswStagingTex[2][kAswStagingSlotCount] = {};
ID3D11Query* g_aswStagingDoneQuery[2][kAswStagingSlotCount] = {};
std::atomic<uint64_t> g_aswStagingSlotSeq[2][kAswStagingSlotCount] = {};
std::atomic<int64_t> g_aswStagingPublishNs[2][kAswStagingSlotCount] = {};
std::atomic<uint32_t> g_aswStagingWriteCursor[2] = { 0, 0 };
std::atomic<int> g_aswStagingPublishedSlot[2] = { -1, -1 };
std::atomic<int> g_aswStagingLastReadySlot[2] = { -1, -1 };
std::atomic<uint64_t> g_aswStagingPublishSeq[2] = { 0, 0 };
XrSwapchain g_aswStagingSwapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
std::vector<XrSwapchainImageD3D11KHR> g_aswStagingSwapImages[2];

ASWProvider* g_aswProvider = nullptr;

// Loading/menu state — set by dx11compositor, read by ASW submit thread
std::atomic<bool> g_aswSkipWarp{ false };

// ============================================================================
// Embedded HLSL compute shader for frame warping
// ============================================================================
static const char* s_warpShaderHLSL = R"(

Texture2D<float4> prevColor    : register(t0);
Texture2D<float2> mvTex        : register(t1);  // camera MVs: prevUV - uv (UV space)
Texture2D<float>  depthTex     : register(t2);
RWTexture2D<float4> output     : register(u0);
RWTexture2D<uint> atomicDepth  : register(u1);  // forward scatter depth test buffer (R32_UINT)
SamplerState linearClamp       : register(s0);

cbuffer WarpParams : register(b0) {
    row_major float4x4 poseDeltaMatrix;   // transforms NEW view -> OLD view (backward warp)
    float2 resolution;
    float nearZ, farZ;
    float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
    float depthScale;           // multiplier on linearized depth (parallax intensity)
    float edgeFadeWidth;        // depth-edge fade threshold (depth ratio units)
    float nearFadeDepth;        // parallax fades to 0 below this depth; 0 = disabled
    float mvConfidence;         // 0=pure parallax, 1=full MV correction
    float mvPixelScale;         // overall MV magnitude multiplier
    float2 depthResolution;     // actual depth data dimensions (may differ from resolution when upscaler active)
    float _pad0;                // alignment padding
    float2 mvResolution;        // actual MV data dimensions (render-res when camera MVs + upscaler)
    float2 _pad1;               // alignment padding
    int debugMode;              // 0=normal, 1=depth viz, 2=MV magnitude viz
    float3 _pad2;               // alignment to 16 bytes
    row_major float4x4 headRotMatrix;  // head rotation delta between prev/cur cached poses
                                       // used to subtract head rot from camera MVs
    column_major float4x4 clipToClipNoLoco;  // prevVP * inv(curVP_original): head rot+trans, no loco
                                              // matches camera MV source exactly
    int hasClipToClipNoLoco;           // 1 = use clipToClipNoLoco, 0 = fallback to headRotMatrix
    float3 _pad3;
    row_major float4x4 forwardPoseDelta;  // transforms OLD view -> NEW view (forward scatter)
    float2 locoScreenDir;   // screen-space locomotion direction (from actorPos delta, not head tracking)
    float staticBlendFactor; // 1.0 = near-stationary (blend scatter→prevColor), 0.0 = moving
    float _pad4;
};

// LinearizeDepth: convert raw depth buffer value to linear distance in game units.
// Skyrim's depth convention: d=0 is near plane, d=1 is far plane (standard-Z).
// Formula: z_linear = zNear * zFar / (zFar - d * (zFar - zNear))
//   d=0 → zNear, d=1 → zFar
float LinearizeDepth(float d, float zNear, float zFar) {
    float denom = zFar - d * (zFar - zNear);
    return (abs(denom) > 0.0001) ? (zNear * zFar / denom) : zFar;
}

// Helper: map output pixel coordinate to MV pixel coordinate
// When camera MVs + upscaler, MVs are render-res but output is display-res.
int2 ToMVCoord(int2 colorPixel) {
    float2 uv = (float2(colorPixel) + 0.5) / resolution;
    int2 mp = int2(uv * mvResolution);
    return clamp(mp, int2(0,0), int2(mvResolution) - 1);
}

// Helper: map output pixel coordinate to depth pixel coordinate
// When an upscaler is active, color is display-res but depth is render-res.
// Depth data fills the top-left corner of the staging texture.
int2 ToDepthCoord(int2 colorPixel) {
    float2 uv = (float2(colorPixel) + 0.5) / resolution;
    int2 dp = int2(uv * depthResolution);
    return clamp(dp, int2(0,0), int2(depthResolution) - 1);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    float2 uv = ((float2)tid.xy + 0.5) / resolution;

    // 1. Read depth and search for foreground along combined parallax + locomotion direction.
    //    At disocclusion edges (object moved, revealing background), the cached depth
    //    is background. During locomotion, poseDeltaMatrix only captures head motion,
    //    so we also use the raw game MV to find the correct search direction.
    int2 pixel = (int2)tid.xy;
    int2 depthPixel = ToDepthCoord(pixel);
    float d = depthTex[depthPixel];
    float origD = d;  // save for disocclusion detection

    float tanX = lerp(fovTanLeft, fovTanRight, uv.x);
    float tanY = lerp(fovTanUp,   fovTanDown,  uv.y);

    // Compute parallax direction from head pose delta (may be tiny during pure locomotion).
    float linearDepth = LinearizeDepth(d, nearZ, farZ);
    float scaledDepth = linearDepth * depthScale;
    float3 newViewPos = float3(tanX * scaledDepth, tanY * scaledDepth, scaledDepth);
    float4 transformed = mul(poseDeltaMatrix, float4(newViewPos, 1.0));

    float2 parallaxDir = float2(0, 0);
    if (scaledDepth > 0.001 && transformed.z > 0.001) {
        float oldTanX = transformed.x / transformed.z;
        float oldTanY = transformed.y / transformed.z;
        parallaxDir = float2(
            (oldTanX - fovTanLeft) / (fovTanRight - fovTanLeft) - uv.x,
            (oldTanY - fovTanUp) / (fovTanDown - fovTanUp) - uv.y);
    }

    // Read raw MV at current pixel for locomotion-aware search direction.
    // During pure locomotion, parallaxDir is tiny (head-only) but game MVs capture
    // the full scene motion. MV points from current UV toward previous UV (cached frame).
    float2 roughMV = mvTex[ToMVCoord(pixel)] * mvPixelScale;
    roughMV = clamp(roughMV, float2(-0.15, -0.15), float2(0.15, 0.15));

    // Pick the search direction with larger screen-space magnitude.
    // parallaxDir dominates during head rotation; roughMV dominates during locomotion.
    float2 searchDir = parallaxDir;
    float parallaxMagPx = length(parallaxDir * resolution);
    float mvSearchMagPx = length(roughMV * resolution);
    if (mvSearchMagPx > parallaxMagPx)
        searchDir = roughMV;
    float searchMagPx = max(parallaxMagPx, mvSearchMagPx);

    // Convert to pixel-space stepping: normalize so dominant axis = +/-1 pixel/step.
    float2 searchDirPx = searchDir * resolution;
    float maxComp = max(abs(searchDirPx.x), abs(searchDirPx.y));
    float2 stepVec = (maxComp > 0.001) ? (searchDirPx / maxComp) : float2(0, 0);

    // Search for foreground depth edge along motion direction.
    // Track found pixel position — its MV is needed for correct locomotion correction.
    // Range covers typical locomotion disocclusion (up to ~36 pixels at sprint speed).
    // Early exit on "found deeper" prevents wasted reads through foreground interiors.
    int2 fgPixel = pixel;
    bool foundForeground = false;

    if (searchMagPx > 0.5) {
        int maxSearch = min(48, max(24, (int)searchMagPx + 16));
        for (int step = 1; step <= maxSearch; step++) {
            int2 searchPx = pixel + int2(stepVec * (float)step);
            searchPx = clamp(searchPx, int2(0,0), int2(resolution) - 1);
            float searchD = depthTex[ToDepthCoord(searchPx)];
            if (d - searchD > 0.01) {
                d = searchD;
                fgPixel = searchPx;
                foundForeground = true;
                break;
            }
            if (searchD - d > 0.01)
                break;  // entered deeper background — stop early
        }
    }

    // Cardinal search fallback: if directed search didn't find foreground and this pixel
    // is background, search 8 directions (cardinal + diagonal) at stride 2 up to 48 pixels.
    // Deep backgrounds have near-zero MVs so the directed search can't trigger.
    // Stride 2 ensures thin foreground edges aren't missed by coarse stepping.
    // 8 directions catch diagonal edges that cardinal-only would miss.
    // Gate on meaningful motion — without displacement there's no disocclusion to fix,
    // and the search would just create dark halos at every static depth edge.
    if (!foundForeground && origD > 0.50 && searchMagPx > 0.5) {
        int bestDist = 49;
        int2 dirs[8] = {
            int2(1,0), int2(-1,0), int2(0,1), int2(0,-1),
            int2(1,1), int2(1,-1), int2(-1,1), int2(-1,-1)
        };
        [unroll]
        for (int dir = 0; dir < 8; dir++) {
            for (int step = 2; step < bestDist; step += 2) {
                int2 sp = pixel + dirs[dir] * step;
                if (any(sp < 0) || sp.x >= (int)resolution.x || sp.y >= (int)resolution.y)
                    break;
                float sd = depthTex[ToDepthCoord(sp)];
                if (origD - sd > 0.01) {
                    bestDist = step;
                    d = sd;
                    fgPixel = sp;
                    foundForeground = true;
                    break;
                }
            }
        }
    }

    // After finding foreground, step a few pixels deeper into the object interior
    // for cleaner depth/MV values. Edge pixels can have MV contaminated by the
    // background (bilinear filtering, sub-pixel coverage), and the contamination
    // is worse when the background is deeper (its MV → 0, pulling the edge MV
    // away from the true foreground MV). Stepping inward avoids this.
    if (foundForeground) {
        float2 stepInDir = float2(fgPixel - pixel);
        float stepInLen = length(stepInDir);
        if (stepInLen > 0.5) {
            stepInDir /= stepInLen;
            for (int extra = 1; extra <= 4; extra++) {
                int2 deepPx = fgPixel + int2(round(stepInDir * (float)extra));
                deepPx = clamp(deepPx, int2(0,0), int2(resolution) - 1);
                float deepD = depthTex[ToDepthCoord(deepPx)];
                if (abs(deepD - d) < 0.02) {
                    fgPixel = deepPx;  // deeper into foreground, cleaner MV
                } else {
                    break;  // hit a different surface or edge — stop
                }
            }
        }
    }

    // Recompute parallax with (possibly foreground) depth.
    // When foreground was found, use fgPixel's screen angle (where the surface actually is)
    // instead of the output pixel's angle (which points at background). Using the output
    // pixel's angle with foreground depth creates a phantom 3D point that projects to the
    // wrong location in the cached frame, causing missing chunks at depth edges.
    float2 fgUV = foundForeground ? (float2(fgPixel) + 0.5) / resolution : uv;
    float fgTanX = foundForeground ? lerp(fovTanLeft, fovTanRight, fgUV.x) : tanX;
    float fgTanY = foundForeground ? lerp(fovTanUp, fovTanDown, fgUV.y) : tanY;

    linearDepth = LinearizeDepth(d, nearZ, farZ);
    scaledDepth = linearDepth * depthScale;
    newViewPos = float3(fgTanX * scaledDepth, fgTanY * scaledDepth, scaledDepth);
    transformed = mul(poseDeltaMatrix, float4(newViewPos, 1.0));
    float3 oldViewPos = transformed.xyz;

    float2 parallaxUV = uv;
    if (scaledDepth > 0.001 && oldViewPos.z > 0.001) {
        float oldTanX = oldViewPos.x / oldViewPos.z;
        float oldTanY = oldViewPos.y / oldViewPos.z;
        // Compute where fgPixel's surface projects in the cached frame, then express
        // as an offset relative to the OUTPUT pixel so parallax is a warp displacement.
        float2 cachedUV = float2(
            (oldTanX - fovTanLeft) / (fovTanRight - fovTanLeft),
            (oldTanY - fovTanUp) / (fovTanDown - fovTanUp));
        parallaxUV = uv + (cachedUV - fgUV);
    }

    // Near-field depth fade
    float depthFade = (nearFadeDepth > 0.0) ? saturate((linearDepth - nearFadeDepth) / nearFadeDepth) : 1.0;

    float2 parallaxOffset = parallaxUV - uv;

    // 3. Combine: parallax (faded at near-field only) to get source position in cached frame.
    float2 fadedParallax = parallaxOffset * depthFade;
    float2 parallaxSourceUV = uv + fadedParallax;

    // Read MV: at disocclusion edges, use the foreground pixel's MV (object MV, not background).
    int2 mvSourcePixel;
    if (foundForeground) {
        mvSourcePixel = ToMVCoord(fgPixel);
    } else {
        mvSourcePixel = ToMVCoord(clamp(int2(parallaxSourceUV * resolution), int2(0,0), int2(resolution) - 1));
    }
    float2 rawMV = mvTex[mvSourcePixel];
    float2 totalMV = rawMV * mvPixelScale;
    totalMV = clamp(totalMV, float2(-0.15, -0.15), float2(0.15, 0.15));

    // headOnlyMV from headRotMatrix (OpenXR frame-to-frame, NO stick rotation).
    // Use fgPixel's angle when foreground found — game MV at fgPixel is relative to fgPixel's
    // position, so headOnlyMV must match to get a clean residual (loco component).
    float2 headOnlyMV = float2(0, 0);
    {
        float3 viewPos = float3(fgTanX * scaledDepth, fgTanY * scaledDepth, scaledDepth);
        float4 rotated = mul(headRotMatrix, float4(viewPos, 1.0));
        if (scaledDepth > 0.001 && rotated.z > 0.001) {
            float rotTanX = rotated.x / rotated.z;
            float rotTanY = rotated.y / rotated.z;
            float2 rotUV = float2(
                (rotTanX - fovTanLeft) / (fovTanRight - fovTanLeft),
                (rotTanY - fovTanUp) / (fovTanDown - fovTanUp));
            headOnlyMV = rotUV - fgUV;
        }
    }

    // c2cHeadMV for diagnostics (captures head + stick rotation from RSS VP)
    float2 c2cHeadMV = float2(0, 0);
    if (hasClipToClipNoLoco) {
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 clipPos = float4(ndc, d, 1.0);
        float4 prevClip = mul(clipToClipNoLoco, clipPos);
        if (abs(prevClip.w) > 0.0001) {
            float2 prevNDC = prevClip.xy / prevClip.w;
            c2cHeadMV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5) - uv;
        }
    }

    // Residuals for diagnostics
    float2 c2cResidual = totalMV - c2cHeadMV;       // head+stick subtracted (loco only)
    float2 headOnlyResidual = totalMV - headOnlyMV;  // head-only subtracted (stick + loco)

    // Apply MV correction: subtract head-only MV so residual includes stick rotation + loco
    float2 mvOffset = float2(0, 0);
    if (abs(mvConfidence) > 0.001) {
        mvOffset = mvConfidence * headOnlyResidual;
    }

    // Final source UV: parallax (using foreground depth if found) + MV loco correction.
    // The foreground search sets d to the closer object's depth and reads its MV,
    // so parallax naturally uses the correct depth and MV captures foreground motion.
    // Head rotation parallax must always be applied so edges track head movement.
    float2 sourceUV = parallaxSourceUV + mvOffset;

    // OOB safety: if warped UV is outside frame, try without parallax, then identity
    if (any(sourceUV < -0.01) || any(sourceUV > 1.01)) {
        sourceUV = uv + mvOffset;  // drop parallax, keep loco
        if (any(sourceUV < -0.01) || any(sourceUV > 1.01))
            sourceUV = uv;  // identity fallback
    }
    sourceUV = saturate(sourceUV);

    // Sample cached frame
    float4 color = prevColor.SampleLevel(linearClamp, sourceUV, 0);
)";

// Third part of the shader — debug visualization + output write.
// Separate string literal to stay within MSVC's 16380-char limit.
static const char* s_warpShaderDebugHLSL = R"(
    // Debug visualization modes (set via aswDebugMode in INI, hot-reloadable)
    if (debugMode == 1) {
        // RAW depth viz: shows the raw depth buffer value (0-1 reversed-Z) as grayscale
        // d=1 (near) → white, d=0 (far/sky) → black
        color = float4(d, d, d, 1.0);
    } else if (debugMode == 2) {
        // Linearized depth viz: blue=near(<500gu), green=mid(500-5000gu), red=far(>5000gu)
        float normDepth = saturate(linearDepth / 50000.0);
        color = float4(normDepth, saturate(linearDepth / 5000.0) * (1.0 - normDepth),
                       saturate(1.0 - linearDepth / 500.0), 1.0);
    } else if (debugMode == 3) {
        // MV magnitude viz: green = small motion, red = large motion
        int2 mvPixel = ToMVCoord((int2)tid.xy);
        float2 mv = mvTex[mvPixel];
        float mvMag = length(mv) * 100.0;  // scale for visibility
        color = float4(saturate(mvMag), saturate(1.0 - mvMag), 0.0, 1.0);
    } else if (debugMode == 4) {
        // locoMV viz: shows totalMV - headMV (isolated locomotion).
        // Red = large locoMV, green = small. If green during loco, headMV is eating the signal.
        int2 mvPixel = ToMVCoord((int2)tid.xy);
        float2 rawMV = mvTex[mvPixel];
        float2 totalMV = rawMV * 1.0f * mvPixelScale;
        float2 headMV2 = float2(0, 0);
        if (hasClipToClipNoLoco) {
            float2 ndc2 = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            float4 cp2 = float4(ndc2, d, 1.0);
            float4 pc2 = mul(clipToClipNoLoco, cp2);
            if (abs(pc2.w) > 0.0001) {
                float2 pndc2 = pc2.xy / pc2.w;
                headMV2 = float2(pndc2.x * 0.5 + 0.5, 0.5 - pndc2.y * 0.5) - uv;
            }
        }
        float2 loco2 = totalMV - headMV2;
        float locoMag = length(loco2) * 100.0;
        color = float4(saturate(locoMag), saturate(1.0 - locoMag), 0.0, 1.0);
    } else if (debugMode == 6) {
        // mvOffset viz: shows actual applied MV correction (mvConfidence * locoMV).
        // Blue channel = magnitude. If zero during loco, mvOffset is being killed somewhere.
        float mvOffMag = length(mvOffset) * 100.0;
        // Also show if OOB would trigger: red = OOB (identity fallback)
        float2 testUV = uv + fadedParallax + mvOffset;
        float oob = (any(testUV < -0.01) || any(testUV > 1.01)) ? 1.0 : 0.0;
        color = float4(oob, 0.0, saturate(mvOffMag), 1.0);
    } else if (debugMode == 5) {
        // headMV viz: shows clipToClipNoLoco-derived head motion.
        // Red = large headMV, green = small.
        float headMag = length(c2cHeadMV) * 100.0;
        color = float4(saturate(headMag), saturate(1.0 - headMag), 0.0, 1.0);
    } else if (debugMode == 7) {
        // C2C RESIDUAL: totalMV - c2cHeadMV. Shows what locoMV looks like.
        // Red = +X residual, Cyan = -X residual, Green = +Y, Magenta = -Y.
        // Black center = perfect cancellation. Bright = large mismatch.
        // Compare left vs right eye: if right eye is brighter, c2c doesn't match right eye MVs.
        float scale = 200.0;  // amplify for visibility
        float rx = c2cResidual.x * scale;
        float ry = c2cResidual.y * scale;
        color = float4(
            saturate(rx) + saturate(-ry),   // R: +X or -Y
            saturate(ry) + saturate(-rx),   // G: +Y or -X
            saturate(-rx) + saturate(-ry),  // B: -X or -Y
            1.0);
    } else if (debugMode == 8) {
        // PDM RESIDUAL: totalMV - parallaxOffset. Shows what extraMV looks like.
        // Same color scheme as mode 7. Compare to mode 7 to see which headMV is better.
        float scale = 200.0;
        float rx = headOnlyResidual.x * scale;
        float ry = headOnlyResidual.y * scale;
        color = float4(
            saturate(rx) + saturate(-ry),
            saturate(ry) + saturate(-rx),
            saturate(-rx) + saturate(-ry),
            1.0);
    } else if (debugMode == 9) {
        // RAW MV direction: R = +X (rightward), G = +Y (downward), B = -X or -Y.
        // Shows raw game MV direction and magnitude per-pixel.
        float scale = 50.0;
        color = float4(
            saturate(totalMV.x * scale),
            saturate(totalMV.y * scale),
            saturate(-totalMV.x * scale) + saturate(-totalMV.y * scale),
            1.0);
    } else if (debugMode == 10) {
        // PARALLAX vs C2C comparison: shows difference between poseDeltaMatrix
        // and clipToClipNoLoco predictions. R=|diff.x|, G=|diff.y|. Should be
        // near-black if both compute the same head rotation.
        float2 diff = parallaxOffset - c2cHeadMV;
        float scale = 500.0;  // high amplification — differences should be tiny
        color = float4(saturate(abs(diff.x) * scale), saturate(abs(diff.y) * scale), 0.0, 1.0);
    } else if (debugMode == 11) {
        // DEPTH SEARCH VIZ: Green = foreground found by directed/cardinal search,
        // Red = original cached depth used (no foreground found).
        float brightness = d;
        if (foundForeground) {
            color = float4(0.0, brightness, 0.0, 1.0);  // green = search found foreground
        } else {
            color = float4(brightness, 0.0, 0.0, 1.0);  // red = original depth
        }
    } else if (debugMode == 12) {
        // RAW DEPTH HEAT MAP — fine-grained bands in 0.0-0.20 range
        // since all values appear to be < 0.50.
        //   Black  = d == 0 or negative
        //   Dark blue  = 0.00 - 0.02
        //   Blue       = 0.02 - 0.05
        //   Cyan       = 0.05 - 0.10
        //   Green      = 0.10 - 0.15
        //   Yellow     = 0.15 - 0.20
        //   Orange     = 0.20 - 0.50
        //   Red        = 0.50 - 1.00
        //   White      = d >= 1.0
        float od = origD;
        if (od >= 1.0)        color = float4(1,1,1,1);
        else if (od > 0.50)   color = float4(1, 0, 0, 1);
        else if (od > 0.20)   color = float4(1, 0.5, 0, 1);
        else if (od > 0.15)   color = float4(1, 1, 0, 1);
        else if (od > 0.10)   color = float4(0, 1, 0, 1);
        else if (od > 0.05)   color = float4(0, 1, 1, 1);
        else if (od > 0.02)   color = float4(0, 0, 1, 1);
        else if (od > 0.001)  color = float4(0, 0, 0.4, 1);
        else                  color = float4(0, 0, 0, 1);

        if (foundForeground) {
            color.rgb = color.rgb * 0.3 + float3(0.7, 0.0, 0.7);
        }
    }

    output[tid.xy] = color;
}
)";

// Second part of the shader — forward scatter kernels.
// Separate string literal to stay within MSVC's 16380-char limit.
static const char* s_forwardScatterHLSL = R"(

// ── Forward scatter pass 0: clear buffers ──
[numthreads(8, 8, 1)]
void CSClear(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;
    atomicDepth[tid.xy] = 0xFFFFFFFF;   // clear to max for InterlockedMin (smaller depth = closer = wins)
    output[tid.xy] = float4(0, 0, 0, 1);
}

// Helper: compute destination UV for a given source pixel coordinate.
// Used by CSForward to compute the Jacobian (destination spacing) for
// adaptive splat sizing.
float2 computeForwardDstUV(int2 srcPixel, bool skipMVCorrection) {
    float2 srcUV = (float2(srcPixel) + 0.5) / resolution;
    float d = depthTex[ToDepthCoord(srcPixel)];
    float linearDepth = LinearizeDepth(d, nearZ, farZ);
    float scaledDepth = linearDepth * depthScale;

    float tanX = lerp(fovTanLeft, fovTanRight, srcUV.x);
    float tanY = lerp(fovTanUp,   fovTanDown,  srcUV.y);
    float3 oldViewPos = float3(tanX * scaledDepth, tanY * scaledDepth, scaledDepth);
    float4 transformed = mul(forwardPoseDelta, float4(oldViewPos, 1.0));

    float2 dstUV = srcUV;
    if (scaledDepth > 0.001 && transformed.z > 0.001) {
        dstUV.x = (transformed.x / transformed.z - fovTanLeft) / (fovTanRight - fovTanLeft);
        dstUV.y = (transformed.y / transformed.z - fovTanUp)   / (fovTanDown  - fovTanUp);
    }

    float depthFade = (nearFadeDepth > 0.0) ? saturate((linearDepth - nearFadeDepth) / nearFadeDepth) : 1.0;
    dstUV = lerp(srcUV, dstUV, depthFade);

    if (!skipMVCorrection && abs(mvConfidence) > 0.001) {
        float2 rawMV = mvTex[ToMVCoord(srcPixel)];
        float2 totalMV = clamp(rawMV * mvPixelScale, float2(-0.15, -0.15), float2(0.15, 0.15));

        float3 viewPos = float3(tanX * scaledDepth, tanY * scaledDepth, scaledDepth);
        float4 rotated = mul(headRotMatrix, float4(viewPos, 1.0));
        float2 headOnlyMV = float2(0, 0);
        if (scaledDepth > 0.001 && rotated.z > 0.001) {
            headOnlyMV = float2(
                (rotated.x / rotated.z - fovTanLeft) / (fovTanRight - fovTanLeft),
                (rotated.y / rotated.z - fovTanUp) / (fovTanDown - fovTanUp)) - srcUV;
        }
        dstUV -= mvConfidence * (totalMV - headOnlyMV);
    }

    return dstUV;
}

// Compute splat half-size for a source pixel using depth-aware Jacobian.
// At depth discontinuities (fg next to sky, or different-depth surfaces),
// the Jacobian falls back to stretch=1 to prevent cross-layer parallax
// from inflating splat sizes. This keeps branches/foliage at correct thickness.
int2 computeSplatHalf(int2 srcPixel, bool skipMV) {
    float2 srcUV = ((float2)srcPixel + 0.5) / resolution;
    float d = depthTex[ToDepthCoord(srcPixel)];

    // Sky pixels don't need large splats — they lose depth test to fg anyway
    if (d >= 0.999) return int2(0, 0);

    float2 dstPx = computeForwardDstUV(srcPixel, skipMV) * resolution;

    // Depth-aware Jacobian: only use neighbor destination if at similar depth.
    // When neighbor is sky or at very different depth (different surface),
    // fall back to stretch=1 to prevent cross-layer parallax inflation.
    int2 rightPx = min(srcPixel + int2(1, 0), int2(resolution) - 1);
    int2 downPx  = min(srcPixel + int2(0, 1), int2(resolution) - 1);
    float dR = depthTex[ToDepthCoord(rightPx)];
    float dD = depthTex[ToDepthCoord(downPx)];

    float depthThresh = d * 0.03;  // 3% relative depth threshold
    bool rSameDepth = (dR < 0.999) && (abs(dR - d) < depthThresh);
    bool dSameDepth = (dD < 0.999) && (abs(dD - d) < depthThresh);

    float2 dDx = rSameDepth ? (computeForwardDstUV(rightPx, skipMV) * resolution - dstPx) : float2(1, 0);
    float2 dDy = dSameDepth ? (computeForwardDstUV(downPx, skipMV) * resolution - dstPx) : float2(0, 1);

    float stretchX = abs(dDx.x) + abs(dDy.x);
    float stretchY = abs(dDx.y) + abs(dDy.y);

    // half=0 when stretch=1 (single pixel, no gap). Max=2 (5x5) to prevent
    // foliage over-fill. Old min=1 made every pixel 3x3, thickening branches.
    int halfW = clamp((int)ceil((stretchX - 1.0) * 0.5), 0, 2);
    int halfH = clamp((int)ceil((stretchY - 1.0) * 0.5), 0, 2);
    return int2(halfW, halfH);
}

// ── Forward scatter pass 1 (depth only): scatter depth with InterlockedMin ──
// After this pass, atomicDepth contains the final closest depth at each pixel.
// No color writes — eliminates race condition where a farther pixel's color
// overwrites a closer pixel's color due to non-atomic depth+color updates.
[numthreads(8, 8, 1)]
void CSForwardDepth(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    int2 depthPixel = ToDepthCoord((int2)tid.xy);
    float d = depthTex[depthPixel];

    // Emissive pixel depth correction: bright pixels (flames, magic effects)
    // rendered with additive blending don't write depth. Where the effect extends
    // beyond the solid geometry, the depth is the background's. This causes the
    // bright pixels to warp with the background instead of the emitting object.
    // Fix: if pixel is bright and a neighbor has significantly closer depth,
    // snap to that foreground depth so the effect warps with the emitter.
    if (d < 0.999) {
        float2 srcUV = ((float2)tid.xy + 0.5) / resolution;
        float3 srcColor = prevColor.SampleLevel(linearClamp, srcUV, 0).rgb;
        float luminance = dot(srcColor, float3(0.299, 0.587, 0.114));
        if (luminance > 0.5) {
            float closestD = d;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int2 np = (int2)tid.xy + int2(dx, dy);
                    if (any(np < 0) || np.x >= (int)resolution.x || np.y >= (int)resolution.y)
                        continue;
                    float nd = depthTex[ToDepthCoord(np)];
                    if (nd < closestD && nd > 0.001) closestD = nd;
                }
            }
            // Only snap if meaningful depth gap (>2% = different layer)
            if (d - closestD > d * 0.02) {
                d = closestD;
            }
        }
    }

    uint quantizedDepth = (d >= 0.999) ? 0xFFFFFFFE : asuint(d);

    bool skipMV = (debugMode == 53);
    float2 dstUV = computeForwardDstUV((int2)tid.xy, skipMV);
    float2 dstPx = dstUV * resolution;

    // When near-stationary (staticBlendFactor > 0.5), freeze ALL pixels to source
    // position. Even tiny head rotations create multi-pixel displacements across the
    // frame, but the correction is imperceptible — it only thickens foliage via
    // integer rounding. Global freeze gives 1:1 faithful output when stationary.
    if (staticBlendFactor > 0.5) {
        dstPx = (float2)tid.xy + 0.5;
        dstUV = dstPx / resolution;
    }

    if (any(dstUV < -0.001) || any(dstUV >= 1.001)) return;

    // When frozen, force 1×1 splat — no expansion into neighbors (prevents thickening).
    int2 half = (staticBlendFactor > 0.5) ? int2(0, 0) : computeSplatHalf((int2)tid.xy, skipMV);
    int2 centerPx = int2(floor(dstPx));

    for (int sy = -half.y; sy <= half.y; sy++) {
        for (int sx = -half.x; sx <= half.x; sx++) {
            int2 p = centerPx + int2(sx, sy);
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                continue;
            InterlockedMin(atomicDepth[p], quantizedDepth);
        }
    }
}

// ── Forward scatter pass 2 (color only): write color using finalized depth ──
// For each source pixel, write color ONLY to destinations where this pixel's
// depth matches the finalized depth buffer (depth test already resolved).
[numthreads(8, 8, 1)]
void CSForwardColor(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    float2 srcUV = ((float2)tid.xy + 0.5) / resolution;
    int2 depthPixel = ToDepthCoord((int2)tid.xy);
    float d = depthTex[depthPixel];

    // Same emissive depth correction as CSForwardDepth — must match exactly
    // so quantizedDepth here equals what CSForwardDepth wrote to atomicDepth.
    if (d < 0.999) {
        float3 srcColor = prevColor.SampleLevel(linearClamp, srcUV, 0).rgb;
        float luminance = dot(srcColor, float3(0.299, 0.587, 0.114));
        if (luminance > 0.5) {
            float closestD = d;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int2 np = (int2)tid.xy + int2(dx, dy);
                    if (any(np < 0) || np.x >= (int)resolution.x || np.y >= (int)resolution.y)
                        continue;
                    float nd = depthTex[ToDepthCoord(np)];
                    if (nd < closestD && nd > 0.001) closestD = nd;
                }
            }
            if (d - closestD > d * 0.02) {
                d = closestD;
            }
        }
    }

    uint quantizedDepth = (d >= 0.999) ? 0xFFFFFFFE : asuint(d);

    // debugMode 50: output raw depth as grayscale (no scatter, writes in-place)
    if (debugMode == 50) {
        float vis = saturate((d - 0.84) / 0.16);
        output[tid.xy] = float4(vis, vis, vis, 1);
        return;
    }

    // debugMode 52: identity forward scatter (no parallax, no MV, just copy).
    if (debugMode == 52) {
        float4 color = prevColor.SampleLevel(linearClamp, srcUV, 0);
        int2 p = (int2)tid.xy;
        if (quantizedDepth == atomicDepth[p])
            output[p] = color;
        return;
    }

    bool skipMV = (debugMode == 53);
    float2 dstUV = computeForwardDstUV((int2)tid.xy, skipMV);
    float2 dstPx = dstUV * resolution;

    // When near-stationary (staticBlendFactor > 0.5), freeze ALL pixels to source
    // position. Even tiny head rotations create multi-pixel displacements across the
    // frame, but the correction is imperceptible — it only thickens foliage via
    // integer rounding. Global freeze gives 1:1 faithful output when stationary.
    if (staticBlendFactor > 0.5) {
        dstPx = (float2)tid.xy + 0.5;
        dstUV = dstPx / resolution;
    }

    if (any(dstUV < -0.001) || any(dstUV >= 1.001)) return;

    // debugMode 51: show parallax offset magnitude as color
    float4 color;
    if (debugMode == 51) {
        float2 offset = dstUV - srcUV;
        float t = saturate(length(offset * resolution) / 30.0);
        color = float4(t, 1.0 - abs(t - 0.5) * 2.0, 1.0 - t, 1);
    } else {
        color = prevColor.SampleLevel(linearClamp, srcUV, 0);
    }

    // When frozen, force 1×1 splat — no expansion into neighbors (prevents thickening).
    int2 half = (staticBlendFactor > 0.5) ? int2(0, 0) : computeSplatHalf((int2)tid.xy, skipMV);
    int2 centerPx = int2(floor(dstPx));

    for (int sy = -half.y; sy <= half.y; sy++) {
        for (int sx = -half.x; sx <= half.x; sx++) {
            int2 p = centerPx + int2(sx, sy);
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                continue;
            if (quantizedDepth == atomicDepth[p]) {
                output[p] = color;
            }
        }
    }
}

// ── Legacy single-pass CSForward (kept for fallback/debug) ──
[numthreads(8, 8, 1)]
void CSForward(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    float2 srcUV = ((float2)tid.xy + 0.5) / resolution;
    int2 depthPixel = ToDepthCoord((int2)tid.xy);
    float d = depthTex[depthPixel];

    if (debugMode == 50) {
        float vis = saturate((d - 0.84) / 0.16);
        output[tid.xy] = float4(vis, vis, vis, 1);
        return;
    }
    if (debugMode == 52) {
        float4 color = prevColor.SampleLevel(linearClamp, srcUV, 0);
        uint quantizedDepth = asuint(d);
        int2 p = (int2)tid.xy;
        uint prevD;
        InterlockedMin(atomicDepth[p], quantizedDepth, prevD);
        if (quantizedDepth <= prevD) output[p] = color;
        return;
    }

    bool skipMV = (debugMode == 53);
    float2 dstUV = computeForwardDstUV((int2)tid.xy, skipMV);
    float2 dstPx = dstUV * resolution;
    if (any(dstUV < -0.001) || any(dstUV >= 1.001)) return;

    float4 color = prevColor.SampleLevel(linearClamp, srcUV, 0);
    uint quantizedDepth = (d >= 0.999) ? 0xFFFFFFFE : asuint(d);
    int2 half = computeSplatHalf((int2)tid.xy, skipMV);
    int2 centerPx = int2(floor(dstPx));

    for (int sy = -half.y; sy <= half.y; sy++) {
        for (int sx = -half.x; sx <= half.x; sx++) {
            int2 p = centerPx + int2(sx, sy);
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                continue;
            uint prevD;
            InterlockedMin(atomicDepth[p], quantizedDepth, prevD);
            if (quantizedDepth <= prevD) output[p] = color;
        }
    }
}

// ── Forward scatter pass 2: depth-edge cleanup + disocclusion fill ──
//
// Three cases handled:
// 1. FILLED pixels at depth edges: background pixels (walls, ground) that
//    scattered to positions adjacent to foreground (NPCs, objects). Detected
//    by checking if this pixel is significantly farther than its 3x3 min
//    neighbor. Replaced with the nearer neighbor's color.
// 2. SKY-MARKED pixels (0xFFFFFFFE): sky interleaved with thin geometry
//    (branches, poles). Only replaced if adjacent to real foreground.
)" R"(
// 3. UNFILLED pixels (0xFFFFFFFF): disocclusion gaps from scatter. Filled
//    by searching up to 128px in 8 directions. PREFERS FARTHER depth so
//    background fills gaps (near-prefer was tested and proven worse).
[numthreads(8, 8, 1)]
void CSDilate(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    uint myDepth = atomicDepth[tid.xy];

    // Case 1: DISABLED — the two-pass pipeline handles depth-edge correctness
    // by finalizing depth before writing color. Case 1 was creating visible
    // streak artifacts by replacing background pixels at depth edges.
    if (myDepth < 0xFFFFFFFE) {
        return;
    }

    // Case 3 only: unfilled (black) pixels. Skip sky-marked pixels entirely —
    // filling sky gaps with fg color makes distant trees/foliage look solid.
    if (myDepth != 0xFFFFFFFF) return;  // only process unfilled pixels

    // Locomotion-aligned mirror-fill with source-passthrough fallback.
    // The mirror-fill along the loco direction produces smooth, motion-aligned
    // content for trailing-edge disocclusion. When no background is found in the
    // loco direction, fall back to source frame passthrough (previous frame's
    // content at this screen position) instead of nearest-pixel search, which
    // pulls in wrong content (ground, other fg) and creates streak artifacts.

    int maxSearch = 128;

    int2 dirs[8] = {
        int2(-1,0), int2(1,0), int2(0,-1), int2(0,1),
        int2(-1,-1), int2(1,-1), int2(-1,1), int2(1,1)
    };

    // locoScreenDir is pre-computed from actorPos delta (thumbstick locomotion),
    // NOT from forwardPoseDelta (which is head-only tracking translation).
    // This ensures CSDilate searches along the actual locomotion direction.

    // Find which of the 8 directions best aligns with locomotion
    float bestAlign = -2.0;
    int primaryDir = 0;
    if (length(locoScreenDir) > 0.0001) {
        float2 locoNorm = normalize(locoScreenDir);
        [unroll]
        for (int i = 0; i < 8; i++) {
            float2 d = normalize(float2(dirs[i]));
            float alignment = dot(d, locoNorm);
            if (alignment > bestAlign) {
                bestAlign = alignment;
                primaryDir = i;
            }
        }
    }

    bool useLocoDir = (length(locoScreenDir) > 0.0001 && bestAlign > 0.3);

    // --- Direction-agnostic mirror fill ---
    // Search BOTH loco and opposite directions with contig>=3 filter.
    // Assign fg/bg by depth (closer=fg, farther=bg). Mirror from bg side.
    // Farthest-depth-edge fallback for intra-object gaps (similar depth).

    int2 oppDirVec = -dirs[primaryDir];

    // Search direction A (loco direction) — skip isolated scattered pixels
    int distA = 0;
    uint depthA = 0xFFFFFFFF;
    bool foundA = false;

    if (useLocoDir) {
        for (int step = 1; step <= maxSearch; step++) {
            int2 p = (int2)tid.xy + dirs[primaryDir] * step;
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                break;
            uint nd = atomicDepth[p];
            if (nd == 0xFFFFFFFF) continue;

            int contig = 1;
            for (int c = 1; c <= 3; c++) {
                int2 cp = p + dirs[primaryDir] * c;
                if (any(cp < 0) || cp.x >= (int)resolution.x || cp.y >= (int)resolution.y)
                    break;
                if (atomicDepth[cp] == 0xFFFFFFFF) break;
                contig++;
            }

            if (contig >= 3) {
                distA = step;
                depthA = nd;
                foundA = true;
                break;
            }
        }
    }

    // Search direction B (opposite to loco) — skip isolated scattered pixels
    int distB = 0;
    uint depthB = 0xFFFFFFFF;
    bool foundB = false;

    if (useLocoDir) {
        for (int step = 1; step <= maxSearch; step++) {
            int2 p = (int2)tid.xy + oppDirVec * step;
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                break;
            uint nd = atomicDepth[p];
            if (nd == 0xFFFFFFFF) continue;

            int contig = 1;
            for (int c = 1; c <= 3; c++) {
                int2 cp = p + oppDirVec * c;
                if (any(cp < 0) || cp.x >= (int)resolution.x || cp.y >= (int)resolution.y)
                    break;
                if (atomicDepth[cp] == 0xFFFFFFFF) break;
                contig++;
            }

            if (contig >= 3) {
                distB = step;
                depthB = nd;
                foundB = true;
                break;
            }
        }
    }

    // Assign fg/bg by depth. Mirror from bg side.
    bool useMirror = false;
    int bgDist = 0;
    int2 bgDirVec = oppDirVec;

    if (foundA && foundB) {
        if (depthA >= 0xFFFFFFFE || depthB >= 0xFFFFFFFE) {
            useMirror = true;
            if (depthA >= 0xFFFFFFFE) {
                bgDist = distA; bgDirVec = dirs[primaryDir];
            } else {
                bgDist = distB; bgDirVec = oppDirVec;
            }
        } else {
            float dA = asfloat(depthA);
            float dB = asfloat(depthB);

            float fgD, bgD;
            if (dA > dB) {
                bgD = dA; fgD = dB;
                bgDist = distA; bgDirVec = dirs[primaryDir];
            } else {
                bgD = dB; fgD = dA;
                bgDist = distB; bgDirVec = oppDirVec;
            }

            bool realDisocclusion = (bgD > fgD * 1.01);

            if (realDisocclusion) {
                // Real disocclusion confirmed — always mirror from bg side.
                useMirror = true;
            } else {
                // Both edges at similar depth — second-pass search skipping
                // content at the found depth to find real background.
                float skipD = min(dA, dB);

                [unroll]
                for (int dir = 0; dir < 2; dir++) {
                    if (useMirror) break;
                    int2 searchDir = (dir == 0) ? dirs[primaryDir] : oppDirVec;
                    int startStep = (dir == 0) ? distA + 1 : distB + 1;

                    for (int step = startStep; step <= maxSearch; step++) {
                        int2 p = (int2)tid.xy + searchDir * step;
                        if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                            break;
                        uint nd = atomicDepth[p];
                        if (nd == 0xFFFFFFFF) continue;
                        if (nd >= 0xFFFFFFFE) {
                            bgDist = step; bgDirVec = searchDir;
                            useMirror = true;
                            break;
                        }
                        float d = asfloat(nd);
                        if (d <= skipD * 1.01) continue;
                        int contig = 1;
                        for (int c = 1; c <= 3; c++) {
                            int2 cp = p + searchDir * c;
                            if (any(cp < 0) || cp.x >= (int)resolution.x || cp.y >= (int)resolution.y)
                                break;
                            if (atomicDepth[cp] == 0xFFFFFFFF) break;
                            contig++;
                        }
                        if (contig >= 3) {
                            bgDist = step; bgDirVec = searchDir;
                            useMirror = true;
                            break;
                        }
                    }
                }
            }
        }
    } else if (foundA && !foundB) {
        bgDist = distA; bgDirVec = dirs[primaryDir];
        useMirror = true;
    } else if (foundB && !foundA) {
        bgDist = distB; bgDirVec = oppDirVec;
        useMirror = true;
    }

    if (useMirror) {
        // Locomotion mirror fill: sample from background surface past the bg edge.
        // Cap the mirror offset to prevent body-shape echo — when bgDist varies
        // along a foreground object's outline, uncapped 2x mirror traces the outline
        // into the background, creating visible outline-shaped fill. Capping to a
        // small fixed offset (8px) makes fill content uniform across the gap.
        int2 bgEdge = (int2)tid.xy + bgDirVec * bgDist;
        int mirrorOffset = min(bgDist, 8);
        int2 mirrorPos = bgEdge + bgDirVec * mirrorOffset;
        mirrorPos = clamp(mirrorPos, int2(0, 0),
                          int2((int)resolution.x - 1, (int)resolution.y - 1));
        uint mirrorDepth = atomicDepth[mirrorPos];
        if (mirrorDepth != 0xFFFFFFFF) {
            output[tid.xy] = output[mirrorPos];
        } else {
            output[tid.xy] = output[bgEdge];
        }
    } else if (useLocoDir) {
        // Locomotion active but mirror rejected (intra-object gap).
        // Use farthest-depth edge found (most likely background).
        bool fallbackFilled = false;
        if (foundA || foundB) {
            int2 bestEdge;
            if (foundA && foundB) {
                float dAf = (depthA >= 0xFFFFFFFE) ? 1.0 : asfloat(depthA);
                float dBf = (depthB >= 0xFFFFFFFE) ? 1.0 : asfloat(depthB);
                if (dAf >= dBf) {
                    bestEdge = (int2)tid.xy + dirs[primaryDir] * distA;
                } else {
                    bestEdge = (int2)tid.xy + oppDirVec * distB;
                }
            } else if (foundA) {
                bestEdge = (int2)tid.xy + dirs[primaryDir] * distA;
            } else {
                bestEdge = (int2)tid.xy + oppDirVec * distB;
            }
            bestEdge = clamp(bestEdge, int2(0, 0),
                             int2((int)resolution.x - 1, (int)resolution.y - 1));
            output[tid.xy] = output[bestEdge];
            fallbackFilled = true;
        }
        if (!fallbackFilled) {
            float2 srcUV = (float2(tid.xy) + 0.5) / resolution;
            output[tid.xy] = prevColor.SampleLevel(linearClamp, srcUV, 0);
        }
    } else {
        // No locomotion: minimal fill for immediate scatter seams only.
        // Head tracking creates sub-pixel to 1px displacement, so scatter gaps
        // are tiny. Search only 2 steps (immediate neighbors) to seal 1px seams.
        // Anything farther is natural foliage transparency — prevColor passthrough
        // preserves thin geometry (flowers, grass) without thickening.
        uint bestDepth = 0;
        int2 bestPos = (int2)tid.xy;
        bool found = false;
        [unroll]
        for (int d = 0; d < 8; d++) {
            for (int step = 1; step <= 2; step++) {
                int2 p = (int2)tid.xy + dirs[d] * step;
                if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                    break;
                uint nd = atomicDepth[p];
                if (nd != 0xFFFFFFFF) {
                    if (!found || nd > bestDepth) {
                        bestDepth = nd;
                        bestPos = p;
                        found = true;
                    }
                    break;
                }
            }
        }
        if (found) {
            output[tid.xy] = output[bestPos];
        } else {
            float2 srcUV = (float2(tid.xy) + 0.5) / resolution;
            output[tid.xy] = prevColor.SampleLevel(linearClamp, srcUV, 0);
        }
    }
}
)";

// ============================================================================
// Quaternion math helpers
// ============================================================================

void ASWProvider::QuatInverse(const XrQuaternionf& q, XrQuaternionf& out)
{
	// For unit quaternions, inverse = conjugate
	out.x = -q.x;
	out.y = -q.y;
	out.z = -q.z;
	out.w = q.w;
}

void ASWProvider::QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b, XrQuaternionf& out)
{
	out.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	out.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	out.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	out.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
}

void ASWProvider::QuatRotateVec(const XrQuaternionf& q, const XrVector3f& v, XrVector3f& out)
{
	// Rotate vector by quaternion: q * v * q^-1
	// Optimized formula: out = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
	float cx = q.y * v.z - q.z * v.y + q.w * v.x;
	float cy = q.z * v.x - q.x * v.z + q.w * v.y;
	float cz = q.x * v.y - q.y * v.x + q.w * v.z;
	out.x = v.x + 2.0f * (q.y * cz - q.z * cy);
	out.y = v.y + 2.0f * (q.z * cx - q.x * cz);
	out.z = v.z + 2.0f * (q.x * cy - q.y * cx);
}

void ASWProvider::BuildPoseDeltaMatrix(const XrPosef& oldPose, const XrPosef& newPose,
    float* m)
{
	// Build the 4x4 matrix that transforms view-space positions from oldPose to newPose.
	// This is: newView * inverse(oldView)
	// Which simplifies to: deltaRot applied to position, then deltaTranslation
	//
	// deltaRotation = inverse(newPose.orientation) * oldPose.orientation
	// (because view matrix inverts the pose: viewRot = inverse(poseRot))
	//
	// Actually: oldView = inverse(oldPose), newView = inverse(newPose)
	// poseDelta = newView * inverse(oldView) = inverse(newPose) * oldPose
	//
	// For the rotation part:
	//   deltaRot = conjugate(newOri) * oldOri
	// For the translation part:
	//   deltaPos in new view space = conjugate(newOri) * (oldPos - newPos)

	XrQuaternionf newOriInv;
	QuatInverse(newPose.orientation, newOriInv);

	// Rotation: conjugate(new) * old
	XrQuaternionf deltaRot;
	QuatMultiply(newOriInv, oldPose.orientation, deltaRot);

	// Translation: conjugate(new) * (oldPos - newPos)
	XrVector3f posDiff = {
		oldPose.position.x - newPose.position.x,
		oldPose.position.y - newPose.position.y,
		oldPose.position.z - newPose.position.z
	};
	XrVector3f deltaTrans;
	QuatRotateVec(newOriInv, posDiff, deltaTrans);

	// Convert quaternion to 3x3 rotation matrix (row-major)
	float qx = deltaRot.x, qy = deltaRot.y, qz = deltaRot.z, qw = deltaRot.w;
	float xx = qx * qx, yy = qy * qy, zz = qz * qz;
	float xy = qx * qy, xz = qx * qz, yz = qy * qz;
	float wx = qw * qx, wy = qw * qy, wz = qw * qz;

	// Row-major 4x4 matrix
	m[0] = 1.0f - 2.0f * (yy + zz);
	m[1] = 2.0f * (xy - wz);
	m[2] = 2.0f * (xz + wy);
	m[3] = deltaTrans.x;
	m[4] = 2.0f * (xy + wz);
	m[5] = 1.0f - 2.0f * (xx + zz);
	m[6] = 2.0f * (yz - wx);
	m[7] = deltaTrans.y;
	m[8] = 2.0f * (xz - wy);
	m[9] = 2.0f * (yz + wx);
	m[10] = 1.0f - 2.0f * (xx + yy);
	m[11] = deltaTrans.z;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

// ============================================================================
// Lifecycle
// ============================================================================

ASWProvider::~ASWProvider()
{
	Shutdown();
}

bool ASWProvider::Initialize(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight)
{
	if (m_ready)
		return true;
	if (!device || eyeWidth == 0 || eyeHeight == 0)
		return false;

	OOVR_LOGF("ASW: Initializing — per-eye %ux%u", eyeWidth, eyeHeight);

	m_eyeWidth = eyeWidth;
	m_eyeHeight = eyeHeight;
	m_device = device;
	m_device->AddRef(); // prevent device destruction while ASW holds a reference

	if (!CreateComputeShader(device)) {
		OOVR_LOG("ASW: Failed to create compute shader");
		Shutdown();
		return false;
	}

	// Set up D3D12 warp pipeline (separate GPU queue to avoid D3D11 contention)
	{
		HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_d3d11Device5));
		if (FAILED(hr)) {
			OOVR_LOG("ASW D3D12: ID3D11Device5 not available — D3D12 warp disabled");
		} else {
			IDXGIDevice* dxgiDevice = nullptr;
			hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
			if (SUCCEEDED(hr)) {
				IDXGIAdapter* adapter = nullptr;
				hr = dxgiDevice->GetAdapter(&adapter);
				dxgiDevice->Release();
				if (SUCCEEDED(hr)) {
					if (CreateDX12Device(adapter)) {
						if (CreateSharedFence()) {
							if (CreateDX12ComputePipeline()) {
								m_d3d12Ready = true;
								OOVR_LOG("ASW D3D12: Warp pipeline fully initialized");
							}
						}
					}
					adapter->Release();
				}
			}
			if (!m_d3d12Ready) {
				OOVR_LOG("ASW D3D12: Setup failed — falling back to D3D11 warp");
			}
		}
	}

	if (!CreateStagingTextures(device)) {
		OOVR_LOG("ASW: Failed to create staging textures");
		Shutdown();
		return false;
	}

	if (!CreateOutputSwapchain(eyeWidth * 2, eyeHeight)) {
		OOVR_LOG("ASW: Failed to create output swapchain");
		Shutdown();
		return false;
	}

	if (!CreateDepthSwapchain(eyeWidth * 2, eyeHeight)) {
		OOVR_LOG("ASW: Failed to create depth swapchain (non-fatal — depth layer disabled)");
		// Non-fatal: ASW works without depth, just no depth attachment for the runtime
	}

	m_ready = true;
	m_publishedSlot.store(-1, std::memory_order_relaxed);
	m_previousPublishedSlot.store(-1, std::memory_order_relaxed);
	m_warpReadSlot.store(-1, std::memory_order_relaxed);
	m_buildSlot = 0;
	m_buildEyeReady[0] = false;
	m_buildEyeReady[1] = false;
	m_hasLastPublishedPose = false;
	m_frameCounter = 0;
	OOVR_LOGF("ASW: Initialized — %ux%u per eye, compute shader ready", eyeWidth, eyeHeight);
	return true;
}

bool ASWProvider::CreateComputeShader(ID3D11Device* device)
{
	// Compile HLSL
	DWORD flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled = nullptr;
	ID3DBlob* errors = nullptr;
	std::string csMainSrc = std::string(s_warpShaderHLSL) + s_warpShaderDebugHLSL;
	HRESULT hr = D3DCompile(csMainSrc.c_str(), csMainSrc.size(),
	    "ASWWarp", nullptr, nullptr, "CSMain", "cs_5_0", flags, 0, &compiled, &errors);
	if (FAILED(hr)) {
		if (errors) {
			OOVR_LOGF("ASW: Shader compile error: %s", (char*)errors->GetBufferPointer());
			errors->Release();
		}
		return false;
	}
	if (errors)
		errors->Release();

	hr = device->CreateComputeShader(compiled->GetBufferPointer(),
	    compiled->GetBufferSize(), nullptr, &m_warpCS);
	compiled->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateComputeShader(CSMain) failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	// Compile forward scatter shaders (CSClear, CSForward, CSDilate)
	// Need full shader source: base declarations + forward scatter kernels
	std::string fullShaderSrc = std::string(s_warpShaderHLSL) + s_warpShaderDebugHLSL + s_forwardScatterHLSL;
	auto compileEntry = [&](const char* entry, ID3D11ComputeShader** outCS) -> bool {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errs = nullptr;
		HRESULT r = D3DCompile(fullShaderSrc.c_str(), fullShaderSrc.size(),
		    entry, nullptr, nullptr, entry, "cs_5_0", flags, 0, &blob, &errs);
		if (FAILED(r)) {
			if (errs) {
				OOVR_LOGF("ASW: Shader compile error (%s): %s", entry, (char*)errs->GetBufferPointer());
				errs->Release();
			}
			return false;
		}
		if (errs) errs->Release();
		r = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outCS);
		blob->Release();
		if (FAILED(r)) {
			OOVR_LOGF("ASW: CreateComputeShader(%s) failed hr=0x%08x", entry, (unsigned)r);
			return false;
		}
		return true;
	};

	if (!compileEntry("CSClear", &m_clearCS) ||
	    !compileEntry("CSForward", &m_forwardCS) ||
	    !compileEntry("CSForwardDepth", &m_forwardDepthCS) ||
	    !compileEntry("CSForwardColor", &m_forwardColorCS) ||
	    !compileEntry("CSDilate", &m_dilateCS)) {
		OOVR_LOG("ASW: Forward scatter shader compilation failed (non-fatal — backward warp still available)");
		// Non-fatal: backward warp (CSMain) still works
	} else {
		OOVR_LOG("ASW: Forward scatter shaders compiled (CSClear, CSForwardDepth, CSForwardColor, CSForward, CSDilate)");
	}

	// Constant buffer
	D3D11_BUFFER_DESC cbDesc = {};
	cbDesc.ByteWidth = sizeof(WarpConstants);
	// Pad to 16-byte alignment (WarpConstants is 112 bytes, already aligned)
	cbDesc.ByteWidth = (cbDesc.ByteWidth + 15) & ~15;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateBuffer (CB) failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	// Linear clamp sampler
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = device->CreateSamplerState(&sampDesc, &m_linearSampler);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateSamplerState failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	OOVR_LOG("ASW: Compute shader compiled and ready");
	return true;
}

bool ASWProvider::CreateStagingTextures(ID3D11Device* device)
{
	// When D3D12 warp is active, textures need SHARED flags for cross-API access
	UINT sharedFlags = 0;
	if (m_d3d12Ready)
		sharedFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = m_eyeWidth;
	desc.Height = m_eyeHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	// D3D12 descriptor heap CPU handle (for creating SRV/UAV descriptors)
	D3D12_CPU_DESCRIPTOR_HANDLE heapCpuStart = {};
	if (m_d3d12Ready && m_d3d12SrvUavHeap)
		heapCpuStart = m_d3d12SrvUavHeap->GetCPUDescriptorHandleForHeapStart();

	for (uint32_t slot = 0; slot < kAswCacheSlotCount; ++slot) {
		for (int eye = 0; eye < 2; ++eye) {
			uint32_t descBaseIndex = (slot * 2 + eye) * 3; // 3 SRVs per slot/eye

			// Cached color (RGBA)
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = sharedFlags;
			HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_cachedColor[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateTexture2D color[%u][%d] failed hr=0x%08X", slot, eye, hr);
				return false;
			}
			srvDesc.Format = desc.Format;
			hr = device->CreateShaderResourceView(m_cachedColor[slot][eye], &srvDesc, &m_srvColor[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateSRV color[%u][%d] failed", slot, eye);
				return false;
			}

			// Cached MV (R16G16_FLOAT)
			desc.Format = DXGI_FORMAT_R16G16_FLOAT;
			hr = device->CreateTexture2D(&desc, nullptr, &m_cachedMV[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateTexture2D MV[%u][%d] failed hr=0x%08X", slot, eye, hr);
				return false;
			}
			srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
			hr = device->CreateShaderResourceView(m_cachedMV[slot][eye], &srvDesc, &m_srvMV[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateSRV MV[%u][%d] failed", slot, eye);
				return false;
			}

			// Cached depth (R32_FLOAT — depth is extracted from R24G8 via compute shader
			// in dx11compositor.cpp before CacheFrame. CopySubresourceRegion silently
			// produces zeros for R24G8_TYPELESS depth-stencil textures.)
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			hr = device->CreateTexture2D(&desc, nullptr, &m_cachedDepth[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateTexture2D depth[%u][%d] R32F failed hr=0x%08X", slot, eye, hr);
				return false;
			}
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			hr = device->CreateShaderResourceView(m_cachedDepth[slot][eye], &srvDesc, &m_srvDepth[slot][eye]);
			if (slot == 0 && eye == 0) {
				OOVR_LOGF("ASW: Depth format=R32F, SRV format=%u", srvDesc.Format);
			}
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateSRV depth[%u][%d] failed", slot, eye);
				return false;
			}

			// Share to D3D12 and create SRV descriptors
			if (m_d3d12Ready) {
				ShareTextureD3D11ToD3D12(m_cachedColor[slot][eye], &m_d3d12CachedColor[slot][eye]);
				ShareTextureD3D11ToD3D12(m_cachedMV[slot][eye], &m_d3d12CachedMV[slot][eye]);
				ShareTextureD3D11ToD3D12(m_cachedDepth[slot][eye], &m_d3d12CachedDepth[slot][eye]);

				if (m_d3d12CachedColor[slot][eye] && m_d3d12CachedMV[slot][eye] && m_d3d12CachedDepth[slot][eye]) {
					// Color SRV (t0)
					D3D12_SHADER_RESOURCE_VIEW_DESC d3d12SrvDesc = {};
					d3d12SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					d3d12SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					d3d12SrvDesc.Texture2D.MipLevels = 1;

					D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
					cpuHandle.ptr = heapCpuStart.ptr + (SIZE_T)(descBaseIndex + 0) * m_d3d12HeapDescriptorSize;
					d3d12SrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					m_d3d12Device->CreateShaderResourceView(m_d3d12CachedColor[slot][eye], &d3d12SrvDesc, cpuHandle);

					// MV SRV (t1)
					cpuHandle.ptr = heapCpuStart.ptr + (SIZE_T)(descBaseIndex + 1) * m_d3d12HeapDescriptorSize;
					d3d12SrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
					m_d3d12Device->CreateShaderResourceView(m_d3d12CachedMV[slot][eye], &d3d12SrvDesc, cpuHandle);

					// Depth SRV (t2) — always R32_FLOAT (extracted from R24G8 via compute shader)
					cpuHandle.ptr = heapCpuStart.ptr + (SIZE_T)(descBaseIndex + 2) * m_d3d12HeapDescriptorSize;
					d3d12SrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
					m_d3d12Device->CreateShaderResourceView(m_d3d12CachedDepth[slot][eye], &d3d12SrvDesc, cpuHandle);
				} else {
					OOVR_LOGF("ASW D3D12: Failed to share cache textures slot=%u eye=%d", slot, eye);
					m_d3d12Ready = false;
				}
			}
		}
	}

	for (int eye = 0; eye < 2; eye++) {
		// Warped output (RGBA, UAV for compute shader)
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = sharedFlags;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_warpedOutput[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateTexture2D output[%d] failed hr=0x%08X", eye, hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(m_warpedOutput[eye], &uavDesc, &m_uavOutput[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateUAV output[%d] failed", eye);
			return false;
		}

		// Share warped output to D3D12 and create UAV descriptor
		// Heap layout: [18 SRVs] [eye0: output_uav, atomicDepth_uav] [eye1: output_uav, atomicDepth_uav]
		if (m_d3d12Ready) {
			ShareTextureD3D11ToD3D12(m_warpedOutput[eye], &m_d3d12WarpedOutput[eye]);
			if (m_d3d12WarpedOutput[eye]) {
				uint32_t uavIndex = 3 * kAswCacheSlotCount * 2 + eye * 2; // u0 for this eye
				D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
				cpuHandle.ptr = heapCpuStart.ptr + (SIZE_T)uavIndex * m_d3d12HeapDescriptorSize;
				D3D12_UNORDERED_ACCESS_VIEW_DESC d3d12UavDesc = {};
				d3d12UavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				d3d12UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				m_d3d12Device->CreateUnorderedAccessView(m_d3d12WarpedOutput[eye], nullptr, &d3d12UavDesc, cpuHandle);
			} else {
				OOVR_LOGF("ASW D3D12: Failed to share warped output eye=%d", eye);
				m_d3d12Ready = false;
			}
		}
	}

	// Forward scatter: atomic depth buffers (R32_UINT, per-eye)
	for (int eye = 0; eye < 2; eye++) {
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = sharedFlags;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_atomicDepth[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateTexture2D atomicDepth[%d] failed hr=0x%08X", eye, hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32_UINT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(m_atomicDepth[eye], &uavDesc, &m_uavAtomicDepth[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateUAV atomicDepth[%d] failed", eye);
			return false;
		}

		// Share to D3D12 and create UAV descriptor (u1 slot for this eye)
		if (m_d3d12Ready) {
			ShareTextureD3D11ToD3D12(m_atomicDepth[eye], &m_d3d12AtomicDepth[eye]);
			if (m_d3d12AtomicDepth[eye]) {
				uint32_t uavIndex = 3 * kAswCacheSlotCount * 2 + eye * 2 + 1; // u1 for this eye
				D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
				cpuHandle.ptr = heapCpuStart.ptr + (SIZE_T)uavIndex * m_d3d12HeapDescriptorSize;
				D3D12_UNORDERED_ACCESS_VIEW_DESC d3d12UavDesc = {};
				d3d12UavDesc.Format = DXGI_FORMAT_R32_UINT;
				d3d12UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				m_d3d12Device->CreateUnorderedAccessView(m_d3d12AtomicDepth[eye], nullptr, &d3d12UavDesc, cpuHandle);
			} else {
				OOVR_LOGF("ASW D3D12: Failed to share atomicDepth eye=%d", eye);
				m_d3d12Ready = false;
			}
		}
	}

	OOVR_LOGF("ASW: Cache textures created (%u slots x 2 eyes, d3d12=%d)", kAswCacheSlotCount, m_d3d12Ready ? 1 : 0);
	return true;
}

bool ASWProvider::CreateOutputSwapchain(uint32_t width, uint32_t height)
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
	    | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	XrResult res = xrCreateSwapchain(xr_session.get(), &ci, &m_outputSwapchain);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW: xrCreateSwapchain failed (%ux%u) result=%d", width, height, (int)res);
		return false;
	}

	// Enumerate images — cache first
	uint32_t imageCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_outputSwapchain, 0, &imageCount, nullptr));
	if (imageCount == 0) {
		OOVR_LOG("ASW: Output swapchain has 0 images");
		xrDestroySwapchain(m_outputSwapchain);
		m_outputSwapchain = {};
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_outputSwapchain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()));

	m_outputSwapchainImages.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++)
		m_outputSwapchainImages[i] = images[i].texture;

	// Try to share swapchain images to D3D12 for direct copy (bypasses D3D11 GPU queue)
	m_d3d12DirectCopy = false;
	if (m_d3d12Ready && imageCount <= kMaxSwapchainImages) {
		// Log swapchain image flags for diagnostics
		{
			D3D11_TEXTURE2D_DESC desc;
			m_outputSwapchainImages[0]->GetDesc(&desc);
			OOVR_LOGF("ASW D3D12: Swapchain image[0] Format=%u BindFlags=0x%X MiscFlags=0x%X Usage=%d",
			    desc.Format, desc.BindFlags, desc.MiscFlags, desc.Usage);
		}

		bool allShared = true;
		for (uint32_t i = 0; i < imageCount; i++) {
			HANDLE handle = nullptr;
			HRESULT hr;

			// Try 1: NT handle sharing (IDXGIResource1::CreateSharedHandle)
			IDXGIResource1* dxgiRes1 = nullptr;
			hr = m_outputSwapchainImages[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes1));
			if (SUCCEEDED(hr)) {
				hr = dxgiRes1->CreateSharedHandle(nullptr,
				    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
				dxgiRes1->Release();
				if (SUCCEEDED(hr) && handle) {
					hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(&m_d3d12SwapchainImages[i]));
					CloseHandle(handle);
					if (SUCCEEDED(hr)) {
						if (i == 0)
							OOVR_LOG("ASW D3D12: Swapchain sharing via NT handle succeeded");
						continue;
					}
					OOVR_LOGF("ASW D3D12: Swapchain image[%u] NT handle OpenSharedHandle failed (hr=0x%08X)", i, hr);
				}
			}

			// Try 2: Legacy sharing (IDXGIResource::GetSharedHandle)
			IDXGIResource* dxgiRes = nullptr;
			hr = m_outputSwapchainImages[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
			if (SUCCEEDED(hr)) {
				handle = nullptr;
				hr = dxgiRes->GetSharedHandle(&handle);
				dxgiRes->Release();
				if (SUCCEEDED(hr) && handle) {
					hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(&m_d3d12SwapchainImages[i]));
					// Legacy handles are global — don't CloseHandle
					if (SUCCEEDED(hr)) {
						if (i == 0)
							OOVR_LOG("ASW D3D12: Swapchain sharing via legacy handle succeeded");
						continue;
					}
					OOVR_LOGF("ASW D3D12: Swapchain image[%u] legacy OpenSharedHandle failed (hr=0x%08X)", i, hr);
				} else {
					OOVR_LOGF("ASW D3D12: Swapchain image[%u] GetSharedHandle failed (hr=0x%08X)", i, hr);
				}
			}

			allShared = false;
			break;
		}
		if (allShared) {
			m_d3d12DirectCopy = true;
			OOVR_LOGF("ASW D3D12: Swapchain images shared to D3D12 (%u images) — direct copy enabled", imageCount);
		} else {
			// Clean up any partially shared images
			for (uint32_t i = 0; i < kMaxSwapchainImages; i++) {
				if (m_d3d12SwapchainImages[i]) {
					m_d3d12SwapchainImages[i]->Release();
					m_d3d12SwapchainImages[i] = nullptr;
				}
			}
			OOVR_LOG("ASW D3D12: Swapchain sharing failed — falling back to D3D11 copy");
		}
	}

	OOVR_LOGF("ASW: Output swapchain created %ux%u (%u images, d3d12DirectCopy=%d)", width, height, imageCount, m_d3d12DirectCopy ? 1 : 0);
	return true;
}

XrRect2Di ASWProvider::GetOutputRect(int eye) const
{
	XrRect2Di rect = {};
	rect.offset.x = eye * (int32_t)m_eyeWidth;
	rect.offset.y = 0;
	rect.extent.width = (int32_t)m_eyeWidth;
	rect.extent.height = (int32_t)m_eyeHeight;
	return rect;
}

bool ASWProvider::CreateDepthSwapchain(uint32_t width, uint32_t height)
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = DXGI_FORMAT_D32_FLOAT;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	XrResult res = xrCreateSwapchain(xr_session.get(), &ci, &m_depthSwapchain);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW: xrCreateSwapchain (depth) failed (%ux%u) result=%d", width, height, (int)res);
		return false;
	}

	uint32_t imageCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_depthSwapchain, 0, &imageCount, nullptr));
	if (imageCount == 0) {
		OOVR_LOG("ASW: Depth swapchain has 0 images");
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = {};
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_depthSwapchain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()));

	m_depthSwapchainImages.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++)
		m_depthSwapchainImages[i] = images[i].texture;

	OOVR_LOGF("ASW: Depth swapchain created %ux%u (%u images)", width, height, imageCount);
	return true;
}

// ============================================================================
// D3D12 warp pipeline setup
// ============================================================================

bool ASWProvider::CreateDX12Device(IDXGIAdapter* adapter)
{
	HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: D3D12CreateDevice failed (hr=0x%08X)", hr);
		return false;
	}

	// DIRECT command queue — COMPUTE type crashes some drivers (see Fsr3Upscaler)
	// COMPUTE queue: runs on async compute hardware, physically separate from the
	// graphics pipe. D3D11 game draws run on the graphics pipe; warp compute runs
	// on the async compute pipe concurrently with ZERO contention.
	// Dispatch + CopyTextureRegion are both supported on COMPUTE queues.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_d3d12CmdQueue));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommandQueue(COMPUTE) failed (hr=0x%08X)", hr);
		return false;
	}

	hr = m_d3d12Device->CreateCommandAllocator(
	    D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_d3d12CmdAlloc));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommandAllocator(COMPUTE) failed (hr=0x%08X)", hr);
		return false;
	}

	hr = m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
	    m_d3d12CmdAlloc, nullptr, IID_PPV_ARGS(&m_d3d12CmdList));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommandList failed (hr=0x%08X)", hr);
		return false;
	}
	// Command list starts recording; close until first use
	m_d3d12CmdList->Close();

	// Second allocator + list for game staging→swapchain copies (step G2)
	hr = m_d3d12Device->CreateCommandAllocator(
	    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_d3d12GameCopyCmdAlloc));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommandAllocator(gameCopy) failed (hr=0x%08X)", hr);
		return false;
	}
	hr = m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	    m_d3d12GameCopyCmdAlloc, nullptr, IID_PPV_ARGS(&m_d3d12GameCopyCmdList));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommandList(gameCopy) failed (hr=0x%08X)", hr);
		return false;
	}
	m_d3d12GameCopyCmdList->Close();
	m_gameCopyFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	OOVR_LOG("ASW D3D12: Device + DIRECT queue created on same adapter");
	return true;
}

bool ASWProvider::CreateSharedFence()
{
	HRESULT hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d12Fence));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateFence(SHARED) failed (hr=0x%08X)", hr);
		return false;
	}

	hr = m_d3d12Device->CreateSharedHandle(m_d3d12Fence, nullptr, GENERIC_ALL, nullptr, &m_fenceSharedHandle);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateSharedHandle(fence) failed (hr=0x%08X)", hr);
		return false;
	}

	hr = m_d3d11Device5->OpenSharedFence(m_fenceSharedHandle, IID_PPV_ARGS(&m_d3d11Fence));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: OpenSharedFence failed (hr=0x%08X)", hr);
		return false;
	}

	m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	m_fenceValue = 0;
	m_cacheFenceValue.store(0, std::memory_order_relaxed);

	// ── Gap fence: separate shared fence for GPU-side stall (D3D12→D3D11) ──
	hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d12GapFence));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateFence(gap,SHARED) failed (hr=0x%08X)", hr);
		return false;
	}
	hr = m_d3d12Device->CreateSharedHandle(m_d3d12GapFence, nullptr, GENERIC_ALL, nullptr, &m_gapFenceSharedHandle);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateSharedHandle(gap) failed (hr=0x%08X)", hr);
		return false;
	}
	hr = m_d3d11Device5->OpenSharedFence(m_gapFenceSharedHandle, IID_PPV_ARGS(&m_d3d11GapFence));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: OpenSharedFence(gap) failed (hr=0x%08X)", hr);
		return false;
	}
	m_gapFenceValue.store(0, std::memory_order_relaxed);
	m_gapTargetValue.store(0, std::memory_order_relaxed);

	OOVR_LOG("ASW D3D12: Cross-API shared fence + gap fence created");
	return true;
}

void ASWProvider::ShareTextureD3D11ToD3D12(ID3D11Texture2D* d3d11Tex, ID3D12Resource** outD3d12)
{
	*outD3d12 = nullptr;
	IDXGIResource1* dxgiRes = nullptr;
	HRESULT hr = d3d11Tex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: QI for IDXGIResource1 failed (hr=0x%08X)", hr);
		return;
	}

	HANDLE handle = nullptr;
	hr = dxgiRes->CreateSharedHandle(nullptr,
	    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
	dxgiRes->Release();
	if (FAILED(hr) || !handle) {
		OOVR_LOGF("ASW D3D12: CreateSharedHandle failed (hr=0x%08X)", hr);
		return;
	}

	hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(outD3d12));
	CloseHandle(handle);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: OpenSharedHandle failed (hr=0x%08X)", hr);
		*outD3d12 = nullptr;
	}
}

bool ASWProvider::CreateDX12ComputePipeline()
{
	// 1. Compile HLSL to cs_5_1 (D3D12 requires SM 5.1+ for root signature binding)
	DWORD flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled = nullptr;
	ID3DBlob* errors = nullptr;
	std::string csMainDX12Src = std::string(s_warpShaderHLSL) + s_warpShaderDebugHLSL;
	HRESULT hr = D3DCompile(csMainDX12Src.c_str(), csMainDX12Src.size(),
	    "ASWWarp_DX12", nullptr, nullptr, "CSMain", "cs_5_1", flags, 0, &compiled, &errors);
	if (FAILED(hr)) {
		if (errors) {
			OOVR_LOGF("ASW D3D12: Shader compile error: %s", (char*)errors->GetBufferPointer());
			errors->Release();
		}
		return false;
	}
	if (errors)
		errors->Release();

	// 2. Root signature: CBV(b0) + SRV table(t0-t2) + UAV table(u0-u1) + static sampler(s0)
	D3D12_DESCRIPTOR_RANGE1 srvRange = {};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 3;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE1 uavRange = {};
	uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uavRange.NumDescriptors = 2;  // u0 = output, u1 = atomicDepth
	uavRange.BaseShaderRegister = 0;
	uavRange.RegisterSpace = 0;
	uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
	uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER1 rootParams[3] = {};
	// Param 0: Root CBV (b0)
	rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParams[0].Descriptor.ShaderRegister = 0;
	rootParams[0].Descriptor.RegisterSpace = 0;
	rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	// Param 1: SRV descriptor table (t0-t2)
	rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
	rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	// Param 2: UAV descriptor table (u0-u1)
	rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
	rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC staticSampler = {};
	staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.ShaderRegister = 0;
	staticSampler.RegisterSpace = 0;
	staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	staticSampler.MaxLOD = D3D12_FLOAT32_MAX;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsDesc.Desc_1_1.NumParameters = 3;
	rsDesc.Desc_1_1.pParameters = rootParams;
	rsDesc.Desc_1_1.NumStaticSamplers = 1;
	rsDesc.Desc_1_1.pStaticSamplers = &staticSampler;

	ID3DBlob* serialized = nullptr;
	ID3DBlob* rsErrors = nullptr;
	hr = D3D12SerializeVersionedRootSignature(&rsDesc, &serialized, &rsErrors);
	if (FAILED(hr)) {
		if (rsErrors) {
			OOVR_LOGF("ASW D3D12: Root signature serialize error: %s", (char*)rsErrors->GetBufferPointer());
			rsErrors->Release();
		}
		compiled->Release();
		return false;
	}
	if (rsErrors)
		rsErrors->Release();

	hr = m_d3d12Device->CreateRootSignature(0, serialized->GetBufferPointer(),
	    serialized->GetBufferSize(), IID_PPV_ARGS(&m_d3d12RootSig));
	serialized->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateRootSignature failed (hr=0x%08X)", hr);
		compiled->Release();
		return false;
	}

	// 3. Compute PSOs (CSMain + forward scatter shaders)
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_d3d12RootSig;
	psoDesc.CS.pShaderBytecode = compiled->GetBufferPointer();
	psoDesc.CS.BytecodeLength = compiled->GetBufferSize();
	hr = m_d3d12Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_d3d12PipelineState));
	compiled->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateComputePipelineState(CSMain) failed (hr=0x%08X)", hr);
		return false;
	}

	// Compile and create PSOs for forward scatter shaders
	// Full shader source for forward scatter compilation (base + forward scatter parts)
	std::string fullDX12Src = std::string(s_warpShaderHLSL) + s_warpShaderDebugHLSL + s_forwardScatterHLSL;
	auto compileDX12Entry = [&](const char* entry, ID3D12PipelineState** outPSO) -> bool {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errs = nullptr;
		HRESULT r = D3DCompile(fullDX12Src.c_str(), fullDX12Src.size(),
		    entry, nullptr, nullptr, entry, "cs_5_1", flags, 0, &blob, &errs);
		if (FAILED(r)) {
			if (errs) {
				OOVR_LOGF("ASW D3D12: Shader compile error (%s): %s", entry, (char*)errs->GetBufferPointer());
				errs->Release();
			}
			return false;
		}
		if (errs) errs->Release();
		D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = m_d3d12RootSig;
		pd.CS.pShaderBytecode = blob->GetBufferPointer();
		pd.CS.BytecodeLength = blob->GetBufferSize();
		r = m_d3d12Device->CreateComputePipelineState(&pd, IID_PPV_ARGS(outPSO));
		blob->Release();
		if (FAILED(r)) {
			OOVR_LOGF("ASW D3D12: CreateComputePipelineState(%s) failed (hr=0x%08X)", entry, r);
			return false;
		}
		return true;
	};

	if (!compileDX12Entry("CSClear", &m_d3d12ClearPSO) ||
	    !compileDX12Entry("CSForward", &m_d3d12ForwardPSO) ||
	    !compileDX12Entry("CSForwardDepth", &m_d3d12ForwardDepthPSO) ||
	    !compileDX12Entry("CSForwardColor", &m_d3d12ForwardColorPSO) ||
	    !compileDX12Entry("CSDilate", &m_d3d12DilatePSO)) {
		OOVR_LOG("ASW D3D12: Forward scatter PSO creation failed (non-fatal)");
	} else {
		OOVR_LOG("ASW D3D12: Forward scatter PSOs created (CSClear, CSForwardDepth, CSForwardColor, CSDilate)");
	}

	// 4. Descriptor heap: 3 SRVs per slot/eye + 4 UAVs (output + atomicDepth per eye)
	// Layout: [slot0_eye0: colorSRV, mvSRV, depthSRV] [slot0_eye1: ...] ...
	//         [eye0: output_uav, atomicDepth_uav] [eye1: output_uav, atomicDepth_uav]
	uint32_t totalDescriptors = 3 * kAswCacheSlotCount * 2 + 4;
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = totalDescriptors;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = m_d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_d3d12SrvUavHeap));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateDescriptorHeap failed (hr=0x%08X)", hr);
		return false;
	}
	m_d3d12HeapDescriptorSize = m_d3d12Device->GetDescriptorHandleIncrementSize(
	    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// 5. Upload-heap constant buffer: 2 x 256-byte-aligned regions (one per eye).
	// Eye 0 and eye 1 dispatches are recorded into the same command list before
	// execution, so each eye needs its own CB region to avoid eye 1 overwriting eye 0.
	m_d3d12CbEyeStride = (sizeof(WarpConstants) + 255) & ~255;
	uint32_t cbSize = m_d3d12CbEyeStride * 2;
	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC cbResDesc = {};
	cbResDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbResDesc.Alignment = 0;
	cbResDesc.Width = cbSize;
	cbResDesc.Height = 1;
	cbResDesc.DepthOrArraySize = 1;
	cbResDesc.MipLevels = 1;
	cbResDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbResDesc.SampleDesc.Count = 1;
	cbResDesc.SampleDesc.Quality = 0;
	cbResDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	cbResDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	hr = m_d3d12Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
	    &cbResDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_d3d12ConstantBuffer));
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CreateCommittedResource(CB) failed (hr=0x%08X)", hr);
		return false;
	}

	// Persistent map
	D3D12_RANGE readRange = { 0, 0 }; // We won't read from CPU
	hr = m_d3d12ConstantBuffer->Map(0, &readRange, &m_d3d12CbMappedPtr);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW D3D12: CB Map failed (hr=0x%08X)", hr);
		return false;
	}

	OOVR_LOGF("ASW D3D12: Compute pipeline ready (%u descriptors, CB %u bytes)", totalDescriptors, cbSize);
	return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE ASWProvider::GetSrvGpuHandle(int slot, int eye) const
{
	// Layout: [slot0_eye0: 3 SRVs] [slot0_eye1: 3 SRVs] [slot1_eye0: ...] ... [UAVs]
	uint32_t index = (slot * 2 + eye) * 3;
	D3D12_GPU_DESCRIPTOR_HANDLE handle = m_d3d12SrvUavHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * m_d3d12HeapDescriptorSize;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE ASWProvider::GetUavGpuHandle(int eye) const
{
	// UAVs are after all SRVs: [eye0: output, atomicDepth] [eye1: output, atomicDepth]
	uint32_t index = 3 * kAswCacheSlotCount * 2 + eye * 2;
	D3D12_GPU_DESCRIPTOR_HANDLE handle = m_d3d12SrvUavHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (SIZE_T)index * m_d3d12HeapDescriptorSize;
	return handle;
}

void ASWProvider::SignalCacheDone(ID3D11DeviceContext* ctx)
{
	if (!m_d3d12Ready || !m_d3d11Fence)
		return;

	ID3D11DeviceContext4* ctx4 = nullptr;
	ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
	if (ctx4) {
		uint64_t val = ++m_fenceValue;
		ctx4->Signal(m_d3d11Fence, val);
		m_cacheFenceValue.store(val, std::memory_order_release);
		// Store per-slot fence value for the just-published slot
		int pub = m_publishedSlot.load(std::memory_order_acquire);
		if (pub >= 0 && pub < (int)kAswCacheSlotCount)
			m_slotCacheFenceValue[pub] = val;
		ctx4->Release();
	}
}

bool ASWProvider::InsertGpuGap(ID3D11DeviceContext* ctx)
{
	if (!m_gpuGapEnabled.load(std::memory_order_acquire))
		return false;
	if (!m_d3d12Ready || !m_d3d11GapFence)
		return false;

	ID3D11DeviceContext4* ctx4 = nullptr;
	ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
	if (!ctx4)
		return false;

	uint64_t target = m_gapFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
	m_gapTargetValue.store(target, std::memory_order_release);

	// GPU-side wait: D3D11 command stream stalls until gap fence reaches target.
	// CPU returns immediately — no blocking. Right eye draws queue behind this.
	ctx4->Wait(m_d3d11GapFence, target);
	ctx4->Release();

	{
		static int s = 0;
		if (s++ < 30 || (s % 300 == 0))
			OOVR_LOGF("ASW InsertGpuGap: target=%llu (GPU stall inserted after left eye)", (unsigned long long)target);
	}
	return true;
}

void ASWProvider::ReleaseGpuGap()
{
	if (!m_d3d12Ready || !m_d3d12GapFence || !m_d3d12CmdQueue)
		return;

	uint64_t target = m_gapTargetValue.load(std::memory_order_acquire);
	if (target == 0)
		return; // no gap pending

	// Signal the gap fence from the D3D12 queue.
	// At this point (~13ms), the D3D12 queue is idle (no pending warp work).
	// The signal fires immediately, releasing the D3D11 GPU stall.
	m_d3d12CmdQueue->Signal(m_d3d12GapFence, target);

	{
		static int s = 0;
		if (s++ < 30 || (s % 300 == 0))
			OOVR_LOGF("ASW ReleaseGpuGap: signaled target=%llu (GPU resumes right eye)", (unsigned long long)target);
	}
}

int ASWProvider::GetLatestReadySlot(int preferredSlot) const
{
	if (!m_d3d12Ready || !m_d3d12Fence)
		return preferredSlot;

	uint64_t completed = m_d3d12Fence->GetCompletedValue();

	// Check preferred slot first (current published slot)
	if (preferredSlot >= 0 && preferredSlot < (int)kAswCacheSlotCount) {
		if (m_slotCacheFenceValue[preferredSlot] > 0 && completed >= m_slotCacheFenceValue[preferredSlot])
			return preferredSlot;
	}

	// Preferred slot's fence hasn't completed — find the most recent ready alternative.
	// "Most recent" = highest fence value that's completed (higher fence = newer frame).
	int bestSlot = -1;
	uint64_t bestFence = 0;
	for (int i = 0; i < (int)kAswCacheSlotCount; i++) {
		if (m_slotCacheFenceValue[i] > 0 && completed >= m_slotCacheFenceValue[i]) {
			if (m_slotCacheFenceValue[i] > bestFence) {
				bestFence = m_slotCacheFenceValue[i];
				bestSlot = i;
			}
		}
	}
	return bestSlot;
}

bool ASWProvider::WaitForFenceValue(uint64_t value, DWORD timeoutMs)
{
	if (!m_d3d12Fence || !m_fenceEvent)
		return false;
	if (m_d3d12Fence->GetCompletedValue() >= value)
		return true;
	HRESULT hr = m_d3d12Fence->SetEventOnCompletion(value, m_fenceEvent);
	if (FAILED(hr))
		return false;
	return (WaitForSingleObject(m_fenceEvent, timeoutMs) == WAIT_OBJECT_0);
}

// ============================================================================
// Per-frame operations
// ============================================================================

// SEH wrapper for CopySubresourceRegion — catches AV from TOCTOU race
// when bridge texture is freed between validation and copy.
// Must be a standalone function: __try/__except can't coexist with C++ destructors.
static bool SafeBridgeCopy(ID3D11DeviceContext* ctx,
    ID3D11Resource* dst, UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* srcBox)
{
	__try {
		ctx->CopySubresourceRegion(dst, dstSub, dstX, dstY, dstZ, src, srcSub, srcBox);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
	        ? EXCEPTION_EXECUTE_HANDLER
	        : EXCEPTION_CONTINUE_SEARCH) {
		return false;
	}
}

void ASWProvider::CacheFrame(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
    const XrPosef& eyePose, const XrFovf& eyeFov,
    float nearZ, float farZ)
{
	if (!m_ready || eye < 0 || eye > 1)
		return;
	const bool enableReadbackDiag = (oovr_global_configuration.ASWDebugMode() >= 10);
	const int slot = m_buildSlot;

	if (eye == 0) {
		// Default dimensions when optional MV/depth copies are unavailable.
		m_slotMVDataW[slot] = m_eyeWidth;
		m_slotMVDataH[slot] = m_eyeHeight;
		m_slotDepthDataW[slot] = m_eyeWidth;
		m_slotDepthDataH[slot] = m_eyeHeight;
	}

	// Copy color (game eye texture -> cached slot)
	if (colorTex && colorRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedColor[slot][eye], 0, 0, 0, 0,
		        colorTex, 0, colorRegion)) {
			OOVR_LOG("ASW: TOCTOU - color texture freed during copy");
			return;
		}
	}

	// Copy motion vectors (bridge MV -> cached slot)
	if (mvTex && mvRegion && fabsf(oovr_global_configuration.ASWMVConfidence()) > 0.001f) {
		if (!SafeBridgeCopy(ctx, m_cachedMV[slot][eye], 0, 0, 0, 0,
		        mvTex, 0, mvRegion)) {
			OOVR_LOG("ASW: TOCTOU - MV texture freed during copy");
			return;
		}
		m_slotMVDataW[slot] = mvRegion->right - mvRegion->left;
		m_slotMVDataH[slot] = mvRegion->bottom - mvRegion->top;

		// Optional readback diagnostics (left eye only)
		{
			static int s_mvReadbackFrame = 0;
			s_mvReadbackFrame++;
			if (enableReadbackDiag && s_mvReadbackFrame > 90 && (s_mvReadbackFrame % 30) == 0 && eye == 0) {
				static int s_mvSampleCount = 0;
				if (s_mvSampleCount < 60) {
					s_mvSampleCount++;
					ID3D11Device* dev = nullptr;
					ctx->GetDevice(&dev);
					if (dev) {
						D3D11_TEXTURE2D_DESC readDesc;
						m_cachedMV[slot][eye]->GetDesc(&readDesc);
						readDesc.Usage = D3D11_USAGE_STAGING;
						readDesc.BindFlags = 0;
						readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
						ID3D11Texture2D* readback = nullptr;
						if (SUCCEEDED(dev->CreateTexture2D(&readDesc, nullptr, &readback))) {
							ctx->CopyResource(readback, m_cachedMV[slot][eye]);
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &mapped))) {
								uint32_t mw = m_slotMVDataW[slot] > 0 ? m_slotMVDataW[slot] : readDesc.Width;
								uint32_t mh = m_slotMVDataH[slot] > 0 ? m_slotMVDataH[slot] : readDesc.Height;
								uint32_t bytesPerRow = mapped.RowPitch;
								auto readMV = [&](uint32_t px, uint32_t py, float& outX, float& outY) {
									const uint16_t* row = (const uint16_t*)((const uint8_t*)mapped.pData + py * bytesPerRow);
									uint16_t hx = row[px * 2 + 0];
									uint16_t hy = row[px * 2 + 1];
									auto h2f = [](uint16_t h) -> float {
										uint32_t sign = (h >> 15) & 1;
										uint32_t exp = (h >> 10) & 0x1F;
										uint32_t mant = h & 0x3FF;
										if (exp == 0)
											return sign ? -0.0f : 0.0f;
										if (exp == 31)
											return sign ? -1e30f : 1e30f;
										float f = ldexpf((float)(mant + 1024) / 1024.0f, (int)exp - 15);
										return sign ? -f : f;
									};
									outX = h2f(hx);
									outY = h2f(hy);
								};
								float cx, cy, lx, ly, rx, ry, tx, ty, bx, by;
								readMV(mw / 2, mh / 2, cx, cy);
								readMV(mw / 8, mh / 2, lx, ly);
								readMV(mw * 7 / 8, mh / 2, rx, ry);
								readMV(mw / 2, mh / 8, tx, ty);
								readMV(mw / 2, mh * 7 / 8, bx, by);
								float headYawDeg = 0.0f;
								if (m_slotHasPrevPose[slot]) {
									XrQuaternionf pInv;
									QuatInverse(m_slotPrevPose[slot][eye].orientation, pInv);
									XrQuaternionf dq;
									QuatMultiply(pInv, m_slotPose[slot][eye].orientation, dq);
									headYawDeg = atan2f(2.0f * (dq.w * dq.y + dq.x * dq.z),
									                 1.0f - 2.0f * (dq.x * dq.x + dq.y * dq.y))
									    * 57.2958f;
								}
								OOVR_LOGF("ASW MV[%d]: center=(%.5f,%.5f) left=(%.5f,%.5f) right=(%.5f,%.5f) top=(%.5f,%.5f) bot=(%.5f,%.5f) headYaw=%.2fdeg (data %ux%u)",
								    s_mvSampleCount, cx, cy, lx, ly, rx, ry, tx, ty, bx, by, headYawDeg, mw, mh);
								ctx->Unmap(readback, 0);
							}
							readback->Release();
						}
						dev->Release();
					}
				}
			}
		}
	}

	// Copy depth (R24G8_TYPELESS or R32F -> cached slot, same-format copy)
	{
		static int s_depthNullCount = 0;
		if (!depthTex || !depthRegion) {
			if (s_depthNullCount < 5) {
				OOVR_LOGF("ASW: depth %s for eye %d (depthTex=%p depthRegion=%p)",
				    !depthTex ? "NULL" : "no-region", eye, depthTex, depthRegion);
				s_depthNullCount++;
			}
		} else {
			s_depthNullCount = 0;
		}
	}
	if (depthTex && depthRegion) {
		// Log source depth format once to diagnose format mismatch
		{
			static int s_depthFmtLog = 0;
			if (s_depthFmtLog < 3) {
				D3D11_TEXTURE2D_DESC srcDesc = {};
				depthTex->GetDesc(&srcDesc);
				D3D11_TEXTURE2D_DESC dstDesc = {};
				m_cachedDepth[slot][eye]->GetDesc(&dstDesc);
				OOVR_LOGF("ASW CacheFrame: depth src fmt=%u (%ux%u bind=0x%X) -> dst fmt=%u (%ux%u bind=0x%X) region=[%u,%u - %u,%u]",
				    srcDesc.Format, srcDesc.Width, srcDesc.Height, srcDesc.BindFlags,
				    dstDesc.Format, dstDesc.Width, dstDesc.Height, dstDesc.BindFlags,
				    depthRegion->left, depthRegion->top, depthRegion->right, depthRegion->bottom);
				s_depthFmtLog++;
			}
		}
		if (!SafeBridgeCopy(ctx, m_cachedDepth[slot][eye], 0, 0, 0, 0,
		        depthTex, 0, depthRegion)) {
			OOVR_LOG("ASW: TOCTOU - depth texture freed during copy");
			return;
		}
		m_slotDepthDataW[slot] = depthRegion->right - depthRegion->left;
		m_slotDepthDataH[slot] = depthRegion->bottom - depthRegion->top;

		// Readback a single pixel from cached depth to verify copy succeeded
		{
			static int s_readbackCount = 0;
			if (s_readbackCount < 5 && eye == 0) {
				D3D11_TEXTURE2D_DESC dstDesc = {};
				m_cachedDepth[slot][eye]->GetDesc(&dstDesc);
				D3D11_TEXTURE2D_DESC stg = dstDesc;
				stg.Width = 1; stg.Height = 1;
				stg.Usage = D3D11_USAGE_STAGING;
				stg.BindFlags = 0; stg.MiscFlags = 0;
				stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ID3D11Texture2D* staging = nullptr;
				ID3D11Device* dev = nullptr;
				ctx->GetDevice(&dev);
				if (dev && SUCCEEDED(dev->CreateTexture2D(&stg, nullptr, &staging))) {
					// Copy center pixel from cached depth (now R32F)
					uint32_t cx = dstDesc.Width / 2;
					uint32_t cy = dstDesc.Height / 2;
					D3D11_BOX box = { cx, cy, 0, cx + 1, cy + 1, 1 };
					ctx->CopySubresourceRegion(staging, 0, 0, 0, 0,
					    m_cachedDepth[slot][eye], 0, &box);
					D3D11_MAPPED_SUBRESOURCE mapped;
					if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
						float val = *(const float*)mapped.pData;
						OOVR_LOGF("ASW CacheFrame READBACK: cachedDepth center = %.6f (0=near, 1=far)", val);
						ctx->Unmap(staging, 0);
					}
					staging->Release();
				}
				if (dev) dev->Release();
				s_readbackCount++;
			}
		}
	}

	m_slotPose[slot][eye] = eyePose;
	m_slotFov[slot][eye] = eyeFov;
	m_slotNear[slot] = nearZ;
	m_slotFar[slot] = farZ;

	if (eye == 0)
		m_slotFrameId[slot] = ++m_frameCounter;

	m_buildEyeReady[eye] = true;
	if (eye == 1 && m_buildEyeReady[0]) {
		if (m_hasLastPublishedPose) {
			m_slotHasPrevPose[slot] = true;
			m_slotPrevPose[slot][0] = m_lastPublishedPose[0];
			m_slotPrevPose[slot][1] = m_lastPublishedPose[1];
		} else {
			m_slotHasPrevPose[slot] = false;
		}

		m_lastPublishedPose[0] = m_slotPose[slot][0];
		m_lastPublishedPose[1] = m_slotPose[slot][1];
		m_hasLastPublishedPose = true;

		m_slotTimestamp[slot] = std::chrono::steady_clock::now();
		m_previousPublishedSlot.store(m_publishedSlot.load(std::memory_order_relaxed), std::memory_order_release);
		m_publishedSlot.store(slot, std::memory_order_release);

		int next = (slot + 1) % (int)kAswCacheSlotCount;
		int protectedSlot = m_warpReadSlot.load(std::memory_order_acquire);
		if (next == protectedSlot)
			next = (next + 1) % (int)kAswCacheSlotCount;
		m_buildSlot = next;
		m_buildEyeReady[0] = false;
		m_buildEyeReady[1] = false;

		static int s = 0;
		if (s++ < 6 || (s % 600 == 0))
			OOVR_LOGF("ASW: Frame cached - slot=%d near=%.2f far=%.1f fid=%llu", slot, nearZ, farZ, m_slotFrameId[slot]);
	}
}

void ASWProvider::WaitForCacheFence()
{
	// N-1 warping: use the previous slot's fence (signaled ~27ms ago).
	// This should be instant — the fence was signaled last cycle.
	if (!m_d3d12Ready || !m_d3d12Fence)
		return;
	int slot = m_previousPublishedSlot.load(std::memory_order_acquire);
	if (slot < 0 || slot >= (int)kAswCacheSlotCount)
		return;
	uint64_t cacheVal = m_slotCacheFenceValue[slot];
	if (cacheVal == 0)
		return;

	if (m_d3d12Fence->GetCompletedValue() < cacheVal) {
		m_d3d12Fence->SetEventOnCompletion(cacheVal, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, 5000);
		OOVR_LOGF("ASW: N-1 cache fence unexpectedly not ready (val=%llu)", cacheVal);
	}
	m_cacheFenceWaitedEarly = true;
}

// ============================================================================
// Warp diagnostic capture — saves all inputs/outputs for offline iteration
// ============================================================================

static void WriteBMP(const char* path, const uint8_t* pixelData, uint32_t w, uint32_t h,
    uint32_t srcStride, int bytesPerPx, bool swapRB)
{
	uint32_t rowBytes = w * 3;
	uint32_t rowPad = (4 - (rowBytes % 4)) % 4;
	uint32_t imgSize = (rowBytes + rowPad) * h;
	uint8_t hdr[54] = {};
	hdr[0] = 'B'; hdr[1] = 'M';
	*(uint32_t*)(hdr + 2) = 54 + imgSize;
	*(uint32_t*)(hdr + 10) = 54;
	*(uint32_t*)(hdr + 14) = 40;
	*(int32_t*)(hdr + 18) = (int32_t)w;
	*(int32_t*)(hdr + 22) = -(int32_t)h; // top-down
	*(uint16_t*)(hdr + 26) = 1;
	*(uint16_t*)(hdr + 28) = 24;
	*(uint32_t*)(hdr + 34) = imgSize;
	std::ofstream f(path, std::ios::binary);
	if (!f) return;
	f.write((const char*)hdr, 54);
	std::vector<uint8_t> row(rowBytes + rowPad, 0);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t* src = pixelData + y * srcStride;
		for (uint32_t x = 0; x < w; x++) {
			uint8_t r, g, b;
			if (bytesPerPx == 4) {
				r = src[x * 4 + 0]; g = src[x * 4 + 1]; b = src[x * 4 + 2];
			} else if (bytesPerPx == 1) {
				r = g = b = src[x];
			} else {
				r = g = b = 0;
			}
			// BMP = BGR
			row[x * 3 + 0] = swapRB ? r : b;
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = swapRB ? b : r;
		}
		f.write((const char*)row.data(), rowBytes + rowPad);
	}
}

static void WriteRaw(const char* path, const void* data, uint32_t w, uint32_t h,
    uint32_t stride, uint32_t dxgiFormat, uint32_t bpp)
{
	std::ofstream f(path, std::ios::binary);
	if (!f) return;
	// Header: width, height, DXGI format, bytes-per-pixel, row pitch
	uint32_t header[5] = { w, h, dxgiFormat, bpp, stride };
	f.write((const char*)header, sizeof(header));
	for (uint32_t y = 0; y < h; y++)
		f.write((const char*)data + y * stride, w * bpp);
}

static bool CopyAndSaveTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* tex, const std::string& folder, const char* baseName,
    bool saveBMP, bool saveRaw)
{
	if (!tex) return false;
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);

	D3D11_TEXTURE2D_DESC stg = desc;
	stg.Usage = D3D11_USAGE_STAGING;
	stg.BindFlags = 0;
	stg.MiscFlags = 0;
	stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ID3D11Texture2D* staging = nullptr;
	if (FAILED(dev->CreateTexture2D(&stg, nullptr, &staging)))
		return false;

	ctx->CopyResource(staging, tex);

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
		staging->Release();
		return false;
	}

	uint32_t w = desc.Width, h = desc.Height;

	if (saveBMP) {
		std::string bmpPath = folder + "\\" + baseName + ".bmp";
		if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
			WriteBMP(bmpPath.c_str(), (const uint8_t*)mapped.pData, w, h, mapped.RowPitch, 4, false);
		} else if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
			// Convert R24G8 depth to grayscale BMP
			std::vector<uint8_t> gray(w * h);
			for (uint32_t y = 0; y < h; y++) {
				const uint32_t* row = (const uint32_t*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
				for (uint32_t x = 0; x < w; x++) {
					uint32_t d24 = row[x] & 0x00FFFFFF;
					float d = (float)d24 / 16777215.0f;
					float vis = (d - 0.84f) / 0.16f;
					vis = vis < 0.0f ? 0.0f : (vis > 1.0f ? 1.0f : vis);
					gray[y * w + x] = (uint8_t)(vis * 255.0f);
				}
			}
			WriteBMP(bmpPath.c_str(), gray.data(), w, h, w, 1, false);
		} else if (desc.Format == DXGI_FORMAT_R32_FLOAT) {
			// Convert R32F depth to grayscale BMP
			std::vector<uint8_t> gray(w * h);
			for (uint32_t y = 0; y < h; y++) {
				const float* row = (const float*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
				for (uint32_t x = 0; x < w; x++) {
					float d = row[x];
					float vis = (d - 0.84f) / 0.16f;
					vis = vis < 0.0f ? 0.0f : (vis > 1.0f ? 1.0f : vis);
					gray[y * w + x] = (uint8_t)(vis * 255.0f);
				}
			}
			WriteBMP(bmpPath.c_str(), gray.data(), w, h, w, 1, false);
		}
	}

	if (saveRaw) {
		std::string rawPath = folder + "\\" + baseName + ".raw";
		uint32_t bpp = 4;
		if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) bpp = 4;
		else if (desc.Format == DXGI_FORMAT_R32_FLOAT) bpp = 4;
		else if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) bpp = 4;
		else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) bpp = 4;
		WriteRaw(rawPath.c_str(), mapped.pData, w, h, mapped.RowPitch, desc.Format, bpp);
	}

	ctx->Unmap(staging, 0);
	staging->Release();
	return true;
}

void ASWProvider::CaptureWarpDiagnostics(int eye, int slot, const WarpConstants& cb,
    ID3D11DeviceContext* ctx, bool /*isPreDispatch*/)
{
	static int s_warpFrame = 0;
	static int s_captureIdx = 0;

	if (eye != 0) return;

	s_warpFrame++;
	// Start capturing ~10 seconds after game start (375 warp pairs at ~37.5 fps)
	if (s_warpFrame < 375) return;
	if (s_captureIdx >= 30) return;

	// Only capture when pose delta is large enough to show artifacts.
	// Measure off-diagonal rotation magnitude in forwardPoseDelta — these elements
	// are approximately the rotation angle in radians for small rotations.
	float rotMag = fabsf(cb.forwardPoseDelta[1]) + fabsf(cb.forwardPoseDelta[2])
	             + fabsf(cb.forwardPoseDelta[4]) + fabsf(cb.forwardPoseDelta[6])
	             + fabsf(cb.forwardPoseDelta[8]) + fabsf(cb.forwardPoseDelta[9]);
	// Also check translation magnitude
	float transMag = fabsf(cb.forwardPoseDelta[3]) + fabsf(cb.forwardPoseDelta[7])
	               + fabsf(cb.forwardPoseDelta[11]);
	// Threshold: ~0.005 rad total rotation ≈ 7-8px displacement at 3560px.
	// Previous captures had rotMag ~0.002 (too small). Need at least 0.005.
	if (rotMag < 0.005f && transMag < 0.001f) return;

	// Throttle: don't capture every qualifying frame, space them out
	static int s_lastCaptureFrame = 0;
	if (s_warpFrame - s_lastCaptureFrame < 30) return;
	s_lastCaptureFrame = s_warpFrame;

	// Create cycle folder
	std::string folder = "C:\\Users\\JLWel\\AppData\\Local\\OpenXR-Toolkit\\screenshots\\TestWarp\\Cycle"
	    + std::to_string(s_captureIdx);
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (ec) {
		OOVR_LOGF("ASW CAPTURE: Failed to create folder %s: %s", folder.c_str(), ec.message().c_str());
		s_captureIdx++;
		return;
	}

	OOVR_LOGF("ASW CAPTURE: Saving cycle %d to %s (warpFrame=%d)", s_captureIdx, folder.c_str(), s_warpFrame);

	// Save inputs (color, depth, MV)
	CopyAndSaveTexture(m_device, ctx, m_cachedColor[slot][0], folder, "color", true, false);
	CopyAndSaveTexture(m_device, ctx, m_cachedDepth[slot][0], folder, "depth", true, true);
	CopyAndSaveTexture(m_device, ctx, m_cachedMV[slot][0], folder, "mv", false, true);

	// Save WarpConstants as binary
	{
		std::string cbPath = folder + "\\warp_constants.bin";
		std::ofstream f(cbPath, std::ios::binary);
		if (f) f.write((const char*)&cb, sizeof(cb));
	}

	// Save a text summary of key parameters
	{
		std::string infoPath = folder + "\\info.txt";
		std::ofstream f(infoPath);
		if (f) {
			f << "Warp frame: " << s_warpFrame << "\n";
			f << "Slot: " << slot << "\n";
			f << "Resolution: " << cb.resolution[0] << " x " << cb.resolution[1] << "\n";
			f << "Depth resolution: " << cb.depthResolution[0] << " x " << cb.depthResolution[1] << "\n";
			f << "MV resolution: " << cb.mvResolution[0] << " x " << cb.mvResolution[1] << "\n";
			f << "nearZ: " << cb.nearZ << " farZ: " << cb.farZ << "\n";
			f << "depthScale: " << cb.depthScale << "\n";
			f << "fovTan: L=" << cb.fovTanLeft << " R=" << cb.fovTanRight
			  << " U=" << cb.fovTanUp << " D=" << cb.fovTanDown << "\n";
			f << "mvConfidence: " << cb.mvConfidence << "\n";
			f << "mvPixelScale: " << cb.mvPixelScale << "\n";
			f << "debugMode: " << cb.debugMode << "\n";
			f << "hasClipToClipNoLoco: " << cb.hasClipToClipNoLoco << "\n";
			f << "d3d12Ready: " << (m_d3d12Ready ? 1 : 0) << "\n";

			// Depth format
			D3D11_TEXTURE2D_DESC depthDesc;
			if (m_cachedDepth[slot][0]) {
				m_cachedDepth[slot][0]->GetDesc(&depthDesc);
				f << "Depth format: " << depthDesc.Format
				  << " (44=R24G8, 41=R32F, bindFlags=0x" << std::hex << depthDesc.BindFlags << std::dec << ")\n";
			}

			// Forward pose delta matrix
			f << "forwardPoseDelta:\n";
			for (int r = 0; r < 4; r++) {
				f << "  ";
				for (int c = 0; c < 4; c++)
					f << cb.forwardPoseDelta[r * 4 + c] << " ";
				f << "\n";
			}
			f << "poseDeltaMatrix:\n";
			for (int r = 0; r < 4; r++) {
				f << "  ";
				for (int c = 0; c < 4; c++)
					f << cb.poseDeltaMatrix[r * 4 + c] << " ";
				f << "\n";
			}
			f << "headRotMatrix:\n";
			for (int r = 0; r < 4; r++) {
				f << "  ";
				for (int c = 0; c < 4; c++)
					f << cb.headRotMatrix[r * 4 + c] << " ";
				f << "\n";
			}
		}
	}

	// Save warped output — m_warpedOutput[0] contains the PREVIOUS frame's warp result
	// which is complete on the GPU (D3D12 fence was waited before this frame started).
	// This is the warp output corresponding to inputs from 1 cycle ago, not this cycle's
	// inputs. But for visual inspection of warp quality it's still useful.
	if (m_warpedOutput[0]) {
		// Flush any pending D3D11 work to ensure D3D12→D3D11 shared texture is readable
		ctx->Flush();
		CopyAndSaveTexture(m_device, ctx, m_warpedOutput[0], folder, "output_prev", true, false);
	}

	OOVR_LOGF("ASW CAPTURE: Cycle %d complete", s_captureIdx);
	s_captureIdx++;
}

bool ASWProvider::WarpFrame(int eye, const XrPosef& newPose, int slotOverride)
{
	if (!m_ready || eye < 0 || eye > 1)
		return false;

	int slot = -1;
	if (eye == 0) {
		if (slotOverride >= 0 && slotOverride < (int)kAswCacheSlotCount) {
			slot = slotOverride;
		} else {
			// N-1 warping: use the PREVIOUS frame's cache slot.
			// Its fence was signaled last cycle (~27ms ago) — zero wait.
			slot = m_previousPublishedSlot.load(std::memory_order_acquire);
		}
		if (slot < 0)
			return false;
		m_warpReadSlot.store(slot, std::memory_order_release);
	} else {
		slot = m_warpReadSlot.load(std::memory_order_acquire);
		if (slot < 0)
			slot = m_publishedSlot.load(std::memory_order_acquire);
		if (slot < 0)
			return false;
	}

	WarpConstants cb = {};
	// poseDeltaMatrix: backward warp transform (new view → old view) in shader coords.
	// The shader uses +Z-forward but OpenXR uses -Z-forward. The correct transform is
	// F * M * F where M = V_old * W_new (OpenXR backward warp) and F = diag(1,1,-1,1).
	// BuildPoseDeltaMatrix(new, cached) gives M directly. Then apply F conjugation:
	//   - Negate rotation column 2 (m[2], m[6]) and row 2 (m[8], m[9])
	//   - m[10] double-negated → unchanged
	//   - Negate Z translation (m[11]); X,Y translation unchanged
	// This correctly handles ALL rotation axes (yaw, pitch, AND roll).
	BuildPoseDeltaMatrix(newPose, m_slotPose[slot][eye], cb.poseDeltaMatrix);
	// Z-flip conjugation: F * M * F
	cb.poseDeltaMatrix[2]  = -cb.poseDeltaMatrix[2];
	cb.poseDeltaMatrix[6]  = -cb.poseDeltaMatrix[6];
	cb.poseDeltaMatrix[8]  = -cb.poseDeltaMatrix[8];
	cb.poseDeltaMatrix[9]  = -cb.poseDeltaMatrix[9];
	cb.poseDeltaMatrix[11] = -cb.poseDeltaMatrix[11];
	m_precompPose[eye] = newPose;

	// forwardPoseDelta: OLD view → NEW view (forward scatter — inverse of poseDeltaMatrix)
	// BuildPoseDeltaMatrix(cached, new) gives cached→new transform.
	BuildPoseDeltaMatrix(m_slotPose[slot][eye], newPose, cb.forwardPoseDelta);
	// Z-flip conjugation: F * M * F (same as poseDeltaMatrix)
	cb.forwardPoseDelta[2]  = -cb.forwardPoseDelta[2];
	cb.forwardPoseDelta[6]  = -cb.forwardPoseDelta[6];
	cb.forwardPoseDelta[8]  = -cb.forwardPoseDelta[8];
	cb.forwardPoseDelta[9]  = -cb.forwardPoseDelta[9];
	cb.forwardPoseDelta[11] = -cb.forwardPoseDelta[11];

	// locoScreenDir: screen-space locomotion direction from actorPos delta.
	// View-space X → screen X, view-space Y → screen -Y (Y-down screen convention).
	// This is the ACTUAL thumbstick locomotion direction, used by CSDilate to search
	// along the correct axis for trailing-edge mirror fill.
	cb.locoScreenDir[0] = m_locoTransX;
	cb.locoScreenDir[1] = -m_locoTransY;

	cb.debugMode = oovr_global_configuration.ASWDebugMode();

	// headRotMatrix: full frame-to-frame head delta (rotation + translation) from OpenXR poses.
	// NO stick rotation — only head tracking. Used to subtract head motion from game MVs,
	// so the residual captures stick rotation + locomotion for mvOffset correction.
	// BuildPoseDeltaMatrix(old, new) transforms from old→new view space.
	// We need backward direction (same as c2c/game MVs): swap args to get cur→prev.
	if (m_slotHasPrevPose[slot]) {
		BuildPoseDeltaMatrix(m_slotPose[slot][eye], m_slotPrevPose[slot][eye], cb.headRotMatrix);
		// Z-flip conjugation: F * M * F (OpenXR right-hand → Skyrim left-hand)
		cb.headRotMatrix[2]  = -cb.headRotMatrix[2];
		cb.headRotMatrix[6]  = -cb.headRotMatrix[6];
		cb.headRotMatrix[8]  = -cb.headRotMatrix[8];
		cb.headRotMatrix[9]  = -cb.headRotMatrix[9];
		cb.headRotMatrix[11] = -cb.headRotMatrix[11];
	} else {
		for (int i = 0; i < 16; i++)
			cb.headRotMatrix[i] = 0;
		cb.headRotMatrix[0] = cb.headRotMatrix[5] = cb.headRotMatrix[10] = cb.headRotMatrix[15] = 1;
	}

	// Stick turn correction: inject yaw rotation from actorYaw into poseDeltaMatrix.
	// actorYaw (PlayerCharacter::data.angle.z) only changes with stick rotation, not head.
	// Dead zone filters physics wobble. Y-axis rotation in view space (XZ plane).
	float rotS = oovr_global_configuration.ASWRotationScale();
	if (rotS != 0.0f && fabsf(m_locoYaw) > 0.002f) { // ~0.1 degree dead zone
		float theta = m_locoYaw * rotS;
		float co = cosf(theta), si = sinf(theta);
		// Apply to poseDeltaMatrix (backward: new→old)
		float* M = cb.poseDeltaMatrix;
		for (int col = 0; col < 4; col++) {
			float r0 = M[0 * 4 + col];
			float r2 = M[2 * 4 + col];
			M[0 * 4 + col] = co * r0 + si * r2;
			M[2 * 4 + col] = -si * r0 + co * r2;
		}
		// Apply to forwardPoseDelta (forward: old→new, negate theta)
		float coN = co, siN = -si; // cos(-θ)=cos(θ), sin(-θ)=-sin(θ)
		float* F = cb.forwardPoseDelta;
		for (int col = 0; col < 4; col++) {
			float r0 = F[0 * 4 + col];
			float r2 = F[2 * 4 + col];
			F[0 * 4 + col] = coN * r0 + siN * r2;
			F[2 * 4 + col] = -siN * r0 + coN * r2;
		}
	}

	// Apply locomotion translation
	// Scale by translation scale and warp strength
	float masterStr = oovr_global_configuration.ASWWarpStrength();
	float transStr = masterStr * oovr_global_configuration.ASWTranslationScale();
	transStr = (transStr < 0.0f) ? 0.0f : transStr;

	if (transStr != 1.0f) {
		cb.poseDeltaMatrix[3] *= transStr;
		cb.poseDeltaMatrix[7] *= transStr;
		cb.poseDeltaMatrix[11] *= transStr;
		cb.forwardPoseDelta[3] *= transStr;
		cb.forwardPoseDelta[7] *= transStr;
		cb.forwardPoseDelta[11] *= transStr;
	}

	// Locomotion correction: handled by MV path (locoMV = totalMV - headMV) in shader.
	// The game's per-pixel MVs already contain depth-dependent locomotion parallax.
	// poseDeltaMatrix only handles head rotation/translation from OpenXR poses.
	// Previous approach of injecting actorPos delta into poseDeltaMatrix failed because
	// a uniform translation cannot capture depth-dependent parallax correctly through
	// the unproject→transform→reproject pipeline when coordinate frames don't match.

	// staticBlendFactor: 1.0 when near-stationary, 0.0 when moving.
	// Computed AFTER all matrix modifications (stick rotation, translation scaling)
	// so forwardPoseDelta reflects the final total transform including stick yaw.
	{
		float headTrans = sqrtf(
			cb.forwardPoseDelta[3]  * cb.forwardPoseDelta[3] +
			cb.forwardPoseDelta[7]  * cb.forwardPoseDelta[7] +
			cb.forwardPoseDelta[11] * cb.forwardPoseDelta[11]);
		// Rotation angle from final 3x3: trace = 1 + 2*cos(θ)
		float trace = cb.forwardPoseDelta[0] + cb.forwardPoseDelta[5] + cb.forwardPoseDelta[10];
		float cosAngle = std::min(1.0f, std::max(-1.0f, (trace - 1.0f) * 0.5f));
		float headRot = acosf(cosAngle); // radians
		float locoMotion = sqrtf(m_locoTransX * m_locoTransX +
			m_locoTransY * m_locoTransY + m_locoTransZ * m_locoTransZ);
		// Include stick yaw directly — may not be in forwardPoseDelta if rotS==0 or dead zone
		float stickYaw = fabsf(m_locoYaw);
		float totalMotion = headTrans + headRot + locoMotion + stickYaw;
		// Ramp: 1.0 below 0.001, 0.0 above 0.005
		cb.staticBlendFactor = 1.0f - std::min(1.0f, std::max(0.0f, (totalMotion - 0.001f) / 0.004f));
	}

	// clipToClipNoLoco: prevVP * inv(curVP_original) — head rotation + head translation,
	// no locomotion injection. Per-eye from RSS VP matrices.
	if (m_slotHasClipToClipNoLoco[slot] && eye >= 0 && eye < 2) {
		memcpy(cb.clipToClipNoLoco, m_slotClipToClipNoLoco[slot][eye], sizeof(cb.clipToClipNoLoco));
		cb.hasClipToClipNoLoco = 1;
	} else {
		// Identity fallback — shader will use headRotMatrix path
		for (int i = 0; i < 16; i++)
			cb.clipToClipNoLoco[i] = 0;
		cb.clipToClipNoLoco[0] = cb.clipToClipNoLoco[5] = cb.clipToClipNoLoco[10] = cb.clipToClipNoLoco[15] = 1.0f;
		cb.hasClipToClipNoLoco = 0;
	}

	cb.resolution[0] = (float)m_eyeWidth;
	cb.resolution[1] = (float)m_eyeHeight;
	cb.nearZ = m_slotNear[slot];
	cb.farZ = m_slotFar[slot];
	cb.fovTanLeft = tanf(m_slotFov[slot][eye].angleLeft);
	cb.fovTanRight = tanf(m_slotFov[slot][eye].angleRight);
	cb.fovTanUp = tanf(m_slotFov[slot][eye].angleUp);
	cb.fovTanDown = tanf(m_slotFov[slot][eye].angleDown);
	float ds = oovr_global_configuration.ASWDepthScale();
	cb.depthScale = (ds < 0.0f) ? 0.0f : ds;
	cb.edgeFadeWidth = oovr_global_configuration.ASWEdgeFadeWidth();
	cb.nearFadeDepth = oovr_global_configuration.ASWNearFadeDepth() * 72.0f;
	{
		// MV correction: locoMV = totalMV - headMV isolates per-pixel locomotion
		// from the game's MV buffer. headMV comes from clipToClipNoLoco (RSS VP).
		// poseDeltaMatrix handles head rotation/translation only (no loco injection).
		cb.mvConfidence = oovr_global_configuration.ASWMVConfidence();
	}
	cb.mvPixelScale = oovr_global_configuration.ASWMVPixelScale();
	cb.depthResolution[0] = (m_slotDepthDataW[slot] > 0) ? (float)m_slotDepthDataW[slot] : (float)m_eyeWidth;
	cb.depthResolution[1] = (m_slotDepthDataH[slot] > 0) ? (float)m_slotDepthDataH[slot] : (float)m_eyeHeight;
	cb.mvResolution[0] = (m_slotMVDataW[slot] > 0) ? (float)m_slotMVDataW[slot] : (float)m_eyeWidth;
	cb.mvResolution[1] = (m_slotMVDataH[slot] > 0) ? (float)m_slotMVDataH[slot] : (float)m_eyeHeight;

	if (eye == 0) {
		static int s_diagCount = 0;
		if (s_diagCount < 120 || (s_diagCount % 300 == 0)) {
			OOVR_LOGF("ASW warp [%d]: slot=%d fid=%llu locoTrans(%.4f, %.4f, %.4f) mvConf=%.3f timingR=%.3f c2c=%d d3d12=%d",
			    s_diagCount, slot, m_slotFrameId[slot],
			    m_locoTransX, m_locoTransY, m_locoTransZ, cb.mvConfidence, m_mvTimingRatio, cb.hasClipToClipNoLoco, m_d3d12Ready ? 1 : 0);
		}
		s_diagCount++;
	}

	if (eye == 0) {
		static int s_fovLogCount = 0;
		if (s_fovLogCount < 5) {
			OOVR_LOGF("ASW fov: tanL=%.4f tanR=%.4f tanU=%.4f tanD=%.4f near=%.2f far=%.1f depthScale=%.3f",
			    cb.fovTanLeft, cb.fovTanRight, cb.fovTanUp, cb.fovTanDown, cb.nearZ, cb.farZ, cb.depthScale);
			OOVR_LOGF("ASW poses: cached=(%.3f,%.3f,%.3f) new=(%.3f,%.3f,%.3f)",
			    m_slotPose[slot][eye].position.x, m_slotPose[slot][eye].position.y, m_slotPose[slot][eye].position.z,
			    newPose.position.x, newPose.position.y, newPose.position.z);
			s_fovLogCount++;
		}
	}

	// ── MV DIAGNOSTIC READBACK ──
	// Read back center pixel of cached MV texture and compare to c2c/pdm predictions.
	// Logs actual numeric values for both eyes every 60 frames.
	{
		static int s_mvDiagCount[2] = { 0, 0 };
		int eyeIdx = (eye >= 0 && eye < 2) ? eye : 0;
		s_mvDiagCount[eyeIdx]++;
		int fc = s_mvDiagCount[eyeIdx];
		bool doMvDiag = (fc > 15 && fc < 80 && (fc % 5 == 0))
		             || (fc % 150 == 0);
		if (doMvDiag && m_cachedMV[slot][eye] && cb.hasClipToClipNoLoco) {
			ID3D11DeviceContext* diagCtx = nullptr;
			m_device->GetImmediateContext(&diagCtx);
			if (diagCtx) {
				// Create staging texture on first use
				static ID3D11Texture2D* s_mvStaging = nullptr;
				if (!s_mvStaging) {
					D3D11_TEXTURE2D_DESC mvDesc;
					m_cachedMV[slot][eye]->GetDesc(&mvDesc);
					D3D11_TEXTURE2D_DESC stgDesc = {};
					stgDesc.Width = 1;
					stgDesc.Height = 1;
					stgDesc.MipLevels = 1;
					stgDesc.ArraySize = 1;
					stgDesc.Format = mvDesc.Format;
					stgDesc.SampleDesc.Count = 1;
					stgDesc.Usage = D3D11_USAGE_STAGING;
					stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					m_device->CreateTexture2D(&stgDesc, nullptr, &s_mvStaging);
				}
				if (s_mvStaging) {
					// Copy center pixel
					uint32_t mvW = (m_slotMVDataW[slot] > 0) ? m_slotMVDataW[slot] : m_eyeWidth;
					uint32_t mvH = (m_slotMVDataH[slot] > 0) ? m_slotMVDataH[slot] : m_eyeHeight;
					D3D11_BOX box = {};
					box.left = mvW / 2;
					box.right = box.left + 1;
					box.top = mvH / 2;
					box.bottom = box.top + 1;
					box.front = 0;
					box.back = 1;
					diagCtx->CopySubresourceRegion(s_mvStaging, 0, 0, 0, 0,
					    m_cachedMV[slot][eye], 0, &box);

					D3D11_MAPPED_SUBRESOURCE mapped;
					if (SUCCEEDED(diagCtx->Map(s_mvStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
						float mvX = 0, mvY = 0;
						// MV textures are typically R16G16_FLOAT or R32G32_FLOAT
						D3D11_TEXTURE2D_DESC mvDesc;
						m_cachedMV[slot][eye]->GetDesc(&mvDesc);
						if (mvDesc.Format == DXGI_FORMAT_R16G16_FLOAT) {
							uint16_t* px = (uint16_t*)mapped.pData;
							// Convert half-float to float (approximate)
							auto halfToFloat = [](uint16_t h) -> float {
								uint32_t sign = (h >> 15) & 1;
								uint32_t exp = (h >> 10) & 0x1F;
								uint32_t mant = h & 0x3FF;
								if (exp == 0) return sign ? -0.0f : 0.0f;
								if (exp == 31) return sign ? -1e30f : 1e30f;
								float f = ldexpf((float)(mant | 0x400) / 1024.0f, (int)exp - 15);
								return sign ? -f : f;
							};
							mvX = halfToFloat(px[0]);
							mvY = halfToFloat(px[1]);
						} else {
							float* px = (float*)mapped.pData;
							mvX = px[0];
							mvY = px[1];
						}
						diagCtx->Unmap(s_mvStaging, 0);

						// Compute c2c prediction at center pixel (uv = 0.5, 0.5)
						// clipToClipNoLoco is in cb already
						float ndc[2] = { 0.0f, 0.0f }; // center = uv(0.5,0.5) → ndc(0,0)
						// We need depth at center — use 0.5 as a reasonable approximation
						// (midrange depth in clip space)
						float clipD = 0.9f; // typical scene depth in clip space
						float c2c[16];
						memcpy(c2c, cb.clipToClipNoLoco, sizeof(c2c));
						// clipPos = (0, 0, clipD, 1) → prevClip = c2c * clipPos
						// Column-major: M[col*4+row], so result[row] = sum_col M[col*4+row]*v[col]
						// For v=(0,0,clipD,1): result[row] = c2c[8+row]*clipD + c2c[12+row]
						float prevClip[4];
						for (int i = 0; i < 4; i++)
							prevClip[i] = c2c[8 + i] * clipD + c2c[12 + i];
						float c2cMVx = 0, c2cMVy = 0;
						if (fabsf(prevClip[3]) > 0.0001f) {
							float prevNDCx = prevClip[0] / prevClip[3];
							float prevNDCy = prevClip[1] / prevClip[3];
							c2cMVx = prevNDCx * 0.5f; // prevUV.x - 0.5
							c2cMVy = -prevNDCy * 0.5f; // prevUV.y - 0.5
						}

						// Compute headOnlyMV from headRotMatrix at center pixel
						float ds = cb.depthScale;
						float tanXc = (cb.fovTanLeft + cb.fovTanRight) * 0.5f;
						float tanYc = (cb.fovTanUp + cb.fovTanDown) * 0.5f;
						float linD = cb.nearZ * cb.farZ / (cb.farZ - clipD * (cb.farZ - cb.nearZ));
						float sd = linD * ds;
						float vp[4] = { tanXc * sd, tanYc * sd, sd, 1.0f };

						// headRotMatrix: cur→prev full head delta (row_major in CB).
						// Row-major multiply: hp[row] = sum_col H[row*4+col] * vp[col]
						float* H = cb.headRotMatrix;
						float hp[4];
						for (int i = 0; i < 4; i++)
							hp[i] = H[i * 4 + 0] * vp[0] + H[i * 4 + 1] * vp[1] + H[i * 4 + 2] * vp[2] + H[i * 4 + 3] * vp[3];
						float hrmMVx = 0, hrmMVy = 0;
						if (sd > 0.001f && hp[2] > 0.001f) {
							float oldTanX = hp[0] / hp[2];
							float oldTanY = hp[1] / hp[2];
							hrmMVx = (oldTanX - cb.fovTanLeft) / (cb.fovTanRight - cb.fovTanLeft) - 0.5f;
							hrmMVy = (oldTanY - cb.fovTanUp) / (cb.fovTanDown - cb.fovTanUp) - 0.5f;
						}

						OOVR_LOGF("ASW MV_DIAG eye=%d frame=%d: rawMV(%.6f,%.6f) c2cMV(%.6f,%.6f) hrmMV(%.6f,%.6f) "
						          "c2cRes(%.6f,%.6f) hrmRes(%.6f,%.6f)",
						    eye, fc,
						    mvX, mvY,
						    c2cMVx, c2cMVy,
						    hrmMVx, hrmMVy,
						    mvX - c2cMVx, mvY - c2cMVy,
						    mvX - hrmMVx, mvY - hrmMVy);
					}
				}
				diagCtx->Release();
			}
		}
	}

	// ── Diagnostic capture (inputs) — runs before D3D12 or D3D11 dispatch ──
	{
		ID3D11DeviceContext* captureCtx = nullptr;
		m_device->GetImmediateContext(&captureCtx);
		if (captureCtx) {
			CaptureWarpDiagnostics(eye, slot, cb, captureCtx, true);
			captureCtx->Release();
		}
	}

	// ── D3D12 warp path ──
	if (m_d3d12Ready) {
		// Write constants to per-eye upload heap region (eye 0 at offset 0, eye 1 at offset stride)
		void* eyeCbPtr = (uint8_t*)m_d3d12CbMappedPtr + eye * m_d3d12CbEyeStride;
		memcpy(eyeCbPtr, &cb, sizeof(cb));

		if (eye == 0) {
			// Cache fence: if WaitForCacheFence() was called early (before
			// xrWaitFrame), skip the wait here — data is already verified ready.
			if (!m_cacheFenceWaitedEarly) {
				uint64_t cacheVal = m_slotCacheFenceValue[slot];
				if (cacheVal > 0 && m_d3d12Fence->GetCompletedValue() < cacheVal) {
					m_d3d12Fence->SetEventOnCompletion(cacheVal, m_fenceEvent);
					WaitForSingleObject(m_fenceEvent, 5000);
				}
			}
			m_cacheFenceWaitedEarly = false;

			// Reset command list once per frame (before first eye)
			m_d3d12CmdAlloc->Reset();
			m_d3d12CmdList->Reset(m_d3d12CmdAlloc, m_d3d12PipelineState);
		}

		// Bind compute root signature + per-eye constant buffer + descriptor heaps
		m_d3d12CmdList->SetComputeRootSignature(m_d3d12RootSig);
		m_d3d12CmdList->SetComputeRootConstantBufferView(0,
		    m_d3d12ConstantBuffer->GetGPUVirtualAddress() + eye * m_d3d12CbEyeStride);

		ID3D12DescriptorHeap* heaps[] = { m_d3d12SrvUavHeap };
		m_d3d12CmdList->SetDescriptorHeaps(1, heaps);
		m_d3d12CmdList->SetComputeRootDescriptorTable(1, GetSrvGpuHandle(slot, eye));
		m_d3d12CmdList->SetComputeRootDescriptorTable(2, GetUavGpuHandle(eye));

		uint32_t groupsX = (m_eyeWidth + 7) / 8;
		uint32_t groupsY = (m_eyeHeight + 7) / 8;

		// Two-pass forward scatter pipeline: depth-first eliminates color race conditions.
		if (m_d3d12ClearPSO && m_d3d12ForwardDepthPSO && m_d3d12ForwardColorPSO && m_d3d12DilatePSO) {
			D3D12_RESOURCE_BARRIER uavBarrier = {};
			uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uavBarrier.UAV.pResource = nullptr; // full UAV barrier (all resources)

			// Pass 0: clear output + atomic depth
			m_d3d12CmdList->SetPipelineState(m_d3d12ClearPSO);
			m_d3d12CmdList->Dispatch(groupsX, groupsY, 1);
			m_d3d12CmdList->ResourceBarrier(1, &uavBarrier);

			// Pass 1: forward scatter depth only (finalize atomicDepth)
			m_d3d12CmdList->SetPipelineState(m_d3d12ForwardDepthPSO);
			m_d3d12CmdList->Dispatch(groupsX, groupsY, 1);
			m_d3d12CmdList->ResourceBarrier(1, &uavBarrier);

			// Pass 2: forward scatter color (write where depth matches)
			m_d3d12CmdList->SetPipelineState(m_d3d12ForwardColorPSO);
			m_d3d12CmdList->Dispatch(groupsX, groupsY, 1);
			m_d3d12CmdList->ResourceBarrier(1, &uavBarrier);

			// Pass 3: dilate (mirror-fill disocclusion + depth-edge cleanup)
			m_d3d12CmdList->SetPipelineState(m_d3d12DilatePSO);
			m_d3d12CmdList->Dispatch(groupsX, groupsY, 1);
		} else {
			// Fallback: backward warp
			m_d3d12CmdList->SetPipelineState(m_d3d12PipelineState);
			m_d3d12CmdList->Dispatch(groupsX, groupsY, 1);
		}

		return true;
	}

	// ── D3D11 fallback path ──
	// Get immediate context from device (WarpFrame no longer takes ctx param)
	ID3D11DeviceContext* ctx = nullptr;
	m_device->GetImmediateContext(&ctx);
	if (!ctx)
		return false;

	if (oovr_global_configuration.ASWDebugMode() == 4) {
		ctx->CopyResource(m_warpedOutput[eye], m_cachedColor[slot][eye]);
		ctx->Release();
		{
			static int s = 0;
			if (s++ < 5)
				OOVR_LOGF("ASW BYPASS[%d]: copied cached->output (no warp), fid=%llu", eye, m_slotFrameId[slot]);
		}
		return true;
	}

	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		ctx->Release();
		return false;
	}
	memcpy(mapped.pData, &cb, sizeof(cb));
	ctx->Unmap(m_constantBuffer, 0);

	ID3D11ShaderResourceView* mvSRV = (fabsf(cb.mvConfidence) > 0.001f) ? m_srvMV[slot][eye] : nullptr;
	ID3D11ShaderResourceView* srvs[] = { m_srvColor[slot][eye], mvSRV, m_srvDepth[slot][eye] };
	ctx->CSSetShaderResources(0, 3, srvs);
	ID3D11UnorderedAccessView* uavs2[] = { m_uavOutput[eye], m_uavAtomicDepth[eye] };
	ctx->CSSetUnorderedAccessViews(0, 2, uavs2, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &m_constantBuffer);
	ctx->CSSetSamplers(0, 1, &m_linearSampler);

	uint32_t groupsX = (m_eyeWidth + 7) / 8;
	uint32_t groupsY = (m_eyeHeight + 7) / 8;

	// Forward scatter pipeline: each SOURCE pixel projects to its destination.
	// Depth test via InterlockedMin ensures closest pixel wins (tree over sky).
	// This naturally handles locomotion disocclusion without expensive searches.
	if (m_clearCS && m_forwardDepthCS && m_forwardColorCS && m_dilateCS) {
		{
			static int s_pathLog = 0;
			if (s_pathLog++ < 5)
				OOVR_LOGF("ASW D3D11: Two-pass forward scatter (CSClear+CSForwardDepth+CSForwardColor+CSDilate) eye=%d slot=%d", eye, slot);
		}
		// Pass 0: clear output + atomic depth
		ctx->CSSetShader(m_clearCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);

		// Pass 1: forward scatter depth only (finalize atomicDepth via InterlockedMin)
		ctx->CSSetShader(m_forwardDepthCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);

		// Pass 2: forward scatter color (write color only where depth matches finalized buffer)
		ctx->CSSetShader(m_forwardColorCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);

		// Pass 3: dilate to fill disocclusion gaps (mirror-fill) + depth-edge cleanup
		ctx->CSSetShader(m_dilateCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);
	} else {
		{
			static int s_pathLog2 = 0;
			if (s_pathLog2++ < 5)
				OOVR_LOGF("ASW D3D11: Backward warp path (CSMain) eye=%d", eye);
		}
		ctx->CSSetShader(m_warpCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);
	}

	ID3D11ShaderResourceView* nullSRVs[3] = {};
	ID3D11UnorderedAccessView* nullUAVs[2] = {};
	ctx->CSSetShaderResources(0, 3, nullSRVs);
	ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);

	ctx->Release();
	return true;
}

bool ASWProvider::FlushWarpCommandList()
{
	if (!m_d3d12Ready)
		return false;

	// Close and execute D3D12 command list (warp compute dispatches from WarpFrame)
	HRESULT hr = m_d3d12CmdList->Close();
	if (FAILED(hr)) {
		static int s = 0;
		if (s++ < 5)
			OOVR_LOGF("ASW FlushWarp: Close failed hr=0x%08X", (unsigned)hr);
		m_precomputedWarpReady = false;
		return false;
	}
	ID3D12CommandList* lists[] = { m_d3d12CmdList };
	m_d3d12CmdQueue->ExecuteCommandLists(1, lists);

	// Signal fence after warp dispatches — CopyWarpedOutputToSwapchain must wait
	// before resetting the command allocator (D3D12 requires GPU to be done first).
	uint64_t val = ++m_fenceValue;
	m_d3d12CmdQueue->Signal(m_d3d12Fence, val);
	m_warpFenceValue = val;
	m_precomputedWarpReady = true;
	m_warpFenceWaitPending = true;
	m_warpReadSlot.store(-1, std::memory_order_release);
	return true;
}

bool ASWProvider::WaitForWarpCompletion()
{
	if (!m_precomputedWarpReady)
		return false;
	if (m_warpFenceWaitPending && m_d3d12Fence && m_fenceEvent) {
		if (m_d3d12Fence->GetCompletedValue() < m_warpFenceValue) {
			m_d3d12Fence->SetEventOnCompletion(m_warpFenceValue, m_fenceEvent);
			WaitForSingleObject(m_fenceEvent, 50);
		}
		m_warpFenceWaitPending = false;
	}
	return m_precomputedWarpReady;
}

// ============================================================================
// D3D12 game staging copy — bypasses D3D11 GPU queue contention
// ============================================================================

void ASWProvider::SignalGameStagingDone(ID3D11DeviceContext* ctx)
{
	if (!m_d3d11Fence || !ctx)
		return;
	ID3D11DeviceContext4* ctx4 = nullptr;
	ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
	if (ctx4) {
		uint64_t val = ++m_fenceValue;
		ctx4->Signal(m_d3d11Fence, val);
		m_stagingFenceValue.store(val, std::memory_order_release);
		ctx4->Release();
	}
}

ID3D12Resource* ASWProvider::GetOrShareTextureD3D12(ID3D11Texture2D* d3d11Tex)
{
	if (!d3d11Tex || !m_d3d12Device)
		return nullptr;

	// Check cache first
	for (uint32_t i = 0; i < m_gameSharedTexCount; i++) {
		if (m_gameSharedTexCache[i].d3d11Tex == d3d11Tex)
			return m_gameSharedTexCache[i].d3d12Res;
	}

	if (m_gameSharedTexCount >= kMaxGameSharedTextures) {
		OOVR_LOG("ASW D3D12 gameCopy: shared texture cache full");
		return nullptr;
	}

	// Try NT handle sharing (IDXGIResource1::CreateSharedHandle)
	HANDLE handle = nullptr;
	ID3D12Resource* d3d12Res = nullptr;

	IDXGIResource1* dxgiRes1 = nullptr;
	HRESULT hr = d3d11Tex->QueryInterface(IID_PPV_ARGS(&dxgiRes1));
	if (SUCCEEDED(hr)) {
		hr = dxgiRes1->CreateSharedHandle(nullptr,
		    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
		dxgiRes1->Release();
		if (SUCCEEDED(hr) && handle) {
			hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(&d3d12Res));
			CloseHandle(handle);
			if (SUCCEEDED(hr) && d3d12Res) {
				m_gameSharedTexCache[m_gameSharedTexCount].d3d11Tex = d3d11Tex;
				m_gameSharedTexCache[m_gameSharedTexCount].d3d12Res = d3d12Res;
				m_gameSharedTexCount++;
				return d3d12Res;
			}
		}
	}

	// Try legacy sharing (IDXGIResource::GetSharedHandle)
	IDXGIResource* dxgiRes = nullptr;
	hr = d3d11Tex->QueryInterface(IID_PPV_ARGS(&dxgiRes));
	if (SUCCEEDED(hr)) {
		handle = nullptr;
		hr = dxgiRes->GetSharedHandle(&handle);
		dxgiRes->Release();
		if (SUCCEEDED(hr) && handle) {
			hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(&d3d12Res));
			if (SUCCEEDED(hr) && d3d12Res) {
				m_gameSharedTexCache[m_gameSharedTexCount].d3d11Tex = d3d11Tex;
				m_gameSharedTexCache[m_gameSharedTexCount].d3d12Res = d3d12Res;
				m_gameSharedTexCount++;
				return d3d12Res;
			}
		}
	}

	OOVR_LOGF("ASW D3D12 gameCopy: failed to share D3D11 texture %p to D3D12", d3d11Tex);
	return nullptr;
}

bool ASWProvider::CopyGameStagingToSwapchainD3D12(
    ID3D11DeviceContext* ctx,
    int count,
    ID3D11Texture2D** stagingTexs,
    ID3D11Texture2D** swapchainImages)
{
	if (!m_d3d12Ready || !m_d3d12GameCopyCmdAlloc || !m_d3d12GameCopyCmdList || count <= 0)
		return false;

	// Wait for previous game copy fence if pending (deferred from last cycle)
	if (m_gameCopyFenceWaitPending) {
		WaitForSingleObject(m_gameCopyFenceEvent, 5000);
		m_gameCopyFenceWaitPending = false;
	}

	// Share all textures to D3D12 (lazy, cached)
	ID3D12Resource* d3d12Staging[4] = {};
	ID3D12Resource* d3d12Swap[4] = {};
	int validCount = (count > 4) ? 4 : count;
	for (int i = 0; i < validCount; i++) {
		d3d12Staging[i] = GetOrShareTextureD3D12(stagingTexs[i]);
		d3d12Swap[i] = GetOrShareTextureD3D12(swapchainImages[i]);
		if (!d3d12Staging[i] || !d3d12Swap[i])
			return false;
	}

	// Skip staging fence wait — read previous frame's staging data (GPU-complete
	// from previous cycle's Flush). Build 12 proved that the compositor can't
	// show the game frame when game draws are pending on the D3D11 GPU queue.
	// By skipping the fence wait, the D3D12 copy runs immediately with
	// GPU-complete (stale) data, giving the compositor a clean swapchain image.

	// Record all copy commands in a single command list (both eyes batched)
	m_d3d12GameCopyCmdAlloc->Reset();
	m_d3d12GameCopyCmdList->Reset(m_d3d12GameCopyCmdAlloc, nullptr);

	for (int i = 0; i < validCount; i++) {
		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = d3d12Staging[i];
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = d3d12Swap[i];
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;

		m_d3d12GameCopyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	m_d3d12GameCopyCmdList->Close();

	// Execute on D3D12 queue (independent of D3D11 game draws)
	ID3D12CommandList* lists[] = { m_d3d12GameCopyCmdList };
	m_d3d12CmdQueue->ExecuteCommandLists(1, lists);

	// Signal fence and CPU-wait for D3D12 copy completion.
	uint64_t fenceVal = ++m_fenceValue;
	m_d3d12CmdQueue->Signal(m_d3d12Fence, fenceVal);
	m_d3d12Fence->SetEventOnCompletion(fenceVal, m_gameCopyFenceEvent);
	WaitForSingleObject(m_gameCopyFenceEvent, 5000);
	m_gameCopyFenceWaitPending = false;

	// ── D3D11↔D3D12 GPU sync: runtime reads swapchain via D3D11 ──
	// D3D12 wrote to the shared swapchain resource, but D3D11 (used by the
	// runtime's compositor) has no visibility of D3D12 writes without a fence
	// wait. Insert D3D11 Wait so the GPU processes D3D12 writes before any
	// subsequent D3D11 commands (e.g. xrReleaseSwapchainImage).
	// NOTE: This runs at Step G2 when the D3D11 GPU queue is CLEAN (game was
	// signaled at prev cycle's S, ~24ms ago — all draws processed by now).
	if (m_d3d11Fence) {
		ID3D11DeviceContext4* ctx4 = nullptr;
		ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
		if (ctx4) {
			ctx4->Wait(m_d3d11Fence, fenceVal);
			ctx4->Release();
		}
	}

	return true;
}

bool ASWProvider::CopyPrecomputedWarpToSwapchain(ID3D11DeviceContext* ctx, uint32_t* outSwapIdx)
{
	if (!m_ready)
		return false;

	// EXPERIMENT: no-fence test — skip warp fence wait entirely.
	if (!m_precomputedWarpReady)
		return false;

	// 1. Acquire output swapchain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
	if (XR_FAILED(res)) {
		static int s = 0;
		if (s++ < 5)
			OOVR_LOGF("ASW PrecompCopy: acquire failed result=%d", (int)res);
		return false;
	}
	if (outSwapIdx)
		*outSwapIdx = idx;

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW PrecompCopy: wait failed result=%d", (int)res);
		return false;
	}

	if (idx >= m_outputSwapchainImages.size()) {
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_outputSwapchain, &rel);
		return false;
	}

	// Debug mode 51: draw cyan center box into warpedOutput BEFORE copy to swapchain.
	// Written via D3D11 (warp compute already done), so the D3D12 copy includes it.
	if (oovr_global_configuration.ASWDebugMode() == 51) {
		for (int eye = 0; eye < 2; eye++) {
			D3D11_TEXTURE2D_DESC desc;
			m_warpedOutput[eye]->GetDesc(&desc);
			UINT boxW = 200, boxH = 200;
			if (boxW > desc.Width)
				boxW = desc.Width;
			if (boxH > desc.Height)
				boxH = desc.Height;
			UINT left = (desc.Width - boxW) / 2;
			UINT top = (desc.Height - boxH) / 2;
			D3D11_BOX box = { left, top, 0, left + boxW, top + boxH, 1 };
			std::vector<uint8_t> pixels(boxW * boxH * 4);
			for (UINT i = 0; i < boxW * boxH; i++) {
				pixels[i * 4 + 0] = 0; // R
				pixels[i * 4 + 1] = 255; // G
				pixels[i * 4 + 2] = 255; // B
				pixels[i * 4 + 3] = 255; // A
			}
			ctx->UpdateSubresource(m_warpedOutput[eye], 0, &box, pixels.data(), boxW * 4, 0);
		}
		// EXPERIMENT: no-fence test — skip debug box fence.
	}

	// 2. Copy warpedOutput → swapchain image
	if (m_d3d12Ready && m_d3d12DirectCopy && idx < kMaxSwapchainImages && m_d3d12SwapchainImages[idx]) {
		// ── D3D12 direct copy: bypasses D3D11 GPU queue entirely ──
		// Reuses warp command allocator/list — WaitForWarpCompletion() already ran,
		// so the allocator is free.
		m_d3d12CmdAlloc->Reset();
		m_d3d12CmdList->Reset(m_d3d12CmdAlloc, nullptr);

		D3D12_TEXTURE_COPY_LOCATION srcLeft = {};
		srcLeft.pResource = m_d3d12WarpedOutput[0];
		srcLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLeft.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION dstLeft = {};
		dstLeft.pResource = m_d3d12SwapchainImages[idx];
		dstLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLeft.SubresourceIndex = 0;
		D3D12_BOX srcBox = { 0, 0, 0, m_eyeWidth, m_eyeHeight, 1 };
		m_d3d12CmdList->CopyTextureRegion(&dstLeft, 0, 0, 0, &srcLeft, &srcBox);

		D3D12_TEXTURE_COPY_LOCATION srcRight = {};
		srcRight.pResource = m_d3d12WarpedOutput[1];
		srcRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcRight.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION dstRight = {};
		dstRight.pResource = m_d3d12SwapchainImages[idx];
		dstRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstRight.SubresourceIndex = 0;
		m_d3d12CmdList->CopyTextureRegion(&dstRight, (UINT)m_eyeWidth, 0, 0, &srcRight, &srcBox);

		m_d3d12CmdList->Close();
		ID3D12CommandList* lists[] = { m_d3d12CmdList };
		m_d3d12CmdQueue->ExecuteCommandLists(1, lists);
		// EXPERIMENT: no-fence test — skip fence signal + CPU-wait before xrRelease.
		// D3D12 copy submitted but we don't wait for GPU completion.
	} else {
		// ── D3D11 fallback ──
		// EXPERIMENT: no-fence test — skip D3D11 fence wait.
		ID3D11Texture2D* target = m_outputSwapchainImages[idx];
		ctx->CopySubresourceRegion(target, 0,
		    0, 0, 0, m_warpedOutput[0], 0, nullptr);
		ctx->CopySubresourceRegion(target, 0,
		    m_eyeWidth, 0, 0, m_warpedOutput[1], 0, nullptr);
		ctx->Flush();
	}

	// 4. Release swapchain
	XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

	// 5. Depth swapchain (D3D11 — depth is small, not contention-sensitive)
	int slot = GetActiveCacheSlot();
	if (m_depthSwapchain != XR_NULL_HANDLE && slot >= 0) {
		XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t depthIdx = 0;
		XrResult depthRes = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
		if (XR_SUCCEEDED(depthRes)) {
			XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			depthWait.timeout = XR_INFINITE_DURATION;
			depthRes = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
			if (XR_SUCCEEDED(depthRes)) {
				if (depthIdx < m_depthSwapchainImages.size()) {
					ID3D11Texture2D* depthTarget = m_depthSwapchainImages[depthIdx];
					D3D11_BOX depthBox = {};
					depthBox.right = m_eyeWidth;
					depthBox.bottom = m_eyeHeight;
					depthBox.front = 0;
					depthBox.back = 1;
					ctx->CopySubresourceRegion(depthTarget, 0,
					    0, 0, 0, m_cachedDepth[slot][0], 0, &depthBox);
					ctx->CopySubresourceRegion(depthTarget, 0,
					    m_eyeWidth, 0, 0, m_cachedDepth[slot][1], 0, &depthBox);
				}
				XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
			}
		}
	}

	m_precomputedWarpReady = false; // consumed
	return true;
}

uint64_t ASWProvider::FlushWarpComputeAsync()
{
	if (!m_d3d12Ready)
		return 0;
	m_d3d12CmdList->Close();
	ID3D12CommandList* lists[] = { m_d3d12CmdList };
	m_d3d12CmdQueue->ExecuteCommandLists(1, lists);
	uint64_t val = ++m_fenceValue;
	m_d3d12CmdQueue->Signal(m_d3d12Fence, val);
	return val;
}

bool ASWProvider::FinishWarpCopy(uint64_t computeFenceVal)
{
	if (!m_d3d12Ready || !m_d3d12DirectCopy)
		return false;

	auto tStart = std::chrono::high_resolution_clock::now();

	// 1. CPU-wait for compute fence (should be done — GPU ran during xrWaitFrame)
	float computeWaitMs = 0;
	if (computeFenceVal > 0 && m_d3d12Fence->GetCompletedValue() < computeFenceVal) {
		auto tw0 = std::chrono::high_resolution_clock::now();
		m_d3d12Fence->SetEventOnCompletion(computeFenceVal, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, 5000);
		auto tw1 = std::chrono::high_resolution_clock::now();
		computeWaitMs = std::chrono::duration<float, std::milli>(tw1 - tw0).count();
	}

	// 2. Acquire swapchain image
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
	if (XR_FAILED(res)) {
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
	if (XR_FAILED(res)) {
		m_ready = false;
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}
	if (idx >= m_outputSwapchainImages.size() || !m_d3d12SwapchainImages[idx]) {
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_outputSwapchain, &rel);
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}

	// 3. Reset allocator (GPU done with compute) and record copy commands
	m_d3d12CmdAlloc->Reset();
	m_d3d12CmdList->Reset(m_d3d12CmdAlloc, nullptr);

	D3D12_BOX srcBox = { 0, 0, 0, m_eyeWidth, m_eyeHeight, 1 };

	D3D12_TEXTURE_COPY_LOCATION srcLeft = {};
	srcLeft.pResource = m_d3d12WarpedOutput[0];
	srcLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION dstLeft = {};
	dstLeft.pResource = m_d3d12SwapchainImages[idx];
	dstLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	m_d3d12CmdList->CopyTextureRegion(&dstLeft, 0, 0, 0, &srcLeft, &srcBox);

	D3D12_TEXTURE_COPY_LOCATION srcRight = {};
	srcRight.pResource = m_d3d12WarpedOutput[1];
	srcRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION dstRight = {};
	dstRight.pResource = m_d3d12SwapchainImages[idx];
	dstRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	m_d3d12CmdList->CopyTextureRegion(&dstRight, (UINT)m_eyeWidth, 0, 0, &srcRight, &srcBox);

	// 4. Execute copy + fence wait
	m_d3d12CmdList->Close();
	ID3D12CommandList* lists[] = { m_d3d12CmdList };
	m_d3d12CmdQueue->ExecuteCommandLists(1, lists);
	uint64_t copyFenceVal = ++m_fenceValue;
	m_d3d12CmdQueue->Signal(m_d3d12Fence, copyFenceVal);
	m_d3d12Fence->SetEventOnCompletion(copyFenceVal, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, 5000);

	auto tDone = std::chrono::high_resolution_clock::now();

	// 5. Release swapchain
	XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

	float totalMs = std::chrono::duration<float, std::milli>(tDone - tStart).count();
	{
		static int s = 0;
		if (s++ < 10 || totalMs > 3.0f || computeWaitMs > 0.5f)
			OOVR_LOGF("ASW FinishWarpCopy: total=%.1fms computeWait=%.1fms copy=%.1fms idx=%u",
			    totalMs, computeWaitMs, totalMs - computeWaitMs, idx);
	}

	// 6. Handle depth swapchain
	if (m_depthSwapchain != XR_NULL_HANDLE) {
		XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t depthIdx = 0;
		res = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
		if (XR_SUCCEEDED(res)) {
			XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			depthWait.timeout = XR_INFINITE_DURATION;
			res = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
			if (XR_SUCCEEDED(res)) {
				XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
			}
		}
	}

	m_warpReadSlot.store(-1, std::memory_order_release);
	return true;
}

bool ASWProvider::SubmitWarpedOutput(ID3D11DeviceContext* ctx)
{
	if (!m_ready || !HasCachedFrame()) {
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}
	int slot = m_warpReadSlot.load(std::memory_order_acquire);
	if (slot < 0)
		slot = m_publishedSlot.load(std::memory_order_acquire);
	if (slot < 0) {
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}

	auto tStart = std::chrono::high_resolution_clock::now();

	// ── D3D12 path ──
	if (m_d3d12Ready) {
		// 1. Acquire OpenXR swapchain FIRST (need idx for D3D12 direct copy)
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
		if (XR_FAILED(res)) {
			static int s = 0;
			if (s++ < 5)
				OOVR_LOGF("ASW: Output acquire failed result=%d", (int)res);
			m_warpReadSlot.store(-1, std::memory_order_release);
			return false;
		}

		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
		if (XR_FAILED(res)) {
			OOVR_LOGF("ASW: Output wait FAILED result=%d — disabling ASW (swapchain stuck)", (int)res);
			m_ready = false;
			m_warpReadSlot.store(-1, std::memory_order_release);
			return false;
		}

		if (idx >= m_outputSwapchainImages.size()) {
			static int s = 0;
			if (s++ < 5)
				OOVR_LOGF("ASW: Acquired idx %u out of range (have %zu)", idx, m_outputSwapchainImages.size());
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(m_outputSwapchain, &rel);
			m_warpReadSlot.store(-1, std::memory_order_release);
			return false;
		}

		// 2. Add D3D12 copy commands to the command list (warp output → swapchain image)
		// This runs entirely on the D3D12 queue, independent of D3D11 game draws.
		if (m_d3d12DirectCopy && idx < kMaxSwapchainImages && m_d3d12SwapchainImages[idx]) {
			// Copy left eye at x=0
			D3D12_TEXTURE_COPY_LOCATION srcLeft = {};
			srcLeft.pResource = m_d3d12WarpedOutput[0];
			srcLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			srcLeft.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dstLeft = {};
			dstLeft.pResource = m_d3d12SwapchainImages[idx];
			dstLeft.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLeft.SubresourceIndex = 0;
			D3D12_BOX srcBox = { 0, 0, 0, m_eyeWidth, m_eyeHeight, 1 };
			m_d3d12CmdList->CopyTextureRegion(&dstLeft, 0, 0, 0, &srcLeft, &srcBox);

			// Copy right eye at x=eyeWidth
			D3D12_TEXTURE_COPY_LOCATION srcRight = {};
			srcRight.pResource = m_d3d12WarpedOutput[1];
			srcRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			srcRight.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dstRight = {};
			dstRight.pResource = m_d3d12SwapchainImages[idx];
			dstRight.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstRight.SubresourceIndex = 0;
			m_d3d12CmdList->CopyTextureRegion(&dstRight, (UINT)m_eyeWidth, 0, 0, &srcRight, &srcBox);
		}

		// 3. Close and execute D3D12 command list (warp compute + copy)
		m_d3d12CmdList->Close();
		ID3D12CommandList* lists[] = { m_d3d12CmdList };
		m_d3d12CmdQueue->ExecuteCommandLists(1, lists);

		// 4. Signal D3D12 fence and CPU-wait for completion
		uint64_t warpFenceVal = ++m_fenceValue;
		m_d3d12CmdQueue->Signal(m_d3d12Fence, warpFenceVal);
		m_d3d12Fence->SetEventOnCompletion(warpFenceVal, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, 5000);

		auto tWarpGPU = std::chrono::high_resolution_clock::now();

		if (m_d3d12DirectCopy && idx < kMaxSwapchainImages && m_d3d12SwapchainImages[idx]) {
			// D3D12 wrote directly to swapchain image. CPU fence wait guarantees
			// the write is complete. No D3D11 copy needed. No D3D11 Flush needed.
			// The WDDM ensures D3D12 writes are visible to the compositor.
		} else if (ctx) {
			// Fallback: D3D11 copy (may be behind game draws)
			ID3D11DeviceContext4* ctx4 = nullptr;
			ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
			if (ctx4) {
				ctx4->Wait(m_d3d11Fence, warpFenceVal);
				ctx4->Release();
			}

			ID3D11Texture2D* target = m_outputSwapchainImages[idx];
			ctx->CopySubresourceRegion(target, 0,
			    0, 0, 0, m_warpedOutput[0], 0, nullptr);
			ctx->CopySubresourceRegion(target, 0,
			    m_eyeWidth, 0, 0, m_warpedOutput[1], 0, nullptr);
			ctx->Flush();
		}

		// Debug mode 51: magenta stripe (still uses D3D11 UpdateSubresource)
		if (ctx && oovr_global_configuration.ASWDebugMode() == 51) {
			ID3D11Texture2D* target = m_outputSwapchainImages[idx];
			D3D11_TEXTURE2D_DESC desc;
			target->GetDesc(&desc);
			UINT stripeH = 64;
			if (stripeH > desc.Height)
				stripeH = desc.Height;
			UINT top = (desc.Height - stripeH) / 2;
			D3D11_BOX box = { 0, top, 0, desc.Width, top + stripeH, 1 };
			std::vector<uint8_t> pixels(desc.Width * stripeH * 4);
			for (UINT i = 0; i < desc.Width * stripeH; i++) {
				pixels[i * 4 + 0] = 255; // R
				pixels[i * 4 + 1] = 0; // G
				pixels[i * 4 + 2] = 255; // B
				pixels[i * 4 + 3] = 255; // A
			}
			ctx->UpdateSubresource(target, 0, &box, pixels.data(), desc.Width * 4, 0);
			ctx->Flush();
		}

		XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

		auto tDone = std::chrono::high_resolution_clock::now();

		// Timing diagnostics
		float warpGpuMs = std::chrono::duration<float, std::milli>(tWarpGPU - tStart).count();
		float totalMs = std::chrono::duration<float, std::milli>(tDone - tStart).count();
		if (totalMs > 5.0f) {
			OOVR_LOGF("ASW WARP TIMING (D3D12%s): total=%.1fms warpGPU=%.1f copy=%.1f idx=%u",
			    m_d3d12DirectCopy ? "+directCopy" : "+d3d11Copy",
			    totalMs, warpGpuMs, totalMs - warpGpuMs, idx);
		}

		// 7. Depth swapchain (requires D3D11 ctx for copy — skip from worker thread)
		if (ctx && m_depthSwapchain != XR_NULL_HANDLE) {
			XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			uint32_t depthIdx = 0;
			XrResult depthRes = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
			if (XR_SUCCEEDED(depthRes)) {
				XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				depthWait.timeout = XR_INFINITE_DURATION;
				depthRes = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
				if (XR_SUCCEEDED(depthRes)) {
					if (depthIdx < m_depthSwapchainImages.size()) {
						ID3D11Texture2D* depthTarget = m_depthSwapchainImages[depthIdx];
						D3D11_BOX depthBox = {};
						depthBox.right = m_eyeWidth;
						depthBox.bottom = m_eyeHeight;
						depthBox.front = 0;
						depthBox.back = 1;
						ctx->CopySubresourceRegion(depthTarget, 0,
						    0, 0, 0, m_cachedDepth[slot][0], 0, &depthBox);
						ctx->CopySubresourceRegion(depthTarget, 0,
						    m_eyeWidth, 0, 0, m_cachedDepth[slot][1], 0, &depthBox);
						// NO Flush() — runtime handles GPU sync
					}
					XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
					xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
				} else {
					OOVR_LOGF("ASW: Depth wait failed result=%d — depth layer disabled", (int)depthRes);
				}
			}
		}

		static int s = 0;
		if (s++ < 3)
			OOVR_LOGF("ASW: Warped output submitted (D3D12%s) warpGPU=%.1fms total=%.1fms",
			    m_d3d12DirectCopy ? " directCopy" : " d3d11Copy",
			    warpGpuMs, totalMs);

		m_warpReadSlot.store(-1, std::memory_order_release);
		return true;
	}

	// ── D3D11 fallback path (original code) ──

	// Acquire output swapchain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
	if (XR_FAILED(res)) {
		static int s = 0;
		if (s++ < 5)
			OOVR_LOGF("ASW: Output acquire failed result=%d", (int)res);
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW: Output wait FAILED result=%d — disabling ASW (swapchain stuck)", (int)res);
		m_ready = false;
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}

	if (idx >= m_outputSwapchainImages.size()) {
		static int s = 0;
		if (s++ < 5)
			OOVR_LOGF("ASW: Acquired idx %u out of range (have %zu)", idx, m_outputSwapchainImages.size());
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_outputSwapchain, &rel);
		m_warpReadSlot.store(-1, std::memory_order_release);
		return false;
	}
	ID3D11Texture2D* target = m_outputSwapchainImages[idx];

	ctx->CopySubresourceRegion(target, 0,
	    0, 0, 0, m_warpedOutput[0], 0, nullptr);
	ctx->CopySubresourceRegion(target, 0,
	    m_eyeWidth, 0, 0, m_warpedOutput[1], 0, nullptr);

	// Debug mode 51: magenta stripe across center of warp frames (D3D11 fallback path)
	if (oovr_global_configuration.ASWDebugMode() == 51) {
		D3D11_TEXTURE2D_DESC desc;
		target->GetDesc(&desc);
		UINT stripeH = 64;
		if (stripeH > desc.Height)
			stripeH = desc.Height;
		UINT top = (desc.Height - stripeH) / 2;
		D3D11_BOX box = { 0, top, 0, desc.Width, top + stripeH, 1 };
		std::vector<uint8_t> pixels(desc.Width * stripeH * 4);
		for (UINT i = 0; i < desc.Width * stripeH; i++) {
			pixels[i * 4 + 0] = 255; // R
			pixels[i * 4 + 1] = 0; // G
			pixels[i * 4 + 2] = 255; // B
			pixels[i * 4 + 3] = 255; // A
		}
		ctx->UpdateSubresource(target, 0, &box, pixels.data(), desc.Width * 4, 0);
	}

	ctx->Flush();

	XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

	// Submit depth swapchain (if available)
	if (m_depthSwapchain != XR_NULL_HANDLE) {
		XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t depthIdx = 0;
		XrResult depthRes = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
		if (XR_SUCCEEDED(depthRes)) {
			XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			depthWait.timeout = XR_INFINITE_DURATION;
			depthRes = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
			if (XR_SUCCEEDED(depthRes)) {
				if (depthIdx < m_depthSwapchainImages.size()) {
					ID3D11Texture2D* depthTarget = m_depthSwapchainImages[depthIdx];
					D3D11_BOX depthBox = {};
					depthBox.right = m_eyeWidth;
					depthBox.bottom = m_eyeHeight;
					depthBox.front = 0;
					depthBox.back = 1;
					ctx->CopySubresourceRegion(depthTarget, 0,
					    0, 0, 0, m_cachedDepth[slot][0], 0, &depthBox);
					ctx->CopySubresourceRegion(depthTarget, 0,
					    m_eyeWidth, 0, 0, m_cachedDepth[slot][1], 0, &depthBox);
					ctx->Flush();
				}
				XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
			} else {
				OOVR_LOGF("ASW: Depth wait failed result=%d — depth layer disabled", (int)depthRes);
			}
		}
	}

	static int s = 0;
	if (s++ < 3)
		OOVR_LOG("ASW: Warped output submitted to swapchain (D3D11 fallback)");

	m_warpReadSlot.store(-1, std::memory_order_release);
	return true;
}

bool ASWProvider::SubmitBlackOutput(ID3D11DeviceContext* ctx)
{
	if (!m_ready || m_outputSwapchain == XR_NULL_HANDLE)
		return false;

	// Acquire output swapchain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
	if (XR_FAILED(res))
		return false;

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
	if (XR_FAILED(res))
		return false;

	if (idx < m_outputSwapchainImages.size()) {
		ID3D11Texture2D* target = m_outputSwapchainImages[idx];
		// Create a temporary RTV to clear the texture to black
		ID3D11RenderTargetView* rtv = nullptr;
		ID3D11Device* dev = nullptr;
		ctx->GetDevice(&dev);
		if (dev) {
			HRESULT hr = dev->CreateRenderTargetView(target, nullptr, &rtv);
			if (SUCCEEDED(hr) && rtv) {
				float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				ctx->ClearRenderTargetView(rtv, black);
				rtv->Release();
			}
			dev->Release();
		}
	}

	XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

	// Also handle depth swapchain if present
	if (m_depthSwapchain != XR_NULL_HANDLE) {
		XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t depthIdx = 0;
		XrResult depthRes = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
		if (XR_SUCCEEDED(depthRes)) {
			XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			depthWait.timeout = XR_INFINITE_DURATION;
			depthRes = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
			if (XR_SUCCEEDED(depthRes)) {
				XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
			}
		}
	}

	return true;
}

// ============================================================================
// Shutdown
// ============================================================================

void ASWProvider::Shutdown()
{
	m_ready = false;
	m_publishedSlot.store(-1, std::memory_order_relaxed);
	m_previousPublishedSlot.store(-1, std::memory_order_relaxed);
	m_warpReadSlot.store(-1, std::memory_order_relaxed);
	m_buildSlot = 0;
	m_buildEyeReady[0] = false;
	m_buildEyeReady[1] = false;
	m_hasLastPublishedPose = false;
	m_frameCounter = 0;

	if (m_depthSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = {};
	}
	m_depthSwapchainImages.clear();

	if (m_outputSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_outputSwapchain);
		m_outputSwapchain = {};
	}
	m_outputSwapchainImages.clear();

	for (int i = 0; i < 2; i++) {
		if (m_uavOutput[i]) {
			m_uavOutput[i]->Release();
			m_uavOutput[i] = nullptr;
		}
		if (m_warpedOutput[i]) {
			m_warpedOutput[i]->Release();
			m_warpedOutput[i] = nullptr;
		}
	}
	for (uint32_t slot = 0; slot < kAswCacheSlotCount; ++slot) {
		for (int eye = 0; eye < 2; ++eye) {
			if (m_srvDepth[slot][eye]) {
				m_srvDepth[slot][eye]->Release();
				m_srvDepth[slot][eye] = nullptr;
			}
			if (m_srvMV[slot][eye]) {
				m_srvMV[slot][eye]->Release();
				m_srvMV[slot][eye] = nullptr;
			}
			if (m_srvColor[slot][eye]) {
				m_srvColor[slot][eye]->Release();
				m_srvColor[slot][eye] = nullptr;
			}
			if (m_cachedDepth[slot][eye]) {
				m_cachedDepth[slot][eye]->Release();
				m_cachedDepth[slot][eye] = nullptr;
			}
			if (m_cachedMV[slot][eye]) {
				m_cachedMV[slot][eye]->Release();
				m_cachedMV[slot][eye] = nullptr;
			}
			if (m_cachedColor[slot][eye]) {
				m_cachedColor[slot][eye]->Release();
				m_cachedColor[slot][eye] = nullptr;
			}
		}
	}

	if (m_linearSampler) {
		m_linearSampler->Release();
		m_linearSampler = nullptr;
	}
	if (m_constantBuffer) {
		m_constantBuffer->Release();
		m_constantBuffer = nullptr;
	}
	if (m_warpCS) {
		m_warpCS->Release();
		m_warpCS = nullptr;
	}
	if (m_clearCS) { m_clearCS->Release(); m_clearCS = nullptr; }
	if (m_forwardCS) { m_forwardCS->Release(); m_forwardCS = nullptr; }
	if (m_forwardDepthCS) { m_forwardDepthCS->Release(); m_forwardDepthCS = nullptr; }
	if (m_forwardColorCS) { m_forwardColorCS->Release(); m_forwardColorCS = nullptr; }
	if (m_dilateCS) { m_dilateCS->Release(); m_dilateCS = nullptr; }

	for (int e = 0; e < 2; ++e) {
		if (m_uavAtomicDepth[e]) { m_uavAtomicDepth[e]->Release(); m_uavAtomicDepth[e] = nullptr; }
		if (m_atomicDepth[e]) { m_atomicDepth[e]->Release(); m_atomicDepth[e] = nullptr; }
	}

	// D3D12 cleanup — drain GPU first
	if (m_d3d12Ready && m_d3d12CmdQueue && m_d3d12Fence && m_fenceEvent) {
		uint64_t drainVal = ++m_fenceValue;
		m_d3d12CmdQueue->Signal(m_d3d12Fence, drainVal);
		m_d3d12Fence->SetEventOnCompletion(drainVal, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, 5000);
	}
	m_d3d12Ready = false;

	// D3D12 shared resources
	for (uint32_t sl = 0; sl < kAswCacheSlotCount; ++sl) {
		for (int e = 0; e < 2; ++e) {
			if (m_d3d12CachedColor[sl][e]) {
				m_d3d12CachedColor[sl][e]->Release();
				m_d3d12CachedColor[sl][e] = nullptr;
			}
			if (m_d3d12CachedMV[sl][e]) {
				m_d3d12CachedMV[sl][e]->Release();
				m_d3d12CachedMV[sl][e] = nullptr;
			}
			if (m_d3d12CachedDepth[sl][e]) {
				m_d3d12CachedDepth[sl][e]->Release();
				m_d3d12CachedDepth[sl][e] = nullptr;
			}
		}
	}
	for (int e = 0; e < 2; ++e) {
		if (m_d3d12WarpedOutput[e]) {
			m_d3d12WarpedOutput[e]->Release();
			m_d3d12WarpedOutput[e] = nullptr;
		}
		if (m_d3d12AtomicDepth[e]) {
			m_d3d12AtomicDepth[e]->Release();
			m_d3d12AtomicDepth[e] = nullptr;
		}
	}
	for (uint32_t i = 0; i < kMaxSwapchainImages; i++) {
		if (m_d3d12SwapchainImages[i]) {
			m_d3d12SwapchainImages[i]->Release();
			m_d3d12SwapchainImages[i] = nullptr;
		}
	}
	m_d3d12DirectCopy = false;

	// D3D12 constant buffer
	if (m_d3d12ConstantBuffer) {
		m_d3d12ConstantBuffer->Unmap(0, nullptr);
		m_d3d12CbMappedPtr = nullptr;
		m_d3d12ConstantBuffer->Release();
		m_d3d12ConstantBuffer = nullptr;
	}

	// D3D12 pipeline
	if (m_d3d12SrvUavHeap) {
		m_d3d12SrvUavHeap->Release();
		m_d3d12SrvUavHeap = nullptr;
	}
	if (m_d3d12PipelineState) { m_d3d12PipelineState->Release(); m_d3d12PipelineState = nullptr; }
	if (m_d3d12ClearPSO) { m_d3d12ClearPSO->Release(); m_d3d12ClearPSO = nullptr; }
	if (m_d3d12ForwardPSO) { m_d3d12ForwardPSO->Release(); m_d3d12ForwardPSO = nullptr; }
	if (m_d3d12ForwardDepthPSO) { m_d3d12ForwardDepthPSO->Release(); m_d3d12ForwardDepthPSO = nullptr; }
	if (m_d3d12ForwardColorPSO) { m_d3d12ForwardColorPSO->Release(); m_d3d12ForwardColorPSO = nullptr; }
	if (m_d3d12DilatePSO) { m_d3d12DilatePSO->Release(); m_d3d12DilatePSO = nullptr; }
	if (m_d3d12RootSig) {
		m_d3d12RootSig->Release();
		m_d3d12RootSig = nullptr;
	}

	// D3D12 fence
	if (m_d3d11Fence) {
		m_d3d11Fence->Release();
		m_d3d11Fence = nullptr;
	}
	if (m_d3d12Fence) {
		m_d3d12Fence->Release();
		m_d3d12Fence = nullptr;
	}
	if (m_fenceSharedHandle) {
		CloseHandle(m_fenceSharedHandle);
		m_fenceSharedHandle = nullptr;
	}
	if (m_fenceEvent) {
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}
	m_fenceValue = 0;
	m_cacheFenceValue.store(0, std::memory_order_relaxed);

	// D3D12 gap fence
	if (m_d3d11GapFence) {
		m_d3d11GapFence->Release();
		m_d3d11GapFence = nullptr;
	}
	if (m_d3d12GapFence) {
		m_d3d12GapFence->Release();
		m_d3d12GapFence = nullptr;
	}
	if (m_gapFenceSharedHandle) {
		CloseHandle(m_gapFenceSharedHandle);
		m_gapFenceSharedHandle = nullptr;
	}
	m_gapFenceValue.store(0, std::memory_order_relaxed);
	m_gapTargetValue.store(0, std::memory_order_relaxed);

	// D3D12 game staging copy infrastructure
	for (uint32_t i = 0; i < m_gameSharedTexCount; i++) {
		if (m_gameSharedTexCache[i].d3d12Res) {
			m_gameSharedTexCache[i].d3d12Res->Release();
			m_gameSharedTexCache[i].d3d12Res = nullptr;
		}
		m_gameSharedTexCache[i].d3d11Tex = nullptr;
	}
	m_gameSharedTexCount = 0;
	if (m_d3d12GameCopyCmdList) {
		m_d3d12GameCopyCmdList->Release();
		m_d3d12GameCopyCmdList = nullptr;
	}
	if (m_d3d12GameCopyCmdAlloc) {
		m_d3d12GameCopyCmdAlloc->Release();
		m_d3d12GameCopyCmdAlloc = nullptr;
	}
	if (m_gameCopyFenceEvent) {
		CloseHandle(m_gameCopyFenceEvent);
		m_gameCopyFenceEvent = nullptr;
	}
	m_gameCopyFenceWaitPending = false;
	m_gameCopyFenceValue = 0;

	// D3D12 command infrastructure
	if (m_d3d12CmdList) {
		m_d3d12CmdList->Release();
		m_d3d12CmdList = nullptr;
	}
	if (m_d3d12CmdAlloc) {
		m_d3d12CmdAlloc->Release();
		m_d3d12CmdAlloc = nullptr;
	}
	if (m_d3d12CmdQueue) {
		m_d3d12CmdQueue->Release();
		m_d3d12CmdQueue = nullptr;
	}
	if (m_d3d12Device) {
		m_d3d12Device->Release();
		m_d3d12Device = nullptr;
	}

	// D3D11 QI refs
	if (m_d3d11Device5) {
		m_d3d11Device5->Release();
		m_d3d11Device5 = nullptr;
	}

	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}

	m_eyeWidth = 0;
	m_eyeHeight = 0;
	OOVR_LOG("ASW: Shutdown");
}
