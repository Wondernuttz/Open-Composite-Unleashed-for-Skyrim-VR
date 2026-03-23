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
#include <future>
#include <vector>
#include <d3d11.h>
#include <d3dcompiler.h>

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
    int _pad_npcMask;           // removed: was hasNpcMask
    float _pad1;                // alignment padding
    int _pad_debugMode;         // removed: was debugMode
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

    float2 searchDir = parallaxDir;
    float parallaxMagPx = length(parallaxDir * resolution);
    float mvSearchMagPx = length(roughMV * resolution);
    bool hasLoco = length(locoScreenDir) > 0.001;
    bool stationaryNpcMode = !hasLoco;
    float stationaryNpcMvConfidence = mvConfidence;
    bool movingNpcMask = false;  // NPC mask removed
    if (hasLoco && mvSearchMagPx > parallaxMagPx)
        searchDir = roughMV;
    float searchMagPx = hasLoco ? max(parallaxMagPx, mvSearchMagPx) : parallaxMagPx;

    // Convert to pixel-space stepping: normalize so dominant axis = +/-1 pixel/step.
    float2 searchDirPx = searchDir * resolution;
    float maxComp = max(abs(searchDirPx.x), abs(searchDirPx.y));
    float2 stepVec = (maxComp > 0.001) ? (searchDirPx / maxComp) : float2(0, 0);

    int2 fgPixel = pixel;
    bool foundForeground = false;
    bool npcExpanded = false;
    bool hasNpcScatter = false;
    bool hasDirectNpcScatter = false;
    bool npcDirectBodyAligned = false;
    int2 npcScatterDelta = int2(0, 0);
    int2 npcScatterSrcPixel = int2(0, 0);
    float npcScatterSrcDepth = 1.0;
    bool npcDestDepthRejected = false;
    bool npcTrailingRejected = false;
    bool carryScatterFound = false;
    float2 carryScatterDeltaPx = float2(0, 0);
    bool allowHeadDepthSearch = (!hasLoco && parallaxMagPx > 0.75 && origD >= 0.97);

)" R"(

    if (hasClipToClipNoLoco) {
        // CSNpcDepthScatter writes its support field in source-screen space plus the
        // NPC residual delta. Querying it at the current output pixel makes the whole
        // moving-NPC region lag behind head motion. Shift the lookup by the local
        // head-only c2c offset so we read from the same space the scatter pass wrote.
        float2 scatterQueryOffsetPx = parallaxDir * resolution;
        {
            float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            float4 clipPos = float4(ndc, d, 1.0);
            float4 prevClip = mul(clipToClipNoLoco, clipPos);
            if (abs(prevClip.w) > 0.0001) {
                float2 prevNDC = prevClip.xy / prevClip.w;
                float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);
                scatterQueryOffsetPx = (prevUV - uv) * resolution;
            }
        }
        int2 scatterQueryPixel = clamp(pixel + int2(round(scatterQueryOffsetPx)), int2(0,0), int2(resolution) - 1);

        uint packedNpc = atomicDepth[scatterQueryPixel];
        bool directHit = (packedNpc != 0xFFFFFFFFu);
        int2 hitPos = scatterQueryPixel;
        // If no direct scatter hit, search a slightly wider neighborhood so thin
        // moving-object leading edges can still pick up the scattered support band.
        if (!directHit) {
            int bestDist2 = 999999;
            [unroll] for (int sy = -6; sy <= 6; sy++) {
                [unroll] for (int sx = -6; sx <= 6; sx++) {
                    if (sx == 0 && sy == 0) continue;
                    int2 sp = clamp(scatterQueryPixel + int2(sx, sy), int2(0,0), int2(resolution) - 1);
                    uint np = atomicDepth[sp];
                    if (np == 0xFFFFFFFFu) continue;
                    int dist2 = sx * sx + sy * sy;
                    if (dist2 < bestDist2 || (dist2 == bestDist2 && np < packedNpc)) {
                        packedNpc = np;
                        hitPos = sp;
                        bestDist2 = dist2;
                    }
                }
            }
        }
        if (packedNpc != 0xFFFFFFFFu) {
            float warpedD = (float)(packedNpc >> 16) / 65535.0;
            int sdx = (int)((packedNpc >> 8) & 0xFFu) - 127;
            int sdy = (int)(packedNpc & 0xFFu) - 127;
            hasNpcScatter = true;
            hasDirectNpcScatter = directHit;
            npcScatterDelta = int2(sdx, sdy);
            npcScatterSrcPixel = clamp(hitPos - int2(sdx, sdy), int2(0,0), int2(resolution) - 1);
            npcScatterSrcDepth = depthTex[ToDepthCoord(npcScatterSrcPixel)];
            npcDirectBodyAligned = (!directHit || (origD - warpedD <= max(0.005, warpedD * 0.02)));
            // Trailing edge rejection: for neighbor hits (not direct scatter),
            // reject if pixel is behind the scatter direction.
            bool trailingEdge = false;
            float2 scatterDir = float2(sdx, sdy);
            float scatterLen = length(scatterDir);
            if (!directHit) {
                float2 pixelOfs = float2(pixel - hitPos);
                if (dot(scatterDir, pixelOfs) < -0.01) trailingEdge = true;
            }
            npcTrailingRejected = trailingEdge;
            float depthGap = origD - warpedD;
            float depthGapThresh = max(0.005, warpedD * 0.02);
            bool supportBand = false;
            // supportBand is only for boundary extension when a nearby scatter hit
            // suggests this destination belongs to the same moving NPC. Direct hits
            // already cover the body itself and should stay on the body/carry path;
            // promoting them to npcExpanded makes the whole NPC follow the looser
            // boundary rules and lags under head-only motion.
            if (!trailingEdge && !directHit && scatterLen > 0.5) {
                float2 dirNorm = scatterDir / scatterLen;
                float deltaSlack = max(1.5, scatterLen * 0.35);
                float bandForward = clamp(scatterLen * 0.35 + 1.0, 2.0, 6.0);
                float bandLateral = directHit ? 2.5 : 1.75;
                float bandDepthSlack = max(0.01, warpedD * 0.03);
                float occluderSlack = max(0.004, warpedD * 0.015);
                int supportCount = 0;
                [unroll] for (int sy = -5; sy <= 5; sy++) {
                    [unroll] for (int sx = -5; sx <= 5; sx++) {
                        int2 sp = clamp(pixel + int2(sx, sy), int2(0,0), int2(resolution) - 1);
                        uint np = atomicDepth[sp];
                        if (np == 0xFFFFFFFFu) continue;
                        float nWarpedD = (float)(np >> 16) / 65535.0;
                        int ndx = (int)((np >> 8) & 0xFFu) - 127;
                        int ndy = (int)(np & 0xFFu) - 127;
                        float2 nDelta = float2(ndx, ndy);
                        if (length(nDelta - scatterDir) > deltaSlack) continue;
                        float2 pixelOfs = float2(pixel - sp);
                        float along = dot(dirNorm, pixelOfs);
                        float lateral = length(pixelOfs - dirNorm * along);
                        if (along < -1.25 || along > bandForward) continue;
                        if (lateral > bandLateral) continue;
                        if (abs(nWarpedD - warpedD) > bandDepthSlack) continue;
                        supportCount++;
                    }
                }
                bool frontUnoccluded = (origD + occluderSlack >= warpedD);
                supportBand = frontUnoccluded && (supportCount >= 3);
            }
            if ((depthGap > depthGapThresh || supportBand) && !trailingEdge && !stationaryNpcMode) {
                float srcSlack = max(0.01, warpedD * 0.03);
                if (npcScatterSrcDepth > warpedD + srcSlack) {
                    float bestDist = 999.0;
                    int2 bestPx = npcScatterSrcPixel;
                    bool foundSrc = false;
                    [unroll] for (int ry = -6; ry <= 6; ry++) {
                        [unroll] for (int rx = -6; rx <= 6; rx++) {
                            int2 tp = clamp(npcScatterSrcPixel + int2(rx, ry), int2(0,0), int2(resolution) - 1);
                            float td = depthTex[ToDepthCoord(tp)];
                            if (td <= warpedD + srcSlack) {
                                float dist = float(rx * rx + ry * ry);
                                if (dist < bestDist) {
                                    bestDist = dist;
                                    bestPx = tp;
                                    foundSrc = true;
                                }
                            }
                        }
                    }
                    if (foundSrc) {
                        npcScatterSrcPixel = bestPx;
                        npcScatterSrcDepth = depthTex[ToDepthCoord(bestPx)];
                    }
                }
                foundForeground = true;
                npcExpanded = true;
                fgPixel = npcScatterSrcPixel;
                d = npcScatterSrcDepth;
            } else if (!trailingEdge && depthGap > depthGapThresh) {
                npcDestDepthRejected = true;
            }
        }
    }

    if (!foundForeground && (hasLoco || allowHeadDepthSearch) && searchMagPx > 0.5) {
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
                break;
        }
    }

    // Cardinal search fallback
    if (hasLoco && !foundForeground && origD > 0.50 && searchMagPx > 0.5) {
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

    // Step into foreground interior for cleaner MV
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
                    fgPixel = deepPx;
                } else {
                    break;
                }
            }
        }
    }

    // Recompute parallax with (possibly foreground) depth.
    // Both locomotion-found foreground and npcExpanded use fgPixel's angle for parallax
    // and uv as the base. This ensures npcExpanded pixels get the same warp as locomotion.
    // For stationary moving-NPC pixels we want one smooth carried source across the whole
    // local region, so only use the exact scatter source as a special-case base when there
    // is no smoothed carry field available yet.
    // Keep the NPC carry path active during head-only motion; dropping it as soon as
    // staticBlendFactor falls with head turns makes outlines peel away from the body.
    // DISABLED: stationary NPC handling causes ghost on head rotation for close NPCs.
    bool stationaryNpcRegion = false;
    bool stationaryNpcCarry = false;
    float2 stationaryCarryDeltaPx = float2(0,0);
    bool useScatterSourceAngle = false;
    bool useFgAngle = foundForeground || useScatterSourceAngle;
    // When the carry field is available, reuse it for boundary pixels too so the outline
    // stays locked to the carried NPC body instead of following per-pixel scatter noise.
    bool useFgBase = npcExpanded && !stationaryNpcCarry;
    int2 parallaxPixel = useScatterSourceAngle ? npcScatterSrcPixel : fgPixel;
    float2 fgUV = useFgAngle ? (float2(parallaxPixel) + 0.5) / resolution : uv;
    float fgTanX = useFgAngle ? lerp(fovTanLeft, fovTanRight, fgUV.x) : tanX;
    float fgTanY = useFgAngle ? lerp(fovTanUp, fovTanDown, fgUV.y) : tanY;

    float parallaxDepth = useScatterSourceAngle ? npcScatterSrcDepth : d;
    linearDepth = LinearizeDepth(parallaxDepth, nearZ, farZ);
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
    // BG npcExpanded: uv base (body MV redirects to NPC body, continuous with neighbors).
    // FG npcExpanded: fgUV base (samples from NPC interior, prevents leading-edge holes).
    // Locomotion search: uv base (original behavior).
    float2 parallaxSourceUV = (useFgBase ? fgUV : uv) + fadedParallax;

    // Read MV
    int2 mvSourcePixel;
    bool stationaryBodyMV = false;  // DISABLED: part of stationary NPC handling that causes ghost
    bool usableNpcScatterForBody = (hasNpcScatter && (!hasDirectNpcScatter || npcDirectBodyAligned));
    bool stationaryBodyScatterAssist = (stationaryBodyMV &&
        (carryScatterFound || usableNpcScatterForBody));
    if (foundForeground) {
        mvSourcePixel = ToMVCoord(fgPixel);
    } else if (stationaryBodyMV) {
        // In the stationary case, silhouette/body pixels are already current-frame NPC.
        // Sampling MV at parallaxSourceUV can step onto background right at the cached
        // border, which leaves the old silhouette behind instead of carrying the body
        // forward with the expanded boundary.
        mvSourcePixel = ToMVCoord(pixel);
    } else {
        mvSourcePixel = ToMVCoord(clamp(int2(parallaxSourceUV * resolution), int2(0,0), int2(resolution) - 1));
    }
    float2 rawMV = mvTex[mvSourcePixel];
    float2 totalMV = rawMV * mvPixelScale;
    totalMV = clamp(totalMV, float2(-0.15, -0.15), float2(0.15, 0.15));

)" R"(
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

    // c2cHeadMV from clipToClipNoLoco: subtracts full head motion and matches fgPixel
    // when foreground was found.
    float2 c2cHeadMV = float2(0, 0);
    if (hasClipToClipNoLoco) {
        float2 c2cBaseUV = useFgAngle ? fgUV : uv;
        float2 ndc = float2(c2cBaseUV.x * 2.0 - 1.0, 1.0 - c2cBaseUV.y * 2.0);
        float4 clipPos = float4(ndc, d, 1.0);
        float4 prevClip = mul(clipToClipNoLoco, clipPos);
        if (abs(prevClip.w) > 0.0001) {
            float2 prevNDC = prevClip.xy / prevClip.w;
            c2cHeadMV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5) - c2cBaseUV;
        }
    }

    // Residuals for diagnostics
    float2 c2cResidual = totalMV - c2cHeadMV;       // head+stick subtracted (loco only)
    float2 headOnlyResidual = totalMV - headOnlyMV;  // head-only subtracted (stick + loco)

)" R"(
    float2 stationaryResidual = c2cResidual;
    float2 mvOffset = float2(0, 0);
    if (abs(mvConfidence) > 0.001) {
        // npcExpanded pixels already used the residual NPC motion once in CSNpcDepthScatter
        // to land on this output pixel. Applying it again here double-shifts the boundary
        // sample into background.
        if (!npcExpanded) {
            float2 residual = hasLoco ? headOnlyResidual : stationaryResidual;
            mvOffset = stationaryNpcMvConfidence * residual;
        }
    }

    float stationaryResidualMagPx = length(stationaryResidual * resolution);
    float headMagPx = length(c2cHeadMV * resolution);
    float stationaryBodyResidualThreshPx = max(0.75, 0.35 + headMagPx * 0.20);
    bool stationaryBodyUseDirectMV = (stationaryBodyMV &&
        stationaryResidualMagPx >= stationaryBodyResidualThreshPx);
    bool staleDirectBody = (stationaryNpcMode && stationaryBodyMV &&
        hasDirectNpcScatter && !npcDirectBodyAligned && !carryScatterFound);

    float2 sourceUV = parallaxSourceUV + mvOffset;

    if (stationaryNpcCarry) {
        sourceUV = parallaxSourceUV - stationaryCarryDeltaPx / float2(resolution);
    } else if (stationaryBodyMV) {
        // Only use the direct current-pixel MV path when there is real non-head residual
        // motion at this pixel. Nearby static ground during head rotation falls into the
        // same depth band as close NPCs, but its residual stays near zero and should remain
        // on the original parallax path instead of following the body-MV shortcut.
        if (stationaryBodyUseDirectMV) {
            // Apply 1.0x head motion + stationaryNpcMvConfidence * animation residual.
            // totalMV = headMV + residual, so: uv + headMV + conf * residual
            //   = uv + conf * totalMV - (conf - 1) * headMV
            sourceUV = uv + stationaryNpcMvConfidence * totalMV
                         - (stationaryNpcMvConfidence - 1.0) * c2cHeadMV;
        } else {
            sourceUV = parallaxSourceUV + mvOffset;
        }
        if (stationaryBodyScatterAssist && (carryScatterFound || usableNpcScatterForBody)) {
            int2 carryDelta = carryScatterFound ? int2(round(carryScatterDeltaPx)) : npcScatterDelta;
            // Use parallaxSourceUV (cached-frame position) as base, not uv (output position).
            // The scatter delta is in cached-frame coordinates. Without head parallax in the
            // base, the entire NPC shifts by the head offset — creating a full-width ghost
            // that tracks head rotation, visible at close range where parallax is large.
            sourceUV = parallaxSourceUV - float2(carryDelta) / float2(resolution);
        }
    }

    // In locomotion mode, npcExpanded boundary pixels use fgUV for small head motion
    // to avoid sampling background. (stationaryNpcMode disables npcExpanded entirely —
    // the NPC forward overlay handles moving-NPC edges instead.)
    if (npcExpanded && !stationaryNpcMode && !stationaryNpcCarry && parallaxMagPx <= 0.75) {
        sourceUV = fgUV;
    }

    // Only let the anchor mismatch fallback steer actual foreground/body pixels.
    // Background pixels that merely sit near scattered NPC support can otherwise get
    // pulled onto stale previous-frame limbs, which shows up as detached duplicates.
    if (stationaryNpcMode && !npcExpanded && origD < 0.97 &&
        stationaryBodyScatterAssist && (usableNpcScatterForBody || carryScatterFound)) {
        float2 anchorDeltaPx = carryScatterFound ? carryScatterDeltaPx : float2(npcScatterDelta);
        // Compare deltas in cached-frame space (relative to parallaxSourceUV, not pixel).
        float2 chosenDeltaPx = parallaxSourceUV * float2(resolution) - sourceUV * float2(resolution);
        float mismatch = length(chosenDeltaPx - anchorDeltaPx);
        float mismatchThresh = max(1.5, length(anchorDeltaPx) * 0.35);
        if (mismatch > mismatchThresh) {
            sourceUV = parallaxSourceUV - anchorDeltaPx / float2(resolution);
        }
    }

    if (any(sourceUV < -0.01) || any(sourceUV > 1.01)) {
        sourceUV = uv + mvOffset;
        if (any(sourceUV < -0.01) || any(sourceUV > 1.01))
            sourceUV = uv;
    }

    // Edge depth guard: only for npcExpanded scatter pixels whose sourceUV may
    // overshoot the NPC body. NOT for regular NPC body pixels — their MV-corrected
    // sourceUV may land on current-frame background at trailing edges (NPC moved away),
    // but the prevColor at that position is correct (NPC was there last frame).
    if (npcExpanded) {
        int2 srcPx = clamp(int2(sourceUV * float2(resolution)), int2(0,0), int2(resolution) - 1);
        float srcD = depthTex[ToDepthCoord(srcPx)];
        float fgSlack = max(0.01, d * 0.03);
        if (srcD - d > fgSlack) {
            float bestDist = 999.0;
            int2 bestPx = srcPx;
            bool foundFg = false;
            [unroll] for (int ry = -8; ry <= 8; ry++) {
                [unroll] for (int rx = -8; rx <= 8; rx++) {
                    if (rx == 0 && ry == 0) continue;
                    int2 tp = clamp(srcPx + int2(rx, ry), int2(0,0), int2(resolution) - 1);
                    float td = depthTex[ToDepthCoord(tp)];
                    if (td <= d + fgSlack) {
                        float dist = float(rx*rx + ry*ry);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestPx = tp;
                            foundFg = true;
                        }
                    }
                }
            }
            if (foundFg) {
                sourceUV = (float2(bestPx) + 0.5) / float2(resolution);
            } else {
                // Preserve head parallax on the fallback too. Dropping back to the raw
                // cached-frame source pixel makes boundary ghosts drift outward on head
                // turns and then snap back once the local guard recovers.
                sourceUV = stationaryNpcCarry
                    ? (parallaxSourceUV - stationaryCarryDeltaPx / float2(resolution))
                    : parallaxSourceUV;
            }
        }
    } else if (staleDirectBody) {
        float bgSlack = max(0.01, origD * 0.03);
        float2 bgSourceUV = parallaxSourceUV;
        int2 bgSrcPx = clamp(int2(bgSourceUV * float2(resolution)), int2(0,0), int2(resolution) - 1);
        float bestScore = 999.0;
        int2 bestPx = bgSrcPx;
        bool foundBg = false;
        [unroll] for (int ry = -8; ry <= 8; ry++) {
            [unroll] for (int rx = -8; rx <= 8; rx++) {
                int2 tp = clamp(bgSrcPx + int2(rx, ry), int2(0,0), int2(resolution) - 1);
                float td = depthTex[ToDepthCoord(tp)];
                float depthErr = abs(td - origD);
                if (depthErr > bgSlack)
                    continue;
                float dist = float(rx * rx + ry * ry);
                float score = dist + depthErr * 2048.0;
                if (score < bestScore) {
                    bestScore = score;
                    bestPx = tp;
                    foundBg = true;
                }
            }
        }
        sourceUV = foundBg
            ? ((float2(bestPx) + 0.5) / float2(resolution))
            : bgSourceUV;
    } else if (!stationaryNpcCarry && stationaryNpcMode && origD < 0.97 &&
        stationaryBodyScatterAssist && (carryScatterFound || usableNpcScatterForBody)) {
        int2 srcPx = clamp(int2(sourceUV * float2(resolution)), int2(0,0), int2(resolution) - 1);
        float srcD = depthTex[ToDepthCoord(srcPx)];
        float fgSlack = max(0.01, origD * 0.03);
        if (srcD - origD > fgSlack) {
            int2 carryDelta = carryScatterFound ? int2(round(carryScatterDeltaPx)) : npcScatterDelta;
            float bestDist = 999.0;
            int2 bestPx = srcPx;
            bool foundFg = false;
            [unroll] for (int ry = -8; ry <= 8; ry++) {
                [unroll] for (int rx = -8; rx <= 8; rx++) {
                    if (rx == 0 && ry == 0) continue;
                    int2 tp = clamp(srcPx + int2(rx, ry), int2(0,0), int2(resolution) - 1);
                    float td = depthTex[ToDepthCoord(tp)];
                    if (td <= origD + fgSlack) {
                        float dist = float(rx*rx + ry*ry);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestPx = tp;
                            foundFg = true;
                        }
                    }
                }
            }
            if (foundFg) {
                sourceUV = (float2(bestPx) + 0.5) / float2(resolution);
            } else {
                sourceUV = uv - float2(carryDelta) / float2(resolution);
            }
        }
    }

    // Ghost suppression: when stationary, detect backward-warp pixels that are
    // moving NPC content (significant MV residual). Replace with background by
    // searching along the residual direction for deeper depth. The NPC forward
    // overlay then composites the correctly-positioned NPC on top.
    if (stationaryNpcMode && origD < 0.97) {
        int2 ghostSrcPx = clamp(int2(sourceUV * float2(resolution)), int2(0,0), int2(resolution) - 1);
        float2 ghostMV = mvTex[ToMVCoord(ghostSrcPx)] * mvPixelScale;
        // Compute accurate head MV at the source pixel position and depth,
        // not the output pixel. For close objects with large parallax, the source
        // is many pixels from the output — head MV differs significantly, causing
        // false negatives in the residual check.
        float2 ghostSrcUV = (float2(ghostSrcPx) + 0.5) / float2(resolution);
        float ghostSrcD = depthTex[ToDepthCoord(ghostSrcPx)];
        float2 ghostHeadMV = c2cHeadMV; // fallback
        if (hasClipToClipNoLoco) {
            float2 gNdc = float2(ghostSrcUV.x * 2.0 - 1.0, 1.0 - ghostSrcUV.y * 2.0);
            float4 gClip = float4(gNdc, ghostSrcD, 1.0);
            float4 gPrev = mul(clipToClipNoLoco, gClip);
            if (abs(gPrev.w) > 0.0001) {
                float2 gPrevNDC = gPrev.xy / gPrev.w;
                ghostHeadMV = float2(gPrevNDC.x * 0.5 + 0.5, 0.5 - gPrevNDC.y * 0.5) - ghostSrcUV;
            }
        }
        float2 ghostResidual = ghostMV - ghostHeadMV;
        float ghostResidualPx = length(ghostResidual * resolution);
        float ghostThreshold = 1.5;

        if (ghostResidualPx > ghostThreshold) {
            // Source is a moving object that has departed. Search along the
            // residual direction (toward trailing edge) for background depth.
            float2 searchDir = normalize(ghostResidual * resolution);
            float ghostD = depthTex[ToDepthCoord(ghostSrcPx)];
            float bgSlack = max(0.005, ghostD * 0.02);
            bool foundBg = false;
            for (int step = 1; step <= 32; step++) {
                int2 tp = clamp(ghostSrcPx + int2(round(searchDir * float(step))),
                    int2(0,0), int2(resolution) - 1);
                float td = depthTex[ToDepthCoord(tp)];
                if (td - ghostD > bgSlack) {
                    sourceUV = (float2(tp) + 0.5) / float2(resolution);
                    foundBg = true;
                    break;
                }
            }
            if (!foundBg) {
                // Try opposite direction (leading edge background)
                for (int step = 1; step <= 32; step++) {
                    int2 tp = clamp(ghostSrcPx - int2(round(searchDir * float(step))),
                        int2(0,0), int2(resolution) - 1);
                    float td = depthTex[ToDepthCoord(tp)];
                    if (td - ghostD > bgSlack) {
                        sourceUV = (float2(tp) + 0.5) / float2(resolution);
                        foundBg = true;
                        break;
                    }
                }
            }
        }
    }

    sourceUV = saturate(sourceUV);

    // Sample cached frame
    float4 color = prevColor.SampleLevel(linearClamp, sourceUV, 0);

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

    // half=0 when stretch<=2 (single pixel, small gap). Cap at 1 (3x3 max) to
    // cover the larger lattice gaps during locomotion without thickening foliage.
    int halfW = clamp((int)ceil((stretchX - 2.0) * 0.5), 0, 1);
    int halfH = clamp((int)ceil((stretchY - 2.0) * 0.5), 0, 1);
    return int2(halfW, halfH);
}

float EffectiveStationaryNpcMvConfidence() {
    return mvConfidence;
}

bool IsMovingNpcPixelForward(int2 srcPixel, float d) {
    if (d >= 0.999)
        return false;

    float2 rawMV = mvTex[ToMVCoord(srcPixel)];
    float2 totalMV = clamp(rawMV * mvPixelScale, float2(-0.15, -0.15), float2(0.15, 0.15));

    float2 srcUV = (float2(srcPixel) + 0.5) / resolution;
    float2 c2cHMV = float2(0, 0);
    if (hasClipToClipNoLoco) {
        float2 ndc = float2(srcUV.x * 2 - 1, (1 - srcUV.y) * 2 - 1);
        float4 clipPos = float4(ndc, d, 1.0);
        float4 prevClip = mul(clipToClipNoLoco, clipPos);
        if (abs(prevClip.w) > 0.0001) {
            float2 prevNDC = prevClip.xy / prevClip.w;
            c2cHMV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5) - srcUV;
        }
    }

    float2 npcMV = totalMV - c2cHMV;
    bool hasLoco = length(locoScreenDir) > 0.001;
    float2 npcOffsetPx = -npcMV * EffectiveStationaryNpcMvConfidence() * resolution;
    float headMagPx = length(c2cHMV * resolution);
    float minResidualPx = !hasLoco ? 1.0 : 0.5;

    return dot(npcOffsetPx, npcOffsetPx) >= minResidualPx * minResidualPx;
}

[numthreads(8, 8, 1)]
void CSForwardDepthNpcOnly(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    int2 srcPixel = (int2)tid.xy;
    float d = depthTex[ToDepthCoord(srcPixel)];
    if (!IsMovingNpcPixelForward(srcPixel, d))
        return;

    uint quantizedDepth = asuint(d);
    float2 dstUV = computeForwardDstUV(srcPixel, false);
    float2 dstPx = dstUV * resolution;
    if (any(dstUV < -0.001) || any(dstUV >= 1.001))
        return;

    int2 p0 = int2(floor(dstPx - 0.5));
    for (int sy = 0; sy <= 1; sy++) {
        for (int sx = 0; sx <= 1; sx++) {
            int2 p = p0 + int2(sx, sy);
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                continue;
            InterlockedMin(atomicDepth[p], quantizedDepth);
        }
    }
}

[numthreads(8, 8, 1)]
void CSForwardColorNpcOnly(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    int2 srcPixel = (int2)tid.xy;
    float d = depthTex[ToDepthCoord(srcPixel)];
    if (!IsMovingNpcPixelForward(srcPixel, d))
        return;

    uint quantizedDepth = asuint(d);
    float2 srcUV = ((float2)srcPixel + 0.5) / resolution;
    float2 dstUV = computeForwardDstUV(srcPixel, false);
    float2 dstPx = dstUV * resolution;
    if (any(dstUV < -0.001) || any(dstUV >= 1.001))
        return;

    float4 color = prevColor.SampleLevel(linearClamp, srcUV, 0);
    int2 p0 = int2(floor(dstPx - 0.5));
    for (int sy = 0; sy <= 1; sy++) {
        for (int sx = 0; sx <= 1; sx++) {
            int2 p = p0 + int2(sx, sy);
            if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                continue;
            if (atomicDepth[p] == quantizedDepth)
                output[p] = color;
        }
    }
}

)" R"(
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

    float2 dstUV = computeForwardDstUV((int2)tid.xy, false);
    float2 dstPx = dstUV * resolution;

    // When near-stationary (staticBlendFactor > 0.5), freeze static pixels to source
    // position. Even tiny head rotations create multi-pixel displacements across the
    // frame, but the correction is imperceptible — it only thickens foliage via
    // integer rounding. Global freeze gives 1:1 faithful output when stationary.
    if (staticBlendFactor > 0.5) {
        dstPx = (float2)tid.xy + 0.5;
        dstUV = dstPx / resolution;
    }

    if (any(dstUV < -0.001) || any(dstUV >= 1.001)) return;

    if (staticBlendFactor > 0.5) {
        // Frozen: exact 1x1 at source position (no gap fill needed)
        int2 p = clamp(int2(floor(dstPx)), int2(0,0), int2(resolution) - 1);
        InterlockedMin(atomicDepth[p], quantizedDepth);
    } else {
        // Moving: bilinear 2x2 splat fills sub-pixel gaps between scattered pixels
        int2 p0 = int2(floor(dstPx - 0.5));
        for (int sy = 0; sy <= 1; sy++) {
            for (int sx = 0; sx <= 1; sx++) {
                int2 p = p0 + int2(sx, sy);
                if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                    continue;
                InterlockedMin(atomicDepth[p], quantizedDepth);
            }
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

    float2 dstUV = computeForwardDstUV((int2)tid.xy, false);
    float2 dstPx = dstUV * resolution;

    // When near-stationary (staticBlendFactor > 0.5), freeze static pixels to source position.
    if (staticBlendFactor > 0.5) {
        dstPx = (float2)tid.xy + 0.5;
        dstUV = dstPx / resolution;
    }

    if (any(dstUV < -0.001) || any(dstUV >= 1.001)) return;

    float4 color = prevColor.SampleLevel(linearClamp, srcUV, 0);

    if (staticBlendFactor > 0.5) {
        // Frozen: exact 1x1
        int2 p = clamp(int2(floor(dstPx)), int2(0,0), int2(resolution) - 1);
        if (quantizedDepth == atomicDepth[p])
            output[p] = color;
    } else {
        // Moving: bilinear 2x2 splat, depth-tested
        int2 p0 = int2(floor(dstPx - 0.5));
        for (int sy = 0; sy <= 1; sy++) {
            for (int sx = 0; sx <= 1; sx++) {
                int2 p = p0 + int2(sx, sy);
                if (any(p < 0) || p.x >= (int)resolution.x || p.y >= (int)resolution.y)
                    continue;
                if (quantizedDepth == atomicDepth[p])
                    output[p] = color;
            }
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

static const char* s_npcForwardCompositeHLSL = R"(
Texture2D<float4> npcForwardTex  : register(t0);
Texture2D<uint>   npcForwardMask : register(t1);
RWTexture2D<float4> output       : register(u0);

[numthreads(8, 8, 1)]
void CSCompositeNpcForward(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    npcForwardMask.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;

    if (npcForwardMask[tid.xy] == 0xFFFFFFFFu)
        return;

    output[tid.xy] = npcForwardTex[tid.xy];
}
)";

static const char* s_npcScatterHLSL = R"(
// ── NPC depth scatter: forward-scatter NPC depth for backward warp boundary extension ──
// For each pixel with significant NPC animation MV, scatter its depth to the
// predicted NPC position via InterlockedMin. The packed value stores 16 bits of
// depth and 8 bits per axis for the source offset (signed ±127).
[numthreads(8, 8, 1)]
void CSNpcDepthScatter(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    int2 srcPixel = (int2)tid.xy;
    int2 depthPixel = ToDepthCoord(srcPixel);
    float d = depthTex[depthPixel];

    if (d >= 0.999) return;
    float2 rawMV = mvTex[ToMVCoord(srcPixel)];
    float2 totalMV = clamp(rawMV * mvPixelScale, float2(-0.15, -0.15), float2(0.15, 0.15));

    float2 srcUV = (float2(srcPixel) + 0.5) / resolution;
    float2 c2cHMV = float2(0, 0);
    if (hasClipToClipNoLoco) {
        float2 ndc = float2(srcUV.x * 2 - 1, (1 - srcUV.y) * 2 - 1);
        float4 clipPos = float4(ndc, d, 1.0);
        float4 prevClip = mul(clipToClipNoLoco, clipPos);
        if (abs(prevClip.w) > 0.0001) {
            float2 prevNDC = prevClip.xy / prevClip.w;
            c2cHMV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5) - srcUV;
        }
    }

    float2 npcMV = totalMV - c2cHMV;
    bool hasLoco = length(locoScreenDir) > 0.001;
    float stationaryNpcMvConfidence = mvConfidence;
    float2 npcOffsetPx = -npcMV * stationaryNpcMvConfidence * resolution;
    float headMagPx = length(c2cHMV * resolution);
    float minScatterResidualPx = 0.5;
    if (!hasLoco) {
        // Without an engine mask, the stationary scatter path is a generic residual-motion
        // fallback. During fast head turns, static geometry can pick up small residual MV
        // error and leave thin lagging strips behind the camera. Raise the required
        // residual as head motion grows so only clearly object-driven motion scatters.
        minScatterResidualPx = 1.0;
    }
    if (dot(npcOffsetPx, npcOffsetPx) < minScatterResidualPx * minScatterResidualPx) return;

    int2 dstPixel = srcPixel + int2(round(npcOffsetPx));
    [unroll] for (int sy = -1; sy <= 1; sy++) {
        [unroll] for (int sx = -1; sx <= 1; sx++) {
            int2 writePixel = dstPixel + int2(sx, sy);
            if (any(writePixel < 0) || writePixel.x >= (int)resolution.x || writePixel.y >= (int)resolution.y)
                continue;
            int dx = clamp(writePixel.x - srcPixel.x, -127, 127);
            int dy = clamp(writePixel.y - srcPixel.y, -127, 127);
            uint depth16 = (uint)round(saturate(d) * 65535.0);
            uint packed = (depth16 << 16) |
                ((uint)clamp(dx + 127, 0, 255) << 8) |
                (uint)clamp(dy + 127, 0, 255);
            InterlockedMin(atomicDepth[writePixel], packed);
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

// Simple FNV-1a hash for shader cache invalidation
static uint64_t FnvHash(const void* data, size_t len)
{
	uint64_t h = 14695981039346656037ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= ((const uint8_t*)data)[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// Compile a shader, using disk cache when available. Cache key = FNV-1a hash
// of (source + entry + flags). Saves ~20s on CSMain which has pathological
// compile time due to complex control flow + large function body.
static bool CompileOrLoadCached(
    const std::string& source, const char* entry, const char* target,
    DWORD flags, ID3DBlob** outBlob, float* outMs)
{
	auto start = std::chrono::high_resolution_clock::now();

	// Build cache key from source + entry point + flags
	uint64_t srcHash = FnvHash(source.data(), source.size());
	uint64_t entryHash = FnvHash(entry, strlen(entry));
	uint64_t flagsHash = FnvHash(&flags, sizeof(flags));
	uint64_t cacheKey = srcHash ^ (entryHash * 31) ^ (flagsHash * 997);

	// Cache directory: next to the DLL in a .shader_cache subfolder
	namespace fs = std::filesystem;
	wchar_t dllPath[MAX_PATH] = {};
	HMODULE hm = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    (LPCWSTR)&CompileOrLoadCached, &hm);
	GetModuleFileNameW(hm, dllPath, MAX_PATH);
	fs::path cacheDir = fs::path(dllPath).parent_path() / ".shader_cache";
	char cacheName[64];
	snprintf(cacheName, sizeof(cacheName), "%s_%016llx.cso", entry, (unsigned long long)cacheKey);
	fs::path cachePath = cacheDir / cacheName;

	// Try loading from cache
	if (fs::exists(cachePath)) {
		std::ifstream f(cachePath, std::ios::binary | std::ios::ate);
		if (f.is_open()) {
			auto sz = f.tellg();
			if (sz > 0) {
				f.seekg(0);
				HRESULT hr = D3DCreateBlob((SIZE_T)sz, outBlob);
				if (SUCCEEDED(hr)) {
					f.read((char*)(*outBlob)->GetBufferPointer(), sz);
					if (f.good()) {
						auto end = std::chrono::high_resolution_clock::now();
						if (outMs) *outMs = std::chrono::duration<float, std::milli>(end - start).count();
						return true;
					}
					(*outBlob)->Release();
					*outBlob = nullptr;
				}
			}
		}
	}

	// Cache miss — compile
	ID3DBlob* errs = nullptr;
	HRESULT hr = D3DCompile(source.c_str(), source.size(),
	    entry, nullptr, nullptr, entry, target, flags, 0, outBlob, &errs);
	auto end = std::chrono::high_resolution_clock::now();
	if (outMs) *outMs = std::chrono::duration<float, std::milli>(end - start).count();

	if (FAILED(hr)) {
		if (errs) {
			OOVR_LOGF("ASW: Shader compile error (%s): %s", entry, (char*)errs->GetBufferPointer());
			errs->Release();
		}
		return false;
	}
	if (errs) errs->Release();

	// Save to cache
	try {
		fs::create_directories(cacheDir);
		std::ofstream f(cachePath, std::ios::binary);
		if (f.is_open()) {
			f.write((const char*)(*outBlob)->GetBufferPointer(), (*outBlob)->GetBufferSize());
		}
	} catch (...) {
		// Cache write failure is non-fatal
	}
	return true;
}

bool ASWProvider::CreateComputeShader(ID3D11Device* device)
{
	// Compile HLSL — use LEVEL0 for fast compilation (~2s total vs 20s at LEVEL1).
	// GPU perf difference is negligible for these warp shaders.
	DWORD flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
#endif

	// Build source strings (shared across parallel jobs — read-only, thread-safe)
	std::string csMainSrc     = std::string(s_warpShaderHLSL);
	std::string fullShaderSrc = csMainSrc + s_forwardScatterHLSL;
	std::string npcShaderSrc  = csMainSrc + s_npcScatterHLSL;
	std::string compositeStr  = std::string(s_npcForwardCompositeHLSL);

	// D3DCompile is thread-safe (stateless compiler). Device::CreateComputeShader is NOT
	// thread-safe on D3D11, so we compile to blobs in parallel, then create shaders serially.
	struct ShaderJob {
		const char* entry;
		const std::string* source;
		ID3DBlob* blob = nullptr;
		bool ok = false;
	};
	ShaderJob jobs[] = {
	    {"CSMain",                 &csMainSrc},     // [0] — backward warp (required)
	    {"CSClear",                &fullShaderSrc}, // [1]
	    {"CSForwardDepth",         &fullShaderSrc}, // [2]
	    {"CSForwardColor",         &fullShaderSrc}, // [3]
	    {"CSForwardDepthNpcOnly",  &fullShaderSrc}, // [4]
	    {"CSForwardColorNpcOnly",  &fullShaderSrc}, // [5]
	    {"CSDilate",               &fullShaderSrc}, // [6]
	    {"CSCompositeNpcForward",  &compositeStr},  // [7]
	    {"CSNpcDepthScatter",      &npcShaderSrc},  // [8]
	};
	constexpr int kJobCount = _countof(jobs);

	// Launch parallel compilation (with disk caching — first run ~20s, subsequent <1ms)
	std::vector<std::future<void>> futures;
	futures.reserve(kJobCount);
	for (int i = 0; i < kJobCount; i++) {
		futures.push_back(std::async(std::launch::async, [&jobs, i, flags]() {
			jobs[i].ok = CompileOrLoadCached(
			    *jobs[i].source, jobs[i].entry, "cs_5_0", flags,
			    &jobs[i].blob, nullptr);
		}));
	}
	// Wait for all compilations
	for (auto& f : futures)
		f.get();

	// Create compute shaders serially (D3D11 device is not thread-safe)
	ID3D11ComputeShader** targets[] = {
	    &m_warpCS,               // [0] CSMain
	    &m_clearCS, &m_forwardDepthCS, &m_forwardColorCS,
	    &m_forwardDepthNpcOnlyCS, &m_forwardColorNpcOnlyCS, &m_dilateCS,
	    &m_compositeNpcForwardCS, &m_npcDepthScatterCS,
	};
	bool csMainOk = false;
	bool allForwardOk = true;
	for (int i = 0; i < kJobCount; i++) {
		if (!jobs[i].ok) {
			OOVR_LOGF("ASW: %s compilation failed%s", jobs[i].entry,
			    i == 0 ? " (FATAL)" : " (non-fatal)");
			if (i == 0) return false; // CSMain is required
			if (i >= 1 && i <= 6) allForwardOk = false;
			continue;
		}
		HRESULT r = device->CreateComputeShader(
		    jobs[i].blob->GetBufferPointer(), jobs[i].blob->GetBufferSize(),
		    nullptr, targets[i]);
		jobs[i].blob->Release();
		if (FAILED(r)) {
			OOVR_LOGF("ASW: CreateComputeShader(%s) failed hr=0x%08x", jobs[i].entry, (unsigned)r);
			if (i == 0) return false;
			if (i >= 1 && i <= 6) allForwardOk = false;
		} else if (i == 0) {
			csMainOk = true;
		}
	}
	if (allForwardOk) {
		OOVR_LOG("ASW: Forward scatter shaders compiled");
	} else {
		OOVR_LOG("ASW: Some forward scatter shaders failed (non-fatal — backward warp still available)");
	}
	if (m_npcDepthScatterCS)
		OOVR_LOG("ASW: CSNpcDepthScatter compiled for moving-NPC boundary extension");

	// Constant buffer and sampler
	HRESULT hr;
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

	for (uint32_t slot = 0; slot < kAswCacheSlotCount; ++slot) {
		for (int eye = 0; eye < 2; ++eye) {

			// Cached color (RGBA)
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = 0;
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

		}
	}

	for (int eye = 0; eye < 2; eye++) {
		// Warped output (RGBA, UAV for compute shader)
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;
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

		// Scratch output for stationary NPC-only forward overlay (D3D11 path only).
		desc.MiscFlags = 0;
		hr = device->CreateTexture2D(&desc, nullptr, &m_forwardNpcOutput[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateTexture2D forwardNpcOutput[%d] failed hr=0x%08X", eye, hr);
			return false;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC npcOutputUavDesc = {};
		npcOutputUavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		npcOutputUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(m_forwardNpcOutput[eye], &npcOutputUavDesc, &m_uavForwardNpcOutput[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateUAV forwardNpcOutput[%d] failed", eye);
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC npcOutputSrvDesc = {};
		npcOutputSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		npcOutputSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		npcOutputSrvDesc.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_forwardNpcOutput[eye], &npcOutputSrvDesc, &m_srvForwardNpcOutput[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateSRV forwardNpcOutput[%d] failed", eye);
			return false;
		}
	}

	// Forward scatter: atomic depth buffers (R32_UINT, per-eye)
	for (int eye = 0; eye < 2; eye++) {
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;
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
		D3D11_SHADER_RESOURCE_VIEW_DESC atomicSrvDesc = {};
		atomicSrvDesc.Format = DXGI_FORMAT_R32_UINT;
		atomicSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		atomicSrvDesc.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_atomicDepth[eye], &atomicSrvDesc, &m_srvAtomicDepth[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateSRV atomicDepth[%d] failed", eye);
			return false;
		}

	}

	OOVR_LOGF("ASW: Cache textures created (%u slots x 2 eyes)", kAswCacheSlotCount);
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

	OOVR_LOGF("ASW: Output swapchain created %ux%u (%u images)", width, height, imageCount);
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
// Per-frame operations
// ============================================================================


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
		if (!SafeBridgeCopy(ctx, m_cachedDepth[slot][eye], 0, 0, 0, 0,
		        depthTex, 0, depthRegion)) {
			OOVR_LOG("ASW: TOCTOU - depth texture freed during copy");
			return;
		}
		m_slotDepthDataW[slot] = depthRegion->right - depthRegion->left;
		m_slotDepthDataH[slot] = depthRegion->bottom - depthRegion->top;

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

	}
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
	cb._pad_npcMask = 0;  // removed: was hasNpcMask



	const float locoMotion = sqrtf(m_locoTransX * m_locoTransX +
		m_locoTransY * m_locoTransY + m_locoTransZ * m_locoTransZ);
	const float stickYawMotion = fabsf(m_locoYaw);
	const bool useForwardScatterForMotion = (locoMotion > 0.15f || stickYawMotion > 0.002f);

	// ── D3D11 warp path ──
	ID3D11DeviceContext* ctx = nullptr;
	m_device->GetImmediateContext(&ctx);
	if (!ctx)
		return false;

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
	ctx->CSSetShaderResources(0, _countof(srvs), srvs);
	ID3D11UnorderedAccessView* uavs2[] = { m_uavOutput[eye], m_uavAtomicDepth[eye] };
	ctx->CSSetUnorderedAccessViews(0, 2, uavs2, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &m_constantBuffer);
	ctx->CSSetSamplers(0, 1, &m_linearSampler);

	uint32_t groupsX = (m_eyeWidth + 7) / 8;
	uint32_t groupsY = (m_eyeHeight + 7) / 8;
	// NPC-only forward scatter overlay: forward-scatter only pixels with significant
	// MV residual (moving objects) on top of the backward warp output. Uses
	// IsMovingNpcPixelForward() which detects motion via MV residual after c2c head
	// subtraction, with a threshold that scales with head motion magnitude.
	const bool useStationaryNpcForwardOverlay = !useForwardScatterForMotion;

	// Forward scatter pipeline: each SOURCE pixel projects to its destination.
	// Depth test via InterlockedMin ensures closest pixel wins (tree over sky).
	// This naturally handles locomotion disocclusion without expensive searches.
	if (m_clearCS && m_forwardDepthCS && m_forwardColorCS && m_dilateCS && useForwardScatterForMotion) {
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
		if (m_clearCS) {
			ctx->CSSetShader(m_clearCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);

			if (m_npcDepthScatterCS) {
				ctx->CSSetShader(m_npcDepthScatterCS, nullptr, 0);
				ctx->Dispatch(groupsX, groupsY, 1);
			}
		}

		ctx->CSSetShader(m_warpCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);

		if (useStationaryNpcForwardOverlay) {
			ID3D11UnorderedAccessView* overlayUAVs[] = { m_uavForwardNpcOutput[eye], m_uavAtomicDepth[eye] };
			ctx->CSSetUnorderedAccessViews(0, 2, overlayUAVs, nullptr);

			ctx->CSSetShader(m_clearCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);

			ctx->CSSetShader(m_forwardDepthNpcOnlyCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);

			ctx->CSSetShader(m_forwardColorNpcOnlyCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);

			ID3D11UnorderedAccessView* nullOverlayUAVs[2] = {};
			ctx->CSSetUnorderedAccessViews(0, 2, nullOverlayUAVs, nullptr);

			ID3D11ShaderResourceView* overlaySRVs[] = { m_srvForwardNpcOutput[eye], m_srvAtomicDepth[eye] };
			ctx->CSSetShaderResources(0, 2, overlaySRVs);
			ID3D11UnorderedAccessView* compositeUAV[] = { m_uavOutput[eye] };
			ctx->CSSetUnorderedAccessViews(0, 1, compositeUAV, nullptr);
			ctx->CSSetShader(m_compositeNpcForwardCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);
		}
	}

	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ID3D11UnorderedAccessView* nullUAVs[2] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);

	ctx->Release();
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
		if (m_srvForwardNpcOutput[i]) {
			m_srvForwardNpcOutput[i]->Release();
			m_srvForwardNpcOutput[i] = nullptr;
		}
		if (m_uavForwardNpcOutput[i]) {
			m_uavForwardNpcOutput[i]->Release();
			m_uavForwardNpcOutput[i] = nullptr;
		}
		if (m_forwardNpcOutput[i]) {
			m_forwardNpcOutput[i]->Release();
			m_forwardNpcOutput[i] = nullptr;
		}
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
	if (m_forwardDepthCS) { m_forwardDepthCS->Release(); m_forwardDepthCS = nullptr; }
	if (m_forwardColorCS) { m_forwardColorCS->Release(); m_forwardColorCS = nullptr; }
	if (m_forwardDepthNpcOnlyCS) { m_forwardDepthNpcOnlyCS->Release(); m_forwardDepthNpcOnlyCS = nullptr; }
	if (m_forwardColorNpcOnlyCS) { m_forwardColorNpcOnlyCS->Release(); m_forwardColorNpcOnlyCS = nullptr; }
	if (m_compositeNpcForwardCS) { m_compositeNpcForwardCS->Release(); m_compositeNpcForwardCS = nullptr; }
	if (m_dilateCS) { m_dilateCS->Release(); m_dilateCS = nullptr; }
	if (m_npcDepthScatterCS) { m_npcDepthScatterCS->Release(); m_npcDepthScatterCS = nullptr; }

	for (int e = 0; e < 2; ++e) {
		if (m_srvAtomicDepth[e]) { m_srvAtomicDepth[e]->Release(); m_srvAtomicDepth[e] = nullptr; }
		if (m_uavAtomicDepth[e]) { m_uavAtomicDepth[e]->Release(); m_uavAtomicDepth[e] = nullptr; }
		if (m_atomicDepth[e]) { m_atomicDepth[e]->Release(); m_atomicDepth[e] = nullptr; }
	}


	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}

	m_eyeWidth = 0;
	m_eyeHeight = 0;
	OOVR_LOG("ASW: Shutdown");
}
