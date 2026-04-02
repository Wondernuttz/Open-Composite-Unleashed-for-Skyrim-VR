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
Texture2D<float4> frameNColor  : register(t3);  // frame N color (newer frame for disocclusion fill)
Texture2D<float>  frameNDepth  : register(t4);  // frame N depth (newer frame)
Texture2D<uint>   fpMask       : register(t5);  // R8_UINT FP mask (reserved)
RWTexture2D<float4> output     : register(u0);
RWTexture2D<uint> atomicDepth  : register(u1);  // forward scatter depth test buffer (R32_UINT)
RWTexture2D<uint> disocclusionMap : register(u2);  // 0=normal, 1=disoccluded (proactive marking)
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
    int debugMode;              // 0=normal, 60=depth scatter viz
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
    float rotMVScale;       // 1.0 = rotation in residual, 0.0 = rotation suppressed (stop transition)
    row_major float4x4 reprojClipToClip;  // warp output → frame N clip space (for disocclusion fill)
    int hasReprojData;      // 1 if frame N data is available for reprojection
    float3 _padReproj;
    column_major float4x4 fullWarpC2C;  // warp output → cached clip (head + locomotion, depth-aware)
    int hasFullWarpC2C;     // 1 if fullWarpC2C is valid
    float3 _padFullC2C;
    float4 controllerUV_LR; // xy=left hand UV, zw=right hand UV (cached frame)
    float controllerRadius; // screen-space radius in UV units for hand detection
    int hasControllerPose;  // 1 if controllerUV is valid
    float2 controllerDeltaL; // warp-time UV - cached-frame UV for left hand
    float2 controllerDeltaR; // warp-time UV - cached-frame UV for right hand
    float fpControllerScale; // controller delta multiplier (ini: aswFPControllerScale)
    int hasFPSphere;        // 1 if fpSphere* is valid
    float fpSphereRadius;   // FP bounding sphere radius
    float _padCtrl;
    float4 fpSphereCenter;  // xyz = center in rendering space, w unused
    int isMenuOpen;         // 1 if a gameplay menu is open (skip MV corrections)
    float3 _padPreFP;
    float4 fpScreenBoxes[16]; // Up to 16 AABBs: [minU, minV, maxU, maxV]
    int fpBoxCount;           // Number of valid screen-space FP bounding boxes
    float3 _padBoxes;
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

// First-person pixel classification: depth threshold + AABB bounding boxes.
// Returns true if this pixel is likely first-person geometry (hands/weapons).
// Uses depth < 0.87 (near camera) AND inside at least one FP bounding box AABB.
// Bounding boxes are projected from BSGeometry::worldBound each frame — they track
// weapon swings, bow draws, etc. Combining with depth eliminates distant world geometry
// that might fall inside a box (e.g., looking down from a cliff).
)" R"(
// IsFirstPerson: classifies pixel for MV skip (no locomotion/rotation correction).
// Uses depth + AABB bounding boxes — covers full weapons.
bool IsFirstPerson(float2 pixelUV, float depth) {
    if (depth >= 0.87)
        return false;
    if (fpBoxCount <= 0)
        return true; // Fallback: depth-only when no boxes available
    for (int i = 0; i < fpBoxCount && i < 16; i++) {
        float4 box = fpScreenBoxes[i]; // minU, minV, maxU, maxV
        if (pixelUV.x >= box.x && pixelUV.x <= box.z &&
            pixelUV.y >= box.y && pixelUV.y <= box.w)
            return true;
    }
    return false;
}

// GetControllerDelta: returns UV shift for controller motion prediction.
// Uses the nearest controller's FULL delta — no distance fade.
// Every FP pixel (hand, arm, weapon) shifts uniformly so the whole mesh
// moves as a rigid unit. Nearest-controller selection ensures left/right
// hand items track their respective controllers.
float2 GetControllerDelta(float2 pixelUV) {
    if (!hasControllerPose) return float2(0, 0);
    float distL = length(pixelUV - controllerUV_LR.xy);
    float distR = length(pixelUV - controllerUV_LR.zw);
    return fpControllerScale * ((distL < distR) ? controllerDeltaL : controllerDeltaR);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    float2 uv = ((float2)tid.xy + 0.5) / resolution;
    int2 pixel = (int2)tid.xy;
    int2 depthPixel = ToDepthCoord(pixel);

    // Read scattered depth at output pixel position (scatter targets output space).
    // With 1x1 scatter (no 2x2 splat), foreground depth can't bleed into background
    // positions — eliminating the trailing-edge contamination that caused sections 1 & 3.
    float d;
    uint scatteredDepth = atomicDepth[pixel];
    bool hasLoco = length(locoScreenDir) > 0.001;

    if (scatteredDepth != 0xFFFFFFFF) {
        d = (float)(scatteredDepth >> 16) / 65535.0;
    } else {
        d = depthTex[depthPixel];
    }

    float tanX = lerp(fovTanLeft, fovTanRight, uv.x);
    float tanY = lerp(fovTanUp,   fovTanDown,  uv.y);

    // Skip disocclusion detection near very close geometry (hands, weapons).
    // Check if any nearby SCATTER pixel has close depth — hands scatter with close
    // depth even when the gap pixel itself has no scatter. During movement, the
    // cached depth at the output position is background (hand shifted) → can't use
    // cached depth. The scatter buffer's close-depth neighbors reliably detect hands.
    bool isCloseGeometry = false;
    {
        int2 cOffsets[4] = { int2(-3,0), int2(3,0), int2(0,-3), int2(0,3) };
        for (int ci = 0; ci < 4; ci++) {
            int2 np = clamp(pixel + cOffsets[ci], int2(0,0), int2(resolution) - 1);
            uint nd = atomicDepth[np];
            if (nd != 0xFFFFFFFF && (float)(nd >> 16) / 65535.0 < 0.87) {
                isCloseGeometry = true;
                break;
            }
        }
    }

    // Disocclusion detection via forward-scatter gaps.
    // NPC animation disocclusion (simple gap detection, no nearGap complexity)
    // with special MVs that don't match world scatter, creating false gaps.
    if (!hasLoco && !isCloseGeometry && scatteredDepth == 0xFFFFFFFF) {
        // Check if gap is near very close geometry (hands) — skip if so
        float nearestD = 1.0;
        int gapN = 0;
        int2 cardinals[4] = { int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
        for (int ci = 0; ci < 4; ci++) {
            int2 np = pixel + cardinals[ci];
            if (np.x >= 0 && np.x < (int)resolution.x && np.y >= 0 && np.y < (int)resolution.y) {
                uint nd = atomicDepth[np];
                if (nd == 0xFFFFFFFF) { gapN++; }
                else { nearestD = min(nearestD, (float)(nd >> 16) / 65535.0); }
            }
        }
        if (gapN >= 2) {
            disocclusionMap[tid.xy] = 1;
            if (debugMode == 61) { output[tid.xy] = float4(1, 0, 0, 1); return; }
            return;
        }
    }

    // Locomotion disocclusion (full detection with nearGap contamination handling)
    if (hasLoco && !isCloseGeometry) {
        bool isGap = (scatteredDepth == 0xFFFFFFFF);
        bool adjacentToGap = false;

        // Check cardinal neighbors for gaps
        int gapNeighbors = 0;
        if (pixel.x > 0 && atomicDepth[pixel + int2(-1,0)] == 0xFFFFFFFF) gapNeighbors++;
        if (pixel.x < (int)resolution.x-1 && atomicDepth[pixel + int2(1,0)] == 0xFFFFFFFF) gapNeighbors++;
        if (pixel.y > 0 && atomicDepth[pixel + int2(0,-1)] == 0xFFFFFFFF) gapNeighbors++;
        if (pixel.y < (int)resolution.y-1 && atomicDepth[pixel + int2(0,1)] == 0xFFFFFFFF) gapNeighbors++;

        if (isGap && gapNeighbors >= 2) {
            // Real gap: 2+ gap neighbors confirms multi-pixel disocclusion zone
            disocclusionMap[tid.xy] = 1;
            if (debugMode == 61) { output[tid.xy] = float4(1, 0, 0, 1); return; }
            return;
        }

        if (!isGap && scatteredDepth != 0xFFFFFFFF && hasLoco) {
            float scatD = (float)(scatteredDepth >> 16) / 65535.0;
            // Use the foreground depth from the anti-loco side to set the threshold.
            float2 locoDirPx = normalize(locoScreenDir * resolution);
            float locoMaxComp = max(abs(locoDirPx.x), abs(locoDirPx.y));
            int2 antiLocoStep = -int2(round(locoDirPx / locoMaxComp));
            float fgRef = 0.0;
            for (int fs = 1; fs <= 30; fs++) {
                int2 fnp = pixel + antiLocoStep * fs;
                if (any(fnp < 0) || fnp.x >= (int)resolution.x || fnp.y >= (int)resolution.y) break;
                uint fnd = atomicDepth[fnp];
                if (fnd != 0xFFFFFFFF && disocclusionMap[fnp] == 0) {
                    fgRef = (float)(fnd >> 16) / 65535.0;
                    break;
                }
            }
            bool isBgScatter = (fgRef > 0.001 && scatD > fgRef + 0.02);

            // Dual-radius nearGap check:
            // FOREGROUND scatter: 1px — don't eat into objects.
            // BACKGROUND scatter (deeper than foreground + margin): 10px — catch the wide
            // contamination zone where background scatter filled positions but backward
            // warp will sample foreground from the cached frame.
            // Check 1px always (foreground near-gap)
            bool nearGap = false;
            if (pixel.x >= 1 && atomicDepth[pixel + int2(-1,0)] == 0xFFFFFFFF) nearGap = true;
            if (pixel.x < (int)resolution.x-1 && atomicDepth[pixel + int2(1,0)] == 0xFFFFFFFF) nearGap = true;
            if (pixel.y >= 1 && atomicDepth[pixel + int2(0,-1)] == 0xFFFFFFFF) nearGap = true;
            if (pixel.y < (int)resolution.y-1 && atomicDepth[pixel + int2(0,1)] == 0xFFFFFFFF) nearGap = true;
            // Extended check for background scatter (14px)
            if (!nearGap && isBgScatter) {
                for (int r = 2; r <= 14; r++) {
                    if (nearGap) break;
                    if (pixel.x >= r && atomicDepth[pixel + int2(-r,0)] == 0xFFFFFFFF) nearGap = true;
                    if (pixel.x < (int)resolution.x-r && atomicDepth[pixel + int2(r,0)] == 0xFFFFFFFF) nearGap = true;
                    if (pixel.y >= r && atomicDepth[pixel + int2(0,-r)] == 0xFFFFFFFF) nearGap = true;
                    if (pixel.y < (int)resolution.y-r && atomicDepth[pixel + int2(0,r)] == 0xFFFFFFFF) nearGap = true;
                }
            }

            if (nearGap) {
                disocclusionMap[tid.xy] = 1;
                if (debugMode == 61) {
                    output[tid.xy] = isBgScatter ? float4(0, 1, 0, 1) : float4(1, 1, 0, 1);
                    return; // GREEN=bg contamination, YELLOW=fg near-gap
                }
                return;
            }
        }
    }

    // Depth-mismatch catch: scatter says far (sky) but cached says close (branch/pole).
    // Catches thin features too small for gap detection (1-2px branches where the 2x2
    // scatter fills the gap entirely → no 0xFFFFFFFF → no gap detected).
    // Large threshold (0.05) is head-turn-stable: adjacent-pixel depth variation from
    // head turn is typically < 0.02, so 0.05 avoids false positives.
    if (hasLoco && scatteredDepth != 0xFFFFFFFF && disocclusionMap[tid.xy] == 0) {
        float scatD = (float)(scatteredDepth >> 16) / 65535.0;
        float cachedD = depthTex[depthPixel];
        if (cachedD < scatD - 0.05) {
            disocclusionMap[tid.xy] = 1;
            if (debugMode == 61) { output[tid.xy] = float4(0, 0, 1, 1); return; } // BLUE
            return;
        }
    }

    // Debug mode 62: scatter coverage visualization
    // GREEN=scatter hit, BLACK=no scatter (0xFFFFFFFF gap).
    // Gaps during locomotion = disocclusion zones.
    if (debugMode == 62) {
        if (scatteredDepth == 0xFFFFFFFF) {
            output[tid.xy] = float4(0, 0, 0, 1);
        } else {
            float sd = (float)(scatteredDepth >> 16) / 65535.0;
            output[tid.xy] = float4(0, 1.0 - sd, 0, 1);
        }
        return;
    }

    // Compute parallax direction from head pose delta.
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

    // Search direction: use parallax or raw game MV (whichever is larger).
    float2 roughMV = mvTex[ToMVCoord(pixel)] * mvPixelScale;
    roughMV = clamp(roughMV, float2(-0.15, -0.15), float2(0.15, 0.15));

    float2 searchDir = parallaxDir;
    float parallaxMagPx = length(parallaxDir * resolution);
    float mvSearchMagPx = length(roughMV * resolution);
    if (hasLoco && mvSearchMagPx > parallaxMagPx)
        searchDir = roughMV;
    float searchMagPx = hasLoco ? max(parallaxMagPx, mvSearchMagPx) : parallaxMagPx;

    float2 searchDirPx = searchDir * resolution;
    float maxComp = max(abs(searchDirPx.x), abs(searchDirPx.y));
    float2 stepVec = (maxComp > 0.001) ? (searchDirPx / maxComp) : float2(0, 0);

    int2 fgPixel = pixel;
    bool foundForeground = false;
    bool allowHeadDepthSearch = (!hasLoco && parallaxMagPx > 0.75 && d >= 0.97);

)" R"(
    // Foreground search: find closer depth along search direction.
    // During locomotion: short search (4px) to catch leading edges only — the tree that
    // moved into this position. Longer search causes "force fields" because it applies
    // foreground depth (and its larger loco offset) to distant background pixels.
    // Head-only mode: longer search for large parallax at sky depth.
    int fgSearchMax = 0;
    if (hasLoco && searchMagPx > 0.5)
        fgSearchMax = 4;  // leading edge only
    else if (allowHeadDepthSearch && searchMagPx > 0.5)
        fgSearchMax = min(48, max(24, (int)searchMagPx + 16));

    if (fgSearchMax > 0) {
        for (int step = 1; step <= fgSearchMax; step++) {
            int2 searchPx = pixel + int2(stepVec * (float)step);
            searchPx = clamp(searchPx, int2(0,0), int2(resolution) - 1);
            // Stop at disocclusion gaps — foreground on the other side has moved away.
            // At leading edges there are NO gaps between us and the foreground (it moved here).
            // At trailing edges there ARE gaps (it moved away) — must not reach across.
            if (atomicDepth[searchPx] == 0xFFFFFFFF) break;
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

    // Recompute parallax with foreground depth if found.
    bool useFgAngle = foundForeground;
    float2 fgUV = useFgAngle ? (float2(fgPixel) + 0.5) / resolution : uv;
    float fgTanX = useFgAngle ? lerp(fovTanLeft, fovTanRight, fgUV.x) : tanX;
    float fgTanY = useFgAngle ? lerp(fovTanUp, fovTanDown, fgUV.y) : tanY;
    linearDepth = LinearizeDepth(d, nearZ, farZ);
    scaledDepth = linearDepth * depthScale;
    newViewPos = float3(fgTanX * scaledDepth, fgTanY * scaledDepth, scaledDepth);
    transformed = mul(poseDeltaMatrix, float4(newViewPos, 1.0));
    float3 oldViewPos = transformed.xyz;

    float2 parallaxUV = uv;
    if (scaledDepth > 0.001 && oldViewPos.z > 0.001) {
        float oldTanX = oldViewPos.x / oldViewPos.z;
        float oldTanY = oldViewPos.y / oldViewPos.z;
        float2 cachedUV = float2(
            (oldTanX - fovTanLeft) / (fovTanRight - fovTanLeft),
            (oldTanY - fovTanUp) / (fovTanDown - fovTanUp));
        parallaxUV = uv + (cachedUV - fgUV);
    }

    // Near-field depth fade.
    float depthFade = (nearFadeDepth > 0.0) ? saturate((linearDepth - nearFadeDepth) / nearFadeDepth) : 1.0;
    float2 fadedParallax = (parallaxUV - uv) * depthFade;
    float2 parallaxSourceUV = uv + fadedParallax;

    // Read MV: prefer scatter source position (correct for leading edges where
    // the output pixel has foreground depth but the cached frame has background).
    // The scatter packed value encodes the source→dest offset in the lower 16 bits.
    int2 mvSourcePixel;
    if (foundForeground) {
        mvSourcePixel = ToMVCoord(fgPixel);
    } else if (scatteredDepth != 0xFFFFFFFF) {
        // Recover scatter source position from packed offset (relative to scatter write position)
        int sdx = (int)((scatteredDepth >> 8) & 0xFFu) - 127;
        int sdy = (int)(scatteredDepth & 0xFFu) - 127;
        int2 scatterSrc = clamp(pixel - int2(sdx, sdy), int2(0,0), int2(resolution) - 1);
        mvSourcePixel = ToMVCoord(scatterSrc);
    } else {
        mvSourcePixel = ToMVCoord(clamp(int2(parallaxSourceUV * resolution), int2(0,0), int2(resolution) - 1));
    }
    float2 rawMV = mvTex[mvSourcePixel];
    float2 totalMV = rawMV * mvPixelScale;
    totalMV = clamp(totalMV, float2(-0.15, -0.15), float2(0.15, 0.15));

)" R"(
    // headOnlyMV from headRotMatrix (OpenXR pose delta, NO stick rotation).
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

    // c2cHeadMV from clipToClipNoLoco (head rotation + stick rotation, no locomotion).
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

)" R"(
    // MV residual decomposition.
    // Skip ALL MV corrections for first-person geometry (hands/weapons).
    // Hands are camera-relative — they move with the head, not with the world.
    // The game MVs capture world motion (loco) but hands should use ONLY parallax
    // from poseDelta (head tracking). Applying loco MV to close pixels creates huge
    // offsets that shift sourceUV far away → wrong content → hands invisible.
    //
    // Detection: depth threshold + FP bounding box AABBs (projected from worldBound each frame).
    float2 pixelUV = (float2(tid.xy) + 0.5) / resolution;
    bool isWorldPixel = !IsFirstPerson(pixelUV, d);

    float2 mvOffset = float2(0, 0);
    float2 sourceUV = parallaxSourceUV;

    // For FP pixels: extract controller-only motion from game MVs.
    // FP is camera-relative → game MVs contain ONLY head tracking + controller motion
    // (no locomotion, no stick rotation). Subtract head-only motion (headRotMatrix)
    // to isolate pure controller tracking. Extrapolate 1.5x.
    if (!isWorldPixel && scatteredDepth != 0xFFFFFFFF) {
        // FP pixels: use forward scatter source color directly.
        // The scatter (CSDepthPreScatter) already computed the correct destination using
        // forwardPoseDelta (head+stick) + game MV residual (controller+bone). The packed
        // offset in atomicDepth encodes source→destination. Sampling prevColor at the
        // scatter source gives the exact cached content with zero MV subtraction errors.
        // No c2c, no head subtraction, no stick rotation issues.
        int sdx = (int)((scatteredDepth >> 8) & 0xFF) - 127;
        int sdy = (int)(scatteredDepth & 0xFF) - 127;
        int2 scatterSrc = pixel - int2(sdx, sdy);
        sourceUV = saturate((float2(scatterSrc) + 0.5) / resolution);
    }

    if (isWorldPixel && abs(mvConfidence) > 0.001 && !isMenuOpen) {
        float2 fullResidual = totalMV - headOnlyMV;
        float2 locoResidual = totalMV - c2cHeadMV;
        float2 rotComponent = fullResidual - locoResidual;
        float2 residual = locoResidual + rotMVScale * rotComponent;
        mvOffset = mvConfidence * residual;
        sourceUV = parallaxSourceUV + mvOffset;
    }

    // Depth-aware locomotion offset: replaces flat loco MV residual with depth-correct
    // parallax from the difference between c2c_with_loco and c2c_no_loco.
    if (isWorldPixel && !isMenuOpen && hasFullWarpC2C && hasClipToClipNoLoco && abs(mvConfidence) > 0.001) {
        float2 baseUV = useFgAngle ? fgUV : uv;
        float2 ndc = float2(baseUV.x * 2.0 - 1.0, 1.0 - baseUV.y * 2.0);
        float4 clipPos = float4(ndc, d, 1.0);

        float4 locoClip = mul(fullWarpC2C, clipPos);
        float4 noLocoClip = mul(clipToClipNoLoco, clipPos);

        if (abs(locoClip.w) > 0.0001 && abs(noLocoClip.w) > 0.0001) {
            float2 locoUV = float2(locoClip.x / locoClip.w * 0.5 + 0.5,
                                   0.5 - locoClip.y / locoClip.w * 0.5);
            float2 noLocoUV = float2(noLocoClip.x / noLocoClip.w * 0.5 + 0.5,
                                     0.5 - noLocoClip.y / noLocoClip.w * 0.5);

            float2 depthAwareLocoOffset = locoUV - noLocoUV;
            float2 rotComponent = c2cHeadMV - headOnlyMV;

            // NPC residual: totalMV includes NPC animation, c2cHeadMV captures head,
            // depthAwareLocoOffset captures camera locomotion. What remains = NPC motion.
            float2 npcMotion = totalMV - c2cHeadMV - depthAwareLocoOffset;
            float npcMotionMag = length(npcMotion * resolution);

            sourceUV = parallaxSourceUV + mvConfidence * (depthAwareLocoOffset + rotMVScale * rotComponent);
            // NPC motion correction: only apply when the residual is reasonable.
            // At background pixels near NPCs, the 2x2 depth scatter splat can bleed
            // foreground depth, causing depthAwareLocoOffset to be computed at WRONG
            // (foreground) depth — much larger than the actual background loco offset.
            // This creates a large spurious npcMotion residual (20-50+ px) that shifts
            // sourceUV to completely wrong positions, causing "warped background in NPC
            // outline shape" artifacts. Real NPC animation at 1.5x extrapolation is
            // rarely > 15px. Clamping eliminates the contamination artifacts.
            if (npcMotionMag < 15.0) {
                sourceUV += 1.5 * npcMotion;
            }
        }
    }

    // Clamp fallback.
    if (any(sourceUV < -0.01) || any(sourceUV > 1.01)) {
        sourceUV = uv + mvOffset;
        if (any(sourceUV < -0.01) || any(sourceUV > 1.01))
            sourceUV = uv;
    }

    // At scatter-covered pixels, override sourceUV with the scatter source position.
    // The scatter source directly encodes WHERE this pixel's content lives in the cached
    // frame — the actual position where the content was visible (not occluded).
    //
    // Backward warp sourceUV used for ALL pixels. No scatter-source override.
    // The override was tried but caused dilation/thickening at depth edges —
    // the 2x2 splat's integer-precision offsets and foreground-winning InterlockedMin
    // degraded visual quality across the entire scene during both locomotion and
    // head-tracking. Disocclusion zones are handled purely by gap detection + CSDilate.

    sourceUV = saturate(sourceUV);

    // Sample cached frame.
    float4 color = prevColor.SampleLevel(linearClamp, sourceUV, 0);

    // Debug mode 60: depth scatter visualization
    // RED = scattered foreground depth read from atomicDepth
    // GREEN = cached depth (no scatter hit at this pixel)
    // Brightness = depth value (darker = closer)
    if (debugMode == 60) {
        uint sd = atomicDepth[pixel];
        if (sd != 0xFFFFFFFF) {
            float scatD = (float)(sd >> 16) / 65535.0;
            color = float4(1.0 - scatD, 0, 0, 1); // RED, brighter = closer
        } else {
            float cachedD = depthTex[depthPixel];
            color = float4(0, 1.0 - cachedD, 0, 1); // GREEN
        }
    }

    // Debug mode 65: first-person bounding sphere detection.
    // Unproject pixel to rendering space, check distance to FP sphere center.
    // RED = inside FP sphere (player model), dimmed = world.
    if (debugMode == 65 && hasFPSphere) {
        // Unproject pixel to view/rendering space using depth + FOV tangents
        float linearD = LinearizeDepth(d, nearZ, farZ);
        float scaledD = linearD * depthScale;
        float3 viewPos = float3(tanX * scaledD, tanY * scaledD, scaledD);
        float dist = length(viewPos - fpSphereCenter.xyz);
        bool isFirstPerson = (dist < fpSphereRadius && d < 0.87);
        if (isFirstPerson)
            color = float4(1.0, color.g * 0.3, color.b * 0.3, 1); // RED = first-person
        else
            color = float4(color.rgb * 0.5, 1); // dimmed = world
    }

    // Debug mode 68: FP detection via depth + AABB bounding boxes.
    if (debugMode == 68) {
        float2 dbgUV = (float2(tid.xy) + 0.5) / resolution;
        if (IsFirstPerson(dbgUV, d))
            color = float4(1.0, color.g * 0.3, color.b * 0.3, 1); // RED = first-person
        else
            color = float4(color.rgb * 0.5, 1); // dimmed = world
    }

    // Debug mode 67: VR controller hand detection circles.
    if (debugMode == 67 && hasControllerPose) {
        float2 ctrls[2] = { controllerUV_LR.xy, controllerUV_LR.zw };
        for (int h = 0; h < 2; h++) {
            float rawDist = length(uv - ctrls[h]);
            bool inside = rawDist < controllerRadius;
            bool onRing = abs(rawDist - controllerRadius) < (2.0 / resolution.y);
            if (onRing)
                color = (h == 0) ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
            else if (inside && d < 0.87)
                color = float4(color.r * 0.5 + 0.25, color.g * 0.5 + 0.25, color.b * 0.5, 1);
            else if (inside)
                color = float4(color.r * 0.7, color.g * 0.7, color.b * 0.7 + 0.3, 1);
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
    disocclusionMap[tid.xy] = 0;       // clear disocclusion map (0=normal)
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

        // Subtract head rotation only — residual = stick + loco + animation from game MVs.
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
    float2 npcOffsetPx = -npcMV * mvConfidence * resolution;
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

    // Only process pixels marked as disoccluded by CSDepthPreScatter.
    // The disocclusionMap is proactively filled at trailing depth edges during scatter.
    if (disocclusionMap[tid.xy] == 0) return;

    // DEBUG: visualize disocclusion areas
    // Don't overwrite — CSMain already wrote the detection source color:
    // RED=gap, YELLOW=near-gap, GREEN=depth-mismatch
    if (debugMode == 61) {
        return; // keep CSMain's debug color
    }
    // DEBUG 64: solid cyan fill — confirms CSDilate processes ALL void pixels.
    // If any void pixel shows foreground instead of cyan, it's NOT being processed
    // by CSDilate (CSMain handled it instead — detection gap).
    if (debugMode == 64) {
        output[tid.xy] = float4(0, 1, 1, 1); // CYAN
        return;
    }

    // ── Loco-direction background extension with perpendicular smoothing ──
    // Walk in the LOCO direction past the gap to find actual background content.
    // Then sample a perpendicular strip at that position for smooth fill.
    //
    // Why loco-only: Isotropic blur samples from ALL directions including the
    // foreground edge, creating angled geometric artifacts where different parts
    // of the foreground silhouette contaminate the fill at different angles.
    // By ONLY extending from the loco side, the fill is pure background — the
    // same approach Oculus ASW uses ("stretching nearby background pixels").
    int2 pixel = (int2)tid.xy;
    bool hasLoco = length(locoScreenDir) > 0.001;

    {
        // 8-direction nearest-neighbor, background-preferring fill.
        // Works for BOTH locomotion disocclusion AND NPC animation disocclusion.
        // Phase 1: find nearest valid pixel + its depth in each direction.
        // Phase 2: find the max depth (most background) across all candidates.
        // Phase 3: average only candidates within 0.02 of max depth.
        //   - Trailing edges: max=sky/wall, foreground excluded → pure background fill
        //   - Inner gaps: max=NPC, all NPC candidates included → NPC fill
        int2 dirs8[8] = { int2(-1,0), int2(1,0), int2(0,-1), int2(0,1),
                          int2(-1,-1), int2(1,-1), int2(-1,1), int2(1,1) };
        int2 candPixel[8];
        float candDepth[8];
        int candDist[8];
        bool candFound[8];

        [unroll] for (int i = 0; i < 8; i++) {
            candFound[i] = false; candDepth[i] = 0; candDist[i] = 999;
        }

        [unroll] for (int dir = 0; dir < 8; dir++) {
            for (int step = 1; step <= 60; step++) {
                int2 np = pixel + dirs8[dir] * step;
                if (any(np < 0) || np.x >= (int)resolution.x || np.y >= (int)resolution.y) break;
                uint nd = atomicDepth[np];
                if (nd == 0xFFFFFFFF) continue;
                if (disocclusionMap[np] != 0) continue;
                candPixel[dir] = np;
                candDepth[dir] = (float)(nd >> 16) / 65535.0;
                candDist[dir] = step;
                candFound[dir] = true;
                break;
            }
        }

        // Find max depth (background)
        float maxD = 0;
        [unroll] for (int i = 0; i < 8; i++) {
            if (candFound[i] && candDepth[i] > maxD) maxD = candDepth[i];
        }

        // Find foreground depth to set the background threshold.
        // Use the SHALLOWEST (closest) candidate from the 8-direction search as fgRef.
        // During loco: this is the NPC/tree that created the trailing edge.
        // During NPC animation: this is the NPC body part that moved.
        float fgRef = 0.0;
        [unroll] for (int i = 0; i < 8; i++) {
            if (candFound[i] && (fgRef < 0.001 || candDepth[i] < fgRef)) {
                fgRef = candDepth[i];
            }
        }
        float bgThreshold = fgRef + 0.02;

        // Find best background candidate.
        // When fgRef found: closest candidate deeper than fgRef+0.02 (contextual bg).
        // When fgRef NOT found (all-void anti-loco): deepest candidate (avoids
        // picking nearby branch over distant sky when threshold is meaningless).
        int bestDir = -1;
        if (fgRef > 0.001) {
            // Closest background deeper than foreground
            int bestDist = 999;
            [unroll] for (int i = 0; i < 8; i++) {
                if (candFound[i] && candDepth[i] > bgThreshold && candDist[i] < bestDist) {
                    bestDist = candDist[i];
                    bestDir = i;
                }
            }
        }
        // Fallback: deepest overall (also used when fgRef not found)
        if (bestDir < 0) {
            [unroll] for (int i = 0; i < 8; i++) {
                if (candFound[i] && (bestDir < 0 || candDepth[i] > candDepth[bestDir]))
                    bestDir = i;
            }
        }

        // Single scatter-source pixel fill — clean background color, no foreground.
        if (bestDir >= 0) {
            uint cnd = atomicDepth[candPixel[bestDir]];
            int cdx = (int)((cnd >> 8) & 0xFFu) - 127;
            int cdy = (int)(cnd & 0xFFu) - 127;
            int2 src = clamp(candPixel[bestDir] - int2(cdx, cdy), int2(0,0), int2(resolution) - 1);
            float2 srcUV = (float2(src) + 0.5) / resolution;
            output[tid.xy] = prevColor.SampleLevel(linearClamp, srcUV, 0);
            return;
        }
    }

    // Final fallback
    output[tid.xy] = float4(0, 0, 0, 1);
}

// ── Post-fill blur: smooth ONLY the void-zone pixels among themselves ──
// Dispatched AFTER CSDilate. Reads the filled output values and averages
// each void pixel with its void-pixel neighbors. Non-void pixels are excluded.
// This adds natural blur without introducing any foreground contamination.
[numthreads(8, 8, 1)]
void CSBlurVoids(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y) return;
    // Process void pixels AND pixels 1-2px outside the void boundary.
    // The border pixels (non-void, next to void) may have dark foreground
    // from CSMain's backward warp — blurring smooths them out.
    if (disocclusionMap[tid.xy] == 0) {
        bool nearVoid = false;
        [unroll] for (int r = 1; r <= 2 && !nearVoid; r++) {
            int2 np;
            np = (int2)tid.xy + int2(-r, 0); if (np.x >= 0 && disocclusionMap[np] != 0) nearVoid = true;
            np = (int2)tid.xy + int2(r, 0); if (np.x < (int)resolution.x && disocclusionMap[np] != 0) nearVoid = true;
            np = (int2)tid.xy + int2(0, -r); if (np.y >= 0 && disocclusionMap[np] != 0) nearVoid = true;
            np = (int2)tid.xy + int2(0, r); if (np.y < (int)resolution.y && disocclusionMap[np] != 0) nearVoid = true;
        }
        if (!nearVoid) return;
    }

    // Void neighbors: always included.
    // Non-void neighbors: only if background scatter depth (deeper than foreground).
    // This smooths the void↔background border without pulling in foreground.
    float4 blurred = float4(0, 0, 0, 0);
    float weight = 0;
    [unroll] for (int dy = -6; dy <= 6; dy++) {
        [unroll] for (int dx = -6; dx <= 6; dx++) {
            if (dx*dx + dy*dy > 36) continue; // circular kernel
            int2 np = (int2)tid.xy + int2(dx, dy);
            if (any(np < 0) || np.x >= (int)resolution.x || np.y >= (int)resolution.y)
                continue;
            if (disocclusionMap[np] == 0) {
                // Non-void: only include if background scatter depth
                uint nd = atomicDepth[np];
                if (nd == 0xFFFFFFFF) continue;
                float ndd = (float)(nd >> 16) / 65535.0;
                if (ndd < 0.95) continue; // foreground — skip
            }
            float w = exp(-(float)(dx*dx + dy*dy) / 24.0);
            blurred += output[np] * w;
            weight += w;
        }
    }
    if (weight > 0.001)
        output[tid.xy] = blurred / weight;
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

// Hand composite: overlay re-rendered FP hand pixels onto warp output.
// Hand RT was cleared to depth=1.0 — any pixel with depth < 0.999 is a hand pixel.
// Controller delta shifts the hand RT sampling to the warp-time controller position.
// Since the hand RT has a transparent background, trailing-edge gaps just show through
// to the warp output beneath — no disocclusion artifacts.
static const char* s_handCompositeHLSL = R"(
Texture2D<float4> handColor : register(t0);
Texture2D<float>  handDepth : register(t1);
RWTexture2D<float4> output  : register(u0);

cbuffer HandCompositeCB : register(b0) {
    float2 controllerDeltaL;  // UV shift for left hand (warp_UV - cached_UV)
    float2 controllerDeltaR;  // UV shift for right hand
    float2 controllerUV_L;    // cached-frame left controller UV
    float2 controllerUV_R;    // cached-frame right controller UV
    float  controllerRadius;  // hand detection radius in UV
    float2 resolution;        // output resolution
    float  _pad;
};

[numthreads(8, 8, 1)]
void CSHandComposite(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    float2 uv = (float2(tid.xy) + 0.5) / resolution;

    // Per-pixel controller delta: blend between L/R based on proximity
    float distL = length(uv - (controllerUV_L + controllerDeltaL));
    float distR = length(uv - (controllerUV_R + controllerDeltaR));
    float2 delta = (distL < distR) ? controllerDeltaL : controllerDeltaR;

    // Source pixel in hand RT (shift backward by controller delta)
    int2 srcPx = int2((uv - delta) * resolution);
    if (srcPx.x < 0 || srcPx.y < 0 || srcPx.x >= (int)resolution.x || srcPx.y >= (int)resolution.y)
        return;

    float hd = handDepth[srcPx];
    if (hd < 0.999) {
        output[tid.xy] = handColor[srcPx];
    }
}
)";

static const char* s_npcScatterHLSL = R"(
// ── CSDepthPreScatter: scatter ALL cached pixels' depths to their WARP OUTPUT positions ──
// Uses forwardPoseDelta (cached→warp head delta) + MV residual (loco/stick/NPC)
// to place scatter in WARP OUTPUT space. The old approach (srcUV - totalMV) placed
// scatter in game-frame space, misaligned with the output by the cached→warp head
// delta — causing the scatter buffer to shift ~7-14px with head turns.
[numthreads(8, 8, 1)]
void CSDepthPreScatter(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    int2 srcPixel = (int2)tid.xy;
    float d = depthTex[ToDepthCoord(srcPixel)];
    // Sky pixels ARE scattered (not skipped) so that sky areas fill atomicDepth
    // with valid depth values. Without this, sky positions remain 0xFFFFFFFF and
    // are falsely detected as disocclusion gaps. Only true voids (behind foreground
    // where nothing was rendered in the cached frame) should remain as 0xFFFFFFFF.

    float2 srcUV = (float2(srcPixel) + 0.5) / resolution;

    // Step 1: Forward transform via forwardPoseDelta (cached view → warp view).
    // This handles the head tracking delta from cached pose to warp predicted pose.
    float linearDepth = LinearizeDepth(d, nearZ, farZ);
    float scaledDepth = linearDepth * depthScale;
    float srcTanX = lerp(fovTanLeft, fovTanRight, srcUV.x);
    float srcTanY = lerp(fovTanUp, fovTanDown, srcUV.y);
    float3 viewPos = float3(srcTanX * scaledDepth, srcTanY * scaledDepth, scaledDepth);
    float4 xformed = mul(forwardPoseDelta, float4(viewPos, 1.0));
    float2 dstUV = srcUV;
    if (scaledDepth > 0.001 && xformed.z > 0.001) {
        dstUV = float2(
            (xformed.x / xformed.z - fovTanLeft) / (fovTanRight - fovTanLeft),
            (xformed.y / xformed.z - fovTanUp) / (fovTanDown - fovTanUp));
    }
    float depthFade = (nearFadeDepth > 0.0) ? saturate((linearDepth - nearFadeDepth) / nearFadeDepth) : 1.0;
    dstUV = lerp(srcUV, dstUV, depthFade);

    // Step 2: Add MV residual for scatter destination.
    // FP pixels: per-pixel game MV residual (totalMV - headOnlyMV) captures controller
    // motion + bone animation. Scatter to predicted position so CSMain has depth coverage.
    // World pixels: loco + stick rotation + NPC animation as before.
    bool isFPScatter = IsFirstPerson(srcUV, d);
    if (abs(mvConfidence) > 0.001 && !isMenuOpen) {
        float2 rawMV = mvTex[ToMVCoord(srcPixel)];
        float2 totalMV = clamp(rawMV * mvPixelScale, float2(-0.15, -0.15), float2(0.15, 0.15));

        float4 rotated = mul(headRotMatrix, float4(viewPos, 1.0));
        float2 headOnlyMV = float2(0, 0);
        if (scaledDepth > 0.001 && rotated.z > 0.001) {
            headOnlyMV = float2(
                (rotated.x / rotated.z - fovTanLeft) / (fovTanRight - fovTanLeft),
                (rotated.y / rotated.z - fovTanUp) / (fovTanDown - fovTanUp)) - srcUV;
        }
        if (isFPScatter) {
            float2 fpResidual = totalMV - headOnlyMV;
            dstUV -= 1.5 * fpResidual;
        } else {
            dstUV -= mvConfidence * (totalMV - headOnlyMV);
        }

    }

    float2 dstPx = dstUV * resolution;
    uint depth16 = (uint)round(saturate(d) * 65535.0);

    // 2x2 bilinear splat: provides full scatter coverage on surfaces with non-uniform MVs
    // (NPC animation, cloth folds). 1x1 scatter created systematic gaps on NPC surfaces
    // that were falsely detected as disocclusion → grid lines filled with background.
    // The 2x2 splat's foreground contamination at gap edges is handled by flagging
    // gap-adjacent pixels for CSDilate fill (adjacentToGap check in CSMain).
    // The scatter-source color override in CSMain handles non-adjacent pixels correctly.
    int2 p0 = int2(floor(dstPx - 0.5));
    for (int sy = 0; sy <= 1; sy++) {
        for (int sx = 0; sx <= 1; sx++) {
            int2 p = p0 + int2(sx, sy);
            if (all(p >= 0) && p.x < (int)resolution.x && p.y < (int)resolution.y) {
                int dx = clamp(p.x - srcPixel.x, -127, 127);
                int dy = clamp(p.y - srcPixel.y, -127, 127);
                uint packed = (depth16 << 16) |
                    ((uint)clamp(dx + 127, 0, 255) << 8) |
                    (uint)clamp(dy + 127, 0, 255);
                InterlockedMin(atomicDepth[p], packed);
            }
        }
    }
}

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
    float2 npcOffsetPx = -npcMV * mvConfidence * resolution;
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

bool ASWProvider::Initialize(ID3D11Device* device, uint32_t renderWidth, uint32_t renderHeight,
    uint32_t outputWidth, uint32_t outputHeight)
{
	if (m_ready || m_compileStarted)
		return true;
	if (!device || renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
		return false;

	OOVR_LOGF("ASW: Initializing — render %ux%u, output %ux%u", renderWidth, renderHeight, outputWidth, outputHeight);

	m_renderWidth = renderWidth;
	m_renderHeight = renderHeight;
	m_outputWidth = outputWidth;
	m_outputHeight = outputHeight;
	m_device = device;
	m_device->AddRef(); // prevent device destruction while ASW holds a reference

	// Create textures and swapchains first (fast D3D11 calls, <100ms)
	if (!CreateStagingTextures(device)) {
		OOVR_LOG("ASW: Failed to create staging textures");
		Shutdown();
		return false;
	}

	if (!CreateOutputSwapchain(m_outputWidth * 2, m_outputHeight)) {
		OOVR_LOG("ASW: Failed to create output swapchain");
		Shutdown();
		return false;
	}

	if (!CreateDepthSwapchain(m_outputWidth * 2, m_outputHeight)) {
		OOVR_LOG("ASW: Failed to create depth swapchain (non-fatal — depth layer disabled)");
	}

	// Launch shader compilation in background — game continues rendering without ASW.
	// TryFinishShaderCompilation() will poll for completion and set m_ready = true.
	if (!LaunchAsyncShaderCompilation()) {
		OOVR_LOG("ASW: Failed to launch shader compilation");
		Shutdown();
		return false;
	}

	m_publishedSlot.store(-1, std::memory_order_relaxed);
	m_previousPublishedSlot.store(-1, std::memory_order_relaxed);
	m_warpReadSlot.store(-1, std::memory_order_relaxed);
	m_buildSlot = 0;
	m_buildEyeReady[0] = false;
	m_buildEyeReady[1] = false;
	m_hasLastPublishedPose = false;
	m_frameCounter = 0;

	OOVR_LOGF("ASW: Resources created — render %ux%u, output %ux%u per eye, shaders compiling in background",
	    m_renderWidth, m_renderHeight, m_outputWidth, m_outputHeight);
	return true; // m_ready stays false until shaders are done
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

bool ASWProvider::LaunchAsyncShaderCompilation()
{
	// Build shader source strings (copied into the lambda captures for thread safety).
	std::string csMainSrc     = std::string(s_warpShaderHLSL);
	std::string fullShaderSrc = csMainSrc + s_forwardScatterHLSL;
	std::string npcShaderSrc  = csMainSrc + s_npcScatterHLSL;
	std::string compositeStr  = std::string(s_npcForwardCompositeHLSL);
	std::string handCompStr   = std::string(s_handCompositeHLSL);

	DWORD flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
#endif

	// Set up pending blob slots — one per shader entry point.
	// Entry point names are string literals (static lifetime), safe to capture by pointer.
	static const char* entryPoints[] = {
		"CSMain", "CSClear", "CSForwardDepth", "CSForwardColor",
		"CSForwardDepthNpcOnly", "CSForwardColorNpcOnly", "CSDilate",
		"CSCompositeNpcForward", "CSNpcDepthScatter", "CSDepthPreScatter", "CSBlurVoids",
		"CSHandComposite"
	};
	constexpr int kJobCount = _countof(entryPoints);
	m_pendingBlobs.resize(kJobCount);
	for (int i = 0; i < kJobCount; i++) {
		m_pendingBlobs[i].entry = entryPoints[i];
		m_pendingBlobs[i].blob = nullptr;
		m_pendingBlobs[i].ok = false;
	}

	// Source string to use for each entry point (by index)
	// 0=csMainSrc, 1-6=fullShaderSrc, 7=compositeStr, 8-9=npcShaderSrc, 10=fullShaderSrc, 11=handCompStr
	struct SourceMap { int blobIdx; const std::string* src; };
	SourceMap srcMap[] = {
		{0, &csMainSrc}, {1, &fullShaderSrc}, {2, &fullShaderSrc}, {3, &fullShaderSrc},
		{4, &fullShaderSrc}, {5, &fullShaderSrc}, {6, &fullShaderSrc},
		{7, &compositeStr}, {8, &npcShaderSrc}, {9, &npcShaderSrc}, {10, &fullShaderSrc},
		{11, &handCompStr}
	};

	// Launch a single background thread that compiles all shaders in parallel internally.
	// D3DCompile is thread-safe. The blobs are written to m_pendingBlobs which is only
	// read by TryFinishShaderCompilation() after the future completes.
	m_compileFuture = std::async(std::launch::async,
		[this, csMainSrc = std::move(csMainSrc),
		 fullShaderSrc = std::move(fullShaderSrc),
		 npcShaderSrc = std::move(npcShaderSrc),
		 compositeStr = std::move(compositeStr),
		 handCompStr = std::move(handCompStr),
		 flags]() {
		const std::string* sources[] = {
			&csMainSrc, &fullShaderSrc, &fullShaderSrc, &fullShaderSrc,
			&fullShaderSrc, &fullShaderSrc, &fullShaderSrc,
			&compositeStr, &npcShaderSrc, &npcShaderSrc, &fullShaderSrc,
			&handCompStr
		};
		constexpr int N = 12;

		// Launch parallel sub-tasks for each shader
		std::vector<std::future<void>> futures;
		futures.reserve(N);
		for (int i = 0; i < N; i++) {
			futures.push_back(std::async(std::launch::async,
				[this, i, sources, flags]() {
				m_pendingBlobs[i].ok = CompileOrLoadCached(
				    *sources[i], m_pendingBlobs[i].entry, "cs_5_0", flags,
				    &m_pendingBlobs[i].blob, nullptr);
			}));
		}
		for (auto& f : futures)
			f.get();

		OOVR_LOG("ASW: Background shader compilation complete");
	});

	m_compileStarted = true;
	OOVR_LOG("ASW: Shader compilation launched in background");
	return true;
}

void ASWProvider::TryFinishShaderCompilation()
{
	if (m_ready || !m_compileStarted)
		return;

	// Non-blocking check: are all shaders compiled?
	if (m_compileFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		return;

	// Harvest the future (no-op since compilation wrote directly to m_pendingBlobs)
	m_compileFuture.get();

	// Create compute shaders serially on the game thread (D3D11 device is not thread-safe)
	ID3D11ComputeShader** targets[] = {
	    &m_warpCS,               // [0] CSMain
	    &m_clearCS, &m_forwardDepthCS, &m_forwardColorCS,
	    &m_forwardDepthNpcOnlyCS, &m_forwardColorNpcOnlyCS, &m_dilateCS,
	    &m_compositeNpcForwardCS, &m_npcDepthScatterCS, &m_depthPreScatterCS, &m_blurVoidsCS,
	    &m_handCompositeCS,      // [11] CSHandComposite
	};
	const int kJobCount = (int)m_pendingBlobs.size();
	bool csMainOk = false;
	bool allForwardOk = true;
	for (int i = 0; i < kJobCount; i++) {
		if (!m_pendingBlobs[i].ok) {
			OOVR_LOGF("ASW: %s compilation failed%s", m_pendingBlobs[i].entry,
			    i == 0 ? " (FATAL)" : " (non-fatal)");
			if (i == 0) { m_compileStarted = false; return; } // CSMain required
			if (i >= 1 && i <= 6) allForwardOk = false;
			continue;
		}
		HRESULT r = m_device->CreateComputeShader(
		    m_pendingBlobs[i].blob->GetBufferPointer(), m_pendingBlobs[i].blob->GetBufferSize(),
		    nullptr, targets[i]);
		m_pendingBlobs[i].blob->Release();
		m_pendingBlobs[i].blob = nullptr;
		if (FAILED(r)) {
			OOVR_LOGF("ASW: CreateComputeShader(%s) failed hr=0x%08x", m_pendingBlobs[i].entry, (unsigned)r);
			if (i == 0) { m_compileStarted = false; return; }
			if (i >= 1 && i <= 6) allForwardOk = false;
		} else if (i == 0) {
			csMainOk = true;
		}
	}
	m_pendingBlobs.clear();

	if (allForwardOk) {
		OOVR_LOG("ASW: Forward scatter shaders compiled");
	} else {
		OOVR_LOG("ASW: Some forward scatter shaders failed (non-fatal — backward warp still available)");
	}

	// Constant buffer and sampler
	D3D11_BUFFER_DESC cbDesc = {};
	cbDesc.ByteWidth = (sizeof(WarpConstants) + 15) & ~15;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateBuffer (CB) failed hr=0x%08x", (unsigned)hr);
		m_compileStarted = false;
		return;
	}

	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateSamplerState failed hr=0x%08x", (unsigned)hr);
		m_compileStarted = false;
		return;
	}

	// Create hand render targets + replay CBs for FP hand re-rendering
	if (!CreateHandRenderTargets(m_device, m_renderWidth, m_renderHeight)) {
		OOVR_LOG("ASW: Hand render targets failed (non-fatal — FP replay disabled)");
	}

	m_ready = true;
	OOVR_LOG("ASW: Shaders compiled and ready — ASW active");
}

bool ASWProvider::CreateHandRenderTargets(ID3D11Device* device, uint32_t w, uint32_t h)
{
	HRESULT hr;

	// Per-eye hand color RT (RGBA8)
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	D3D11_TEXTURE2D_DESC dd = {};
	dd.Width = w; dd.Height = h;
	dd.MipLevels = 1; dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_R32_TYPELESS; // TYPELESS allows both D32_FLOAT DSV and R32_FLOAT SRV
	dd.SampleDesc.Count = 1; dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

	for (int eye = 0; eye < 2; eye++) {
		hr = device->CreateTexture2D(&td, nullptr, &m_handColor[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand color RT failed hr=0x%08x", (unsigned)hr); return false; }

		D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
		rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		hr = device->CreateRenderTargetView(m_handColor[eye], &rtvd, &m_handRTV[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand RTV failed hr=0x%08x", (unsigned)hr); return false; }

		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_handColor[eye], &srvd, &m_srvHandColor[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand color SRV failed hr=0x%08x", (unsigned)hr); return false; }

		hr = device->CreateTexture2D(&dd, nullptr, &m_handDepthTex[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand depth tex failed hr=0x%08x", (unsigned)hr); return false; }

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
		dsvd.Format = DXGI_FORMAT_D32_FLOAT;
		dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		hr = device->CreateDepthStencilView(m_handDepthTex[eye], &dsvd, &m_handDSV[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand DSV failed hr=0x%08x", (unsigned)hr); return false; }

		// SRV on D32_FLOAT (read as R32_FLOAT)
		D3D11_SHADER_RESOURCE_VIEW_DESC dsrvd = {};
		dsrvd.Format = DXGI_FORMAT_R32_FLOAT;
		dsrvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		dsrvd.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_handDepthTex[eye], &dsrvd, &m_srvHandDepth[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: hand depth SRV failed hr=0x%08x", (unsigned)hr); return false; }
	}

	// Depth stencil state for hand replay: LESS_EQUAL, depth write enabled
	D3D11_DEPTH_STENCIL_DESC dssd = {};
	dssd.DepthEnable = TRUE;
	dssd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dssd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	dssd.StencilEnable = FALSE;
	hr = device->CreateDepthStencilState(&dssd, &m_handDSS);
	if (FAILED(hr)) { OOVR_LOGF("ASW: hand DSS failed hr=0x%08x", (unsigned)hr); return false; }

	// Pre-allocate replay constant buffers
	for (int i = 0; i < 7; i++) {
		D3D11_BUFFER_DESC cbd = {};
		cbd.ByteWidth = (kReplayCBSizes[i] + 15) & ~15; // 16-byte aligned
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = device->CreateBuffer(&cbd, nullptr, &m_replayCB[i]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: replay CB[%d] (slot %d, %u bytes) failed hr=0x%08x",
				i, kReplayCBSlots[i], kReplayCBSizes[i], (unsigned)hr);
			return false;
		}
	}
	// PS replay CBs (256 bytes each)
	for (int i = 0; i < 3; i++) {
		D3D11_BUFFER_DESC cbd = {};
		cbd.ByteWidth = 256;
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = device->CreateBuffer(&cbd, nullptr, &m_replayPSCB[i]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: replay PSCB[%d] failed hr=0x%08x", i, (unsigned)hr);
			return false;
		}
	}

	// Hand composite CB
	{
		D3D11_BUFFER_DESC cbd = {};
		cbd.ByteWidth = (sizeof(HandCompositeConstants) + 15) & ~15;
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = device->CreateBuffer(&cbd, nullptr, &m_handCompositeCB);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: hand composite CB failed hr=0x%08x", (unsigned)hr);
			return false;
		}
	}

	OOVR_LOGF("ASW: Hand render targets created (%ux%u per eye)", w, h);
	return true;
}

bool ASWProvider::CreateStagingTextures(ID3D11Device* device)
{
	// Cache and warp textures use render resolution.
	// Output swapchain uses output resolution (display-res when upscaler active).
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = m_renderWidth;
	desc.Height = m_renderHeight;
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

			// Cached stencil (R8_UINT — extracted from R24G8 depth-stencil)
			desc.Format = DXGI_FORMAT_R8_UINT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = 0;
			hr = device->CreateTexture2D(&desc, nullptr, &m_cachedStencil[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateTexture2D stencil[%u][%d] R8_UINT failed hr=0x%08X", slot, eye, hr);
				return false;
			}
			srvDesc.Format = DXGI_FORMAT_R8_UINT;
			hr = device->CreateShaderResourceView(m_cachedStencil[slot][eye], &srvDesc, &m_srvStencil[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateSRV stencil[%u][%d] failed", slot, eye);
				return false;
			}

			// Cached FP mask (R8_UINT from SKSE DS→DS depth comparison)
			desc.Format = DXGI_FORMAT_R8_UINT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hr = device->CreateTexture2D(&desc, nullptr, &m_cachedPreFPDepth[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateTexture2D fpMask[%u][%d] failed hr=0x%08X", slot, eye, hr);
				return false;
			}
			srvDesc.Format = DXGI_FORMAT_R8_UINT;
			hr = device->CreateShaderResourceView(m_cachedPreFPDepth[slot][eye], &srvDesc, &m_srvPreFPDepth[slot][eye]);
			if (FAILED(hr)) {
				OOVR_LOGF("ASW: CreateSRV fpMask[%u][%d] failed", slot, eye);
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

	// Forward map: R32_UINT per source pixel, stores scatter destination (x16|y16)
	for (int eye = 0; eye < 2; eye++) {
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_forwardMap[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateTexture2D forwardMap[%d] failed hr=0x%08X", eye, hr);
			return false;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32_UINT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(m_forwardMap[eye], &uavDesc, &m_uavForwardMap[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateUAV forwardMap[%d] failed", eye);
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R32_UINT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_forwardMap[eye], &srvDesc, &m_srvForwardMap[eye]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: CreateSRV forwardMap[%d] failed", eye);
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
	rect.offset.x = eye * (int32_t)m_outputWidth;
	rect.offset.y = 0;
	rect.extent.width = (int32_t)m_outputWidth;
	rect.extent.height = (int32_t)m_outputHeight;
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

void ASWProvider::CachePreFPDepth(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* preFPR32F, const D3D11_BOX* region)
{
	if (!m_ready || eye < 0 || eye > 1 || !preFPR32F || !region) return;
	const int slot = m_buildSlot;
	if (!m_cachedPreFPDepth[slot][eye]) return;

	if (!SafeBridgeCopy(ctx, m_cachedPreFPDepth[slot][eye], 0, 0, 0, 0,
	        preFPR32F, 0, region)) {
		OOVR_LOG("ASW: TOCTOU - preFPDepth texture freed during copy");
	}
}

void ASWProvider::CacheFrame(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
    const XrPosef& eyePose, const XrFovf& eyeFov,
    float nearZ, float farZ)
{
	// Poll for async shader compilation completion (non-blocking)
	if (!m_ready && m_compileStarted)
		TryFinishShaderCompilation();
	if (!m_ready || eye < 0 || eye > 1)
		return;
	const int slot = m_buildSlot;
	if (eye == 0) {
		// Default dimensions when optional MV/depth copies are unavailable.
		m_slotMVDataW[slot] = m_renderWidth;
		m_slotMVDataH[slot] = m_renderHeight;
		m_slotDepthDataW[slot] = m_renderWidth;
		m_slotDepthDataH[slot] = m_renderHeight;
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

void ASWProvider::CacheStencil(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* stencilTex, const D3D11_BOX* stencilRegion)
{
	if (!m_ready || eye < 0 || eye > 1 || !stencilTex || !stencilRegion)
		return;
	// CacheStencil is called AFTER CacheFrame. For eye 0, the build slot hasn't
	// advanced yet (CacheFrame only advances on eye 1). For eye 1, we've already
	// advanced — use the PREVIOUS slot (which is the just-published slot).
	int slot = (eye == 1)
	    ? m_publishedSlot.load(std::memory_order_acquire)
	    : m_buildSlot;
	if (slot < 0 || slot >= (int)kAswCacheSlotCount)
		return;
	if (!SafeBridgeCopy(ctx, m_cachedStencil[slot][eye], 0, 0, 0, 0,
	        stencilTex, 0, stencilRegion)) {
		static int s_count = 0;
		if (s_count++ < 3)
			OOVR_LOG("ASW: stencil copy failed");
	}
}

// ── FP Hand Replay: re-render captured FP draws with updated View matrix ──
// Called per-eye from WarpFrame after the main warp dispatches.
// Renders to m_handColor/m_handDepth, then composites onto m_warpedOutput.
bool ASWProvider::ReplayFPHands(int eye, ID3D11DeviceContext* ctx,
    const float* forwardPoseDelta, const HandCompositeConstants& hcc)
{
	if (!m_fpReplayPtr || !m_handRTV[eye] || !m_handDSV[eye] || !m_handCompositeCS)
		return false;

	int readIdx = m_fpReplayPtr->readyIdx.load(std::memory_order_acquire);
	if (readIdx < 0 || readIdx >= FPReplayData::NUM_BUFS)
		return false;

	auto& cs = m_fpReplayPtr->sets[readIdx];
	if (cs.drawCount <= 0 || !cs.hasCB12)
		return false;

	static int s_replayLog = 0;
	bool doLog = (s_replayLog < 20 && eye == 0);

	// Build patched CB[12]: overwrite CameraView[0] with new View for this eye.
	//
	// CB[12] (FrameBufferVR) layout — column-major float4x3 (4 rows × 3 cols = 48 bytes each):
	//   CameraView[0]: bytes 0-47   (12 floats) — column-major 4x3
	//   CameraView[1]: bytes 48-95  (12 floats)
	//   CameraProj[0]: bytes 96-143 (12 floats)
	//   CameraProj[1]: bytes 144-191 (12 floats)
	//
	// Column-major float4x3 memory layout (3 columns of float4):
	//   Col 0: [R00, R10, R20, Tx]  — rotation column 0 + translation X
	//   Col 1: [R01, R11, R21, Ty]  — rotation column 1 + translation Y
	//   Col 2: [R02, R12, R22, Tz]  — rotation column 2 + translation Z
	//
	// Row-vector shader convention: viewPos = mul(float4(worldPos,1), CameraView)
	// = float3(dot(col0, wp4), dot(col1, wp4), dot(col2, wp4))
	static constexpr int kViewBytes = 48;   // 12 floats per View matrix
	static constexpr int kProjOffset = 96;  // CameraProj[0] starts at byte 96

	uint8_t patchedCB12[1408];
	memcpy(patchedCB12, cs.cb12, 1408);

	const float* cachedView = reinterpret_cast<const float*>(&cs.cb12[eye * kViewBytes]);
	const float* cachedProj = reinterpret_cast<const float*>(&cs.cb12[kProjOffset + eye * kViewBytes]);

	// Apply forwardPoseDelta to compute newView for warp-time head pose.
	//
	// forwardPoseDelta (4×4 row-major) transforms cached eye → new eye:
	//   p_new = delta * p_cached  (column-vector, our internal convention)
	//
	// CameraView is column-major float4x3 used with row-vector mul:
	//   viewPos = mul(float4(worldPos, 1), CameraView)
	//   = float3(dot(col0, wp4), dot(col1, wp4), dot(col2, wp4))
	//
	// The View matrix V maps world → eye.  We want V_new = delta * V_cached.
	// But V is 4×3 (column-major). Expand to 4×4 by adding row [0,0,0,1]:
	//   V_4x4 = | col0  col1  col2  [0,0,0,1]^T |
	// Then V_new_4x4 = delta * V_4x4 and extract first 3 columns.
	//
	// For each output column j (j=0,1,2):
	//   newCol_j[i] = sum_k delta[i][k] * cachedCol_j[k]   for i=0..3
	// where delta is row-major: delta[i][k] = forwardPoseDelta[i*4+k]
	// and cachedCol_j = cachedView[j*4 .. j*4+3]
	float newView[12]; // 3 columns of float4
	for (int col = 0; col < 3; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += forwardPoseDelta[row * 4 + k] * cachedView[col * 4 + k];
			newView[col * 4 + row] = sum;
		}
	}

	// Write newView into CameraView[0] (bytes 0-47)
	memcpy(&patchedCB12[0], newView, kViewBytes);
	// Write this eye's Proj into CameraProj[0] (bytes 96-143)
	memcpy(&patchedCB12[kProjOffset], cachedProj, kViewBytes);

	if (doLog) {
		const float* cv = cachedView;
		const float* nv = newView;
		OOVR_LOGF("FPReplay[%d]: cachedView col0=[%.3f %.3f %.3f %.1f] col1=[%.3f %.3f %.3f %.1f] col2=[%.3f %.3f %.3f %.1f]",
			eye, cv[0],cv[1],cv[2],cv[3], cv[4],cv[5],cv[6],cv[7], cv[8],cv[9],cv[10],cv[11]);
		OOVR_LOGF("FPReplay[%d]: newView    col0=[%.3f %.3f %.3f %.1f] col1=[%.3f %.3f %.3f %.1f] col2=[%.3f %.3f %.3f %.1f]",
			eye, nv[0],nv[1],nv[2],nv[3], nv[4],nv[5],nv[6],nv[7], nv[8],nv[9],nv[10],nv[11]);
		OOVR_LOGF("FPReplay[%d]: fwdDelta row0=[%.5f %.5f %.5f %.5f]",
			eye, forwardPoseDelta[0], forwardPoseDelta[1], forwardPoseDelta[2], forwardPoseDelta[3]);
		OOVR_LOGF("FPReplay[%d]: replaying %d draws", eye, cs.drawCount);
	}

	// Upload patched CB[12] to replay buffer
	ctx->UpdateSubresource(m_replayCB[5], 0, nullptr, patchedCB12, 0, 0); // index 5 = CB[12]

	// Clear hand RT
	FLOAT clearColor[4] = { 0, 0, 0, 0 };
	ctx->ClearRenderTargetView(m_handRTV[eye], clearColor);
	ctx->ClearDepthStencilView(m_handDSV[eye], D3D11_CLEAR_DEPTH, 1.0f, 0);

	// Set viewport
	D3D11_VIEWPORT vp = {};
	vp.Width = (float)m_renderWidth;
	vp.Height = (float)m_renderHeight;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);

	// Bind hand RT + DSV
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	rtvs[0] = m_handRTV[eye];
	ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, m_handDSV[eye]);

	// Replay each captured FP draw
	int replayed = 0;
	for (int i = 0; i < cs.drawCount && i < FPReplayData::MAX_DRAWS; i++) {
		const auto& draw = cs.draws[i];
		if (!draw.valid || draw.indexCount == 0)
			continue;

		// IA state
		ctx->IASetIndexBuffer(draw.ib, draw.ibFmt, draw.ibOff);
		ctx->IASetVertexBuffers(0, 1, const_cast<ID3D11Buffer* const*>(&draw.vb),
			&draw.vbStride, &draw.vbOff);
		ctx->IASetInputLayout(draw.il);
		ctx->IASetPrimitiveTopology(draw.topo);

		// VS + PS shaders
		ctx->VSSetShader(draw.vs, nullptr, 0);
		ctx->PSSetShader(draw.ps, nullptr, 0);

		// Per-draw VS CBs: upload byte snapshots to pre-allocated buffers
		if (draw.hasCB0) ctx->UpdateSubresource(m_replayCB[0], 0, nullptr, draw.cb0, 0, 0);
		if (draw.hasCB1) ctx->UpdateSubresource(m_replayCB[1], 0, nullptr, draw.cb1, 0, 0);
		if (draw.hasCB2) ctx->UpdateSubresource(m_replayCB[2], 0, nullptr, draw.cb2, 0, 0);
		if (draw.hasCB9) ctx->UpdateSubresource(m_replayCB[3], 0, nullptr, draw.cb9, 0, 0);
		if (draw.hasCB10) ctx->UpdateSubresource(m_replayCB[4], 0, nullptr, draw.cb10, 0, 0);
		if (draw.hasCB13) ctx->UpdateSubresource(m_replayCB[6], 0, nullptr, draw.cb13, 0, 0);

		// Bind VS CBs to proper slots
		// Slots: 0, 1, 2, 9, 10, 12, 13
		ID3D11Buffer* vsCBBinds[14] = {};
		if (draw.hasCB0) vsCBBinds[0] = m_replayCB[0];
		if (draw.hasCB1) vsCBBinds[1] = m_replayCB[1];
		if (draw.hasCB2) vsCBBinds[2] = m_replayCB[2];
		if (draw.hasCB9) vsCBBinds[9] = m_replayCB[3];
		if (draw.hasCB10) vsCBBinds[10] = m_replayCB[4];
		vsCBBinds[12] = m_replayCB[5]; // always bind patched CB[12]
		if (draw.hasCB13) vsCBBinds[13] = m_replayCB[6];
		ctx->VSSetConstantBuffers(0, 14, vsCBBinds);

		// PS SRVs + samplers
		ctx->PSSetShaderResources(0, 8, const_cast<ID3D11ShaderResourceView* const*>(draw.psSRVs));
		ctx->PSSetSamplers(0, 4, const_cast<ID3D11SamplerState* const*>(draw.psSamplers));

		// PS CBs
		if (draw.hasPSCB0) ctx->UpdateSubresource(m_replayPSCB[0], 0, nullptr, draw.psCB0, 0, 0);
		if (draw.hasPSCB1) ctx->UpdateSubresource(m_replayPSCB[1], 0, nullptr, draw.psCB1, 0, 0);
		if (draw.hasPSCB2) ctx->UpdateSubresource(m_replayPSCB[2], 0, nullptr, draw.psCB2, 0, 0);
		ID3D11Buffer* psCBBinds[3] = {};
		if (draw.hasPSCB0) psCBBinds[0] = m_replayPSCB[0];
		if (draw.hasPSCB1) psCBBinds[1] = m_replayPSCB[1];
		if (draw.hasPSCB2) psCBBinds[2] = m_replayPSCB[2];
		ctx->PSSetConstantBuffers(0, 3, psCBBinds);

		// OM: our own DSS for depth testing, capture's blend state
		ctx->OMSetDepthStencilState(m_handDSS, 0);
		if (draw.blendState)
			ctx->OMSetBlendState(draw.blendState, draw.blendFactor, draw.sampleMask);

		// RS
		if (draw.rs)
			ctx->RSSetState(draw.rs);

		// Draw (instanceCount=1 for single-eye)
		ctx->DrawIndexed(draw.indexCount, draw.startIndex, draw.baseVertex);
		replayed++;
	}

	// Unbind hand RT
	ID3D11RenderTargetView* nullRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRTVs, nullptr);

	if (replayed == 0)
		return false;

	// Composite hand pixels onto warp output via CS, with controller delta shift
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(ctx->Map(m_handCompositeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &hcc, sizeof(hcc));
			ctx->Unmap(m_handCompositeCB, 0);
		}

		ID3D11ShaderResourceView* handSRVs[] = { m_srvHandColor[eye], m_srvHandDepth[eye] };
		ctx->CSSetShaderResources(0, 2, handSRVs);
		ID3D11UnorderedAccessView* compUAV[] = { m_uavOutput[eye] };
		ctx->CSSetUnorderedAccessViews(0, 1, compUAV, nullptr);
		ID3D11Buffer* cbs[] = { m_handCompositeCB };
		ctx->CSSetConstantBuffers(0, 1, cbs);
		ctx->CSSetShader(m_handCompositeCS, nullptr, 0);

		uint32_t groupsX = (m_renderWidth + 7) / 8;
		uint32_t groupsY = (m_renderHeight + 7) / 8;
		ctx->Dispatch(groupsX, groupsY, 1);

		// Unbind
		ID3D11ShaderResourceView* nullSRVs[2] = {};
		ID3D11UnorderedAccessView* nullUAVs[1] = {};
		ID3D11Buffer* nullCBs[1] = {};
		ctx->CSSetShaderResources(0, 2, nullSRVs);
		ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
		ctx->CSSetConstantBuffers(0, 1, nullCBs);
		ctx->CSSetShader(nullptr, nullptr, 0);
	}

	if (doLog) {
		s_replayLog++;
		OOVR_LOGF("FPReplay[%d]: replayed %d draws, composite dispatched (%ux%u) ctrlDelta L=(%.4f,%.4f) R=(%.4f,%.4f)",
			eye, replayed, m_renderWidth, m_renderHeight,
			hcc.controllerDeltaL[0], hcc.controllerDeltaL[1],
			hcc.controllerDeltaR[0], hcc.controllerDeltaR[1]);
	}

	return true;
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
			// Locomotion/rotation deltas are calibrated for N-2→N-1 step.
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

	// Store final poseDeltaMatrix for DLSS warp callback (warp MV generation)
	memcpy(m_lastPoseDelta[eye], cb.poseDeltaMatrix, sizeof(cb.poseDeltaMatrix));

	// Vertical camera correction: handled by game MVs through the residual path
	// (totalMV - headOnlyMV), same as horizontal locomotion. The game MVs capture
	// jumping, falling, stairs, and walk-cycle bob. No additional injection needed.

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
		float targetBlend = 1.0f - std::min(1.0f, std::max(0.0f, (totalMotion - 0.001f) / 0.004f));
		// Smooth the blend factor to avoid jarring snap between stationary/moving modes.
		// Fast attack (quickly enter moving mode), slow release (gradually return to stationary).
		static float s_smoothBlend = 1.0f;
		if (targetBlend < s_smoothBlend)
			s_smoothBlend = targetBlend; // instant attack: enter moving mode immediately
		else
			s_smoothBlend += (targetBlend - s_smoothBlend) * 0.15f; // slow release: ~7 frames to settle
		cb.staticBlendFactor = s_smoothBlend;
	}
	cb.rotMVScale = m_rotMVScale;

	// Reprojection: compute clipToClip from warp output → frame N (published slot).
	// Frame N was rendered at a newer pose where disoccluded background may be visible.
	// Uses direct deltaV computation (proven correct for warp-to-warp DLSS MVs).
	int pubSlot = m_publishedSlot.load(std::memory_order_acquire);
	if (pubSlot >= 0 && pubSlot != slot) {
		const auto& warpP = newPose;  // current warp pose
		const auto& frameNP = m_slotPose[pubSlot][eye]; // frame N's pose

		// deltaV = V_frameN * inv(V_warp) — maps warp view → frame N view
		float pqx=frameNP.orientation.x, pqy=frameNP.orientation.y, pqz=frameNP.orientation.z, pqw=frameNP.orientation.w;
		float Rn00=1-2*(pqy*pqy+pqz*pqz), Rn01=2*(pqx*pqy-pqz*pqw), Rn02=2*(pqx*pqz+pqy*pqw);
		float Rn10=2*(pqx*pqy+pqz*pqw), Rn11=1-2*(pqx*pqx+pqz*pqz), Rn12=2*(pqy*pqz-pqx*pqw);
		float Rn20=2*(pqx*pqz-pqy*pqw), Rn21=2*(pqy*pqz+pqx*pqw), Rn22=1-2*(pqx*pqx+pqy*pqy);

		float cqx=warpP.orientation.x, cqy=warpP.orientation.y, cqz=warpP.orientation.z, cqw=warpP.orientation.w;
		float Rw00=1-2*(cqy*cqy+cqz*cqz), Rw01=2*(cqx*cqy-cqz*cqw), Rw02=2*(cqx*cqz+cqy*cqw);
		float Rw10=2*(cqx*cqy+cqz*cqw), Rw11=1-2*(cqx*cqx+cqz*cqz), Rw12=2*(cqy*cqz-cqx*cqw);
		float Rw20=2*(cqx*cqz-cqy*cqw), Rw21=2*(cqy*cqz+cqx*cqw), Rw22=1-2*(cqx*cqx+cqy*cqy);

		// deltaRot = Rn^T * Rw
		float d00=Rn00*Rw00+Rn10*Rw10+Rn20*Rw20, d01=Rn00*Rw01+Rn10*Rw11+Rn20*Rw21, d02=Rn00*Rw02+Rn10*Rw12+Rn20*Rw22;
		float d10=Rn01*Rw00+Rn11*Rw10+Rn21*Rw20, d11=Rn01*Rw01+Rn11*Rw11+Rn21*Rw21, d12=Rn01*Rw02+Rn11*Rw12+Rn21*Rw22;
		float d20=Rn02*Rw00+Rn12*Rw10+Rn22*Rw20, d21=Rn02*Rw01+Rn12*Rw11+Rn22*Rw21, d22=Rn02*Rw02+Rn12*Rw12+Rn22*Rw22;

		// deltaTrans = Rn^T * (pw - pn)
		float dpx=warpP.position.x-frameNP.position.x, dpy=warpP.position.y-frameNP.position.y, dpz=warpP.position.z-frameNP.position.z;
		float dt0=Rn00*dpx+Rn10*dpy+Rn20*dpz;
		float dt1=Rn01*dpx+Rn11*dpy+Rn21*dpz;
		float dt2=Rn02*dpx+Rn12*dpy+Rn22*dpz;

		// deltaV column-major
		float dV[16] = {};
		dV[0]=d00; dV[4]=d01; dV[8]=d02;  dV[12]=dt0;
		dV[1]=d10; dV[5]=d11; dV[9]=d12;  dV[13]=dt1;
		dV[2]=d20; dV[6]=d21; dV[10]=d22; dV[14]=dt2;
		dV[3]=0;   dV[7]=0;   dV[11]=0;   dV[15]=1;

		// P and P^{-1} from FOV (right-handed, same as DLSS warp callback)
		float tanL=cb.fovTanLeft, tanR=cb.fovTanRight, tanU=cb.fovTanUp, tanD=cb.fovTanDown;
		float w=tanR-tanL, h=tanU-tanD, n=cb.nearZ, f=cb.farZ;

		float P[16]={}, Pi[16]={};
		P[0]=2.0f/w; P[8]=(tanR+tanL)/w;
		P[5]=2.0f/h; P[9]=(tanU+tanD)/h;
		P[10]=-f/(f-n); P[14]=-(f*n)/(f-n);
		P[11]=-1.0f;

		Pi[0]=w/2.0f; Pi[12]=(tanR+tanL)/2.0f;
		Pi[5]=h/2.0f; Pi[13]=(tanU+tanD)/2.0f;
		Pi[14]=-1.0f;
		Pi[11]=-(f-n)/(f*n); Pi[15]=1.0f/n;

		// reprojC2C = P * dV * Pi (column-major)
		float tmp[16]={}, reproj[16]={};
		for (int c=0; c<4; c++) for (int r=0; r<4; r++) {
			float s=0; for (int k=0; k<4; k++) s+=dV[k*4+r]*Pi[c*4+k]; tmp[c*4+r]=s;
		}
		for (int c=0; c<4; c++) for (int r=0; r<4; r++) {
			float s=0; for (int k=0; k<4; k++) s+=P[k*4+r]*tmp[c*4+k]; reproj[c*4+r]=s;
		}

		// Store as row-major for HLSL (transpose column-major → row-major)
		for (int r=0; r<4; r++) for (int c=0; c<4; c++)
			cb.reprojClipToClip[r*4+c] = reproj[c*4+r];
		cb.hasReprojData = 1;
	} else {
		memset(cb.reprojClipToClip, 0, sizeof(cb.reprojClipToClip));
		cb.hasReprojData = 0;
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

	// fullWarpC2C: pass through the raw c2c_with_loco (N-1→N-2, with locomotion injection).
	// In the shader, the DIFFERENCE between this and clipToClipNoLoco at each pixel's depth
	// gives the depth-aware locomotion offset. This avoids composing matrices from different
	// frameworks (RSS vs OpenXR) which causes clip space mismatches.
	if (m_slotHasClipToClipWithLoco[slot] && eye >= 0 && eye < 2) {
		memcpy(cb.fullWarpC2C, m_slotClipToClipWithLoco[slot][eye], sizeof(cb.fullWarpC2C));
		cb.hasFullWarpC2C = 1;
	} else {
		cb.hasFullWarpC2C = 0;
	}

	cb.resolution[0] = (float)m_renderWidth;
	cb.resolution[1] = (float)m_renderHeight;
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
		// MV correction: residual = totalMV - headOnlyMV isolates stick + loco + animation
		// from game MVs. poseDeltaMatrix handles real-time head tracking from OpenXR.
		// m_mvConfidenceScale decays at warp time when sticks are released to prevent
		// stale game MVs from overshooting on stop transitions.
		cb.mvConfidence = oovr_global_configuration.ASWMVConfidence() * m_mvConfidenceScale;
	}
	cb.mvPixelScale = oovr_global_configuration.ASWMVPixelScale();
	cb.depthResolution[0] = (m_slotDepthDataW[slot] > 0) ? (float)m_slotDepthDataW[slot] : (float)m_renderWidth;
	cb.depthResolution[1] = (m_slotDepthDataH[slot] > 0) ? (float)m_slotDepthDataH[slot] : (float)m_renderHeight;
	cb.mvResolution[0] = (m_slotMVDataW[slot] > 0) ? (float)m_slotMVDataW[slot] : (float)m_renderWidth;
	cb.mvResolution[1] = (m_slotMVDataH[slot] > 0) ? (float)m_slotMVDataH[slot] : (float)m_renderHeight;
	cb._pad_npcMask = 0;  // removed: was hasNpcMask
	cb.debugMode = oovr_global_configuration.ASWDebugMode();



	const float locoMotion = sqrtf(m_locoTransX * m_locoTransX +
		m_locoTransY * m_locoTransY + m_locoTransZ * m_locoTransZ);
	const float stickYawMotion = fabsf(m_locoYaw);
	// Always use backward warp + NPC overlay. Forward scatter during locomotion is disabled
	// because: (1) we now warp the most recent frame (locomotion baked in, only head tracking delta),
	// (2) forward scatter requires dilation which thickens foliage noticeably,
	// (3) the mode switch on start/stop locomotion causes a visible jerk.
	// NPC-only forward scatter overlay handles moving NPCs with correct depth ordering.
	const bool useForwardScatterForMotion = false;

	// ── Controller hand detection (pre-projected at CacheFrame time) ──
	cb.hasControllerPose = 0;
	if (m_slotHasControllerUV[slot][eye]) {
		cb.controllerUV_LR[0] = m_slotControllerUV[slot][eye][0][0];
		cb.controllerUV_LR[1] = m_slotControllerUV[slot][eye][0][1];
		cb.controllerUV_LR[2] = m_slotControllerUV[slot][eye][1][0];
		cb.controllerUV_LR[3] = m_slotControllerUV[slot][eye][1][1];
		cb.controllerRadius = m_slotControllerRadius[slot];
		cb.hasControllerPose = 1;
	}
	// Compute controller UV delta for FP hand prediction.
	// Project current controller positions using the CACHED-FRAME head pose, not warp-time.
	// This way the delta captures ONLY controller movement relative to head —
	// head rotation/translation is already handled by parallaxSourceUV (poseDeltaMatrix).
	cb.controllerDeltaL[0] = cb.controllerDeltaL[1] = 0.0f;
	cb.controllerDeltaR[0] = cb.controllerDeltaR[1] = 0.0f;

	// Compute FP controller scale from timing.
	// Controller delta spans cache_time → now (warp read time).
	// With N-1 warping, cache is ~1 period old, so delta ≈ 1 period of motion.
	// The warp frame displays at the midpoint between game frames = 0.5 periods
	// after the latest game frame = (cacheAge - 0.5*period) from cache time.
	// Since cacheAge ≈ period: display offset ≈ 0.5 * cacheAge.
	// Scale = displayOffset / cacheAge = 0.5 * (cacheAge + jitter) / cacheAge.
	// The autoScale term captures the timing jitter (typically 1.0-1.2).
	{
		auto now = std::chrono::steady_clock::now();
		float cacheAgeMs = std::chrono::duration<float, std::milli>(now - m_slotTimestamp[slot]).count();
		// previousPublished is N-1; the published slot (N) is one period newer.
		// Display time is 0.5 periods after slot N = cacheAge - 0.5*period from cache.
		// Estimate period from the gap between published and previousPublished timestamps.
		int pubSlot = m_publishedSlot.load(std::memory_order_acquire);
		float periodMs = cacheAgeMs; // fallback: assume cacheAge ≈ period
		if (pubSlot >= 0 && pubSlot != slot) {
			float pubAgeMs = std::chrono::duration<float, std::milli>(now - m_slotTimestamp[pubSlot]).count();
			periodMs = cacheAgeMs - pubAgeMs; // time between N-1 and N
			if (periodMs < 5.0f) periodMs = cacheAgeMs; // fallback
		}
		float displayOffsetMs = cacheAgeMs - 0.5f * periodMs;
		float scale = (cacheAgeMs > 1.0f) ? (displayOffsetMs / cacheAgeMs) : 0.5f;
		scale = (scale < 0.2f) ? 0.2f : (scale > 1.0f) ? 1.0f : scale; // clamp
		cb.fpControllerScale = scale;
	}
	if (cb.hasControllerPose && m_controllerValid[0] && m_controllerValid[1]) {
		const XrPosef& cachedPose = m_slotPose[slot][eye];
		// Log cached vs current controller positions to verify delta
		{
			static int s_deltaLog = 0;
			if (s_deltaLog++ < 20 && eye == 0) {
				// Cached controller positions were stored via SetSlotControllerUV at CacheFrame time.
				// m_controllerPos is from the LATEST WaitGetPoses call.
				OOVR_LOGF("CtrlDelta: slot=%d cachedUV_L=(%.4f,%.4f) cachedUV_R=(%.4f,%.4f) "
				    "ctrlPos_L=(%.3f,%.3f,%.3f) ctrlPos_R=(%.3f,%.3f,%.3f) "
				    "cachedHead=(%.3f,%.3f,%.3f)",
				    slot,
				    cb.controllerUV_LR[0], cb.controllerUV_LR[1],
				    cb.controllerUV_LR[2], cb.controllerUV_LR[3],
				    m_controllerPos[0][0], m_controllerPos[0][1], m_controllerPos[0][2],
				    m_controllerPos[1][0], m_controllerPos[1][1], m_controllerPos[1][2],
				    cachedPose.position.x, cachedPose.position.y, cachedPose.position.z);
			}
		}
		XrQuaternionf qi = { -cachedPose.orientation.x, -cachedPose.orientation.y,
		    -cachedPose.orientation.z, cachedPose.orientation.w };
		float fovL = cb.fovTanLeft, fovR = cb.fovTanRight;
		float fovU = cb.fovTanUp, fovD = cb.fovTanDown;
		float vOffset = 0.07f;
		for (int h = 0; h < 2; h++) {
			float cx = m_controllerPos[h][0], cy = m_controllerPos[h][1], cz = m_controllerPos[h][2];
			// Relative to CACHED head position (same frame as controllerUV_LR)
			float rx = cx - cachedPose.position.x;
			float ry = cy - cachedPose.position.y;
			float rz = cz - cachedPose.position.z;
			// Rotate by inverse CACHED head orientation → view space
			float qx = qi.x, qy = qi.y, qz = qi.z, qw = qi.w;
			float tx = 2.0f * (qy * rz - qz * ry);
			float ty = 2.0f * (qz * rx - qx * rz);
			float tz = 2.0f * (qx * ry - qy * rx);
			float vx = rx + qw * tx + (qy * tz - qz * ty);
			float vy = ry + qw * ty + (qz * tx - qx * tz);
			float vz = rz + qw * tz + (qx * ty - qy * tx);
			if (vz > -0.01f) continue;
			float tanX = vx / (-vz);
			float tanY = vy / (-vz);
			float nowU = (tanX - fovL) / (fovR - fovL);
			float nowV = (tanY - fovU) / (fovD - fovU) + vOffset;
			// Delta = current controller UV (in cached head space) - cached-frame UV
			// This is purely controller-relative-to-head motion.
			if (h == 0) {
				cb.controllerDeltaL[0] = nowU - cb.controllerUV_LR[0];
				cb.controllerDeltaL[1] = nowV - cb.controllerUV_LR[1];
			} else {
				cb.controllerDeltaR[0] = nowU - cb.controllerUV_LR[2];
				cb.controllerDeltaR[1] = nowV - cb.controllerUV_LR[3];
			}
		}
		{
			static int s_deltaLog2 = 0;
			if (s_deltaLog2++ < 20 && eye == 0) {
				OOVR_LOGF("CtrlDeltaUV: dL=(%.5f,%.5f) dR=(%.5f,%.5f) px_dL=(%.1f,%.1f) px_dR=(%.1f,%.1f)",
				    cb.controllerDeltaL[0], cb.controllerDeltaL[1],
				    cb.controllerDeltaR[0], cb.controllerDeltaR[1],
				    cb.controllerDeltaL[0] * cb.resolution[0], cb.controllerDeltaL[1] * cb.resolution[1],
				    cb.controllerDeltaR[0] * cb.resolution[0], cb.controllerDeltaR[1] * cb.resolution[1]);
			}
		}
	}

	// ── First-person bounding sphere (world-space distance test) ──
	// Read worldBound from FP root NiAVObject, subtract posAdjust for rendering space.
	// posAdjust is at RSS base + 0x3A4 (EYE_POSITION<NiPoint3, 2>).
	cb.hasFPSphere = 0;
	if (m_firstPersonRootPtr && m_rssViewMatPtr) {
		const uint8_t* fpNode = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(m_firstPersonRootPtr));
		const float* bc = reinterpret_cast<const float*>(fpNode + 0xE4); // worldBound.center
		float br = *reinterpret_cast<const float*>(fpNode + 0xF0);       // worldBound.radius

		// posAdjust: rssViewMatPtr = rssBase + 0x3E0 + 1*0x250 + 0x30
		// posAdjust for eye 0 = rssBase + 0x3A4
		// offset from rssViewMatPtr: 0x3A4 - (0x3E0 + 0x250 + 0x30) = 0x3A4 - 0x660 = -0x2BC
		const float* posAdj = reinterpret_cast<const float*>(
		    reinterpret_cast<const uint8_t*>(m_rssViewMatPtr) - 0x2BC + eye * 12);

		if (br > 0.1f && br < 500.0f) {
			cb.fpSphereCenter[0] = bc[0] - posAdj[0];
			cb.fpSphereCenter[1] = bc[1] - posAdj[1];
			cb.fpSphereCenter[2] = bc[2] - posAdj[2];
			cb.fpSphereCenter[3] = 0.0f; // padding
			cb.fpSphereRadius = br * 1.5f; // scale up to cover fully extended arms + weapons
			cb.hasFPSphere = 1;

			static int s_fpLog = 0;
			if (s_fpLog++ < 5 && eye == 0) {
				OOVR_LOGF("FPSphere: center=(%.1f,%.1f,%.1f) r=%.1f posAdj=(%.1f,%.1f,%.1f)",
				    cb.fpSphereCenter[0], cb.fpSphereCenter[1], cb.fpSphereCenter[2], br,
				    posAdj[0], posAdj[1], posAdj[2]);
			}
		}
	}

	// ── Menu state (skip MV corrections when menu is open) ──
	cb.isMenuOpen = m_isMenuOpen ? 1 : 0;

	// ── FP bounding box projection (world spheres → screen AABBs) ──
	cb.fpBoxCount = 0;
	// Project per-geometry FP bounds to screen-space AABBs — read LIVE from game memory
	if (m_rssViewMatPtr && m_fpGeomCount > 0) {
		const float* posAdj = reinterpret_cast<const float*>(
		    reinterpret_cast<const uint8_t*>(m_rssViewMatPtr) - 0x2BC + eye * 12);
		// RSS VP: row-major, row-vector convention (clip = v * M)
		// m_rssViewMatPtr = rssBase + 0x3E0 + 1*0x250 + 0x30 (eye 1 view matrix)
		// VP for eye e = rssBase + 0x3E0 + e*0x250 + 0x130
		// offset from m_rssViewMatPtr: (e-1)*0x250 + (0x130-0x30) = (e-1)*0x250 + 0x100
		float vp[16];
		memcpy(vp, reinterpret_cast<const uint8_t*>(m_rssViewMatPtr) + (eye - 1) * 0x250 + 0x100, sizeof(vp));
		// vp is now row-major (RSS convention). For v*M: clip = [x,y,z,1] * vp

		int boxCount = 0;
		for (uint32_t i = 0; i < m_fpGeomCount && i < 16; i++) {
			// Read worldBound LIVE from BSGeometry pointer (NiAVObject::worldBound at +0xE4)
			const uint8_t* geom = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(m_fpGeomPointers[i]));
			if (!geom) continue;
			const float* bc = reinterpret_cast<const float*>(geom + 0xE4);
			float r = *reinterpret_cast<const float*>(geom + 0xF0);
			if (r < 0.01f || r > 500.0f) continue;
			float cx = bc[0] - posAdj[0];
			float cy = bc[1] - posAdj[1];
			float cz = bc[2] - posAdj[2];

			// Transform center by VP (row-major, row-vector: clip = v * M)
			float clipX = cx * vp[0] + cy * vp[4] + cz * vp[8]  + vp[12];
			float clipY = cx * vp[1] + cy * vp[5] + cz * vp[9]  + vp[13];
			float clipW = cx * vp[3] + cy * vp[7] + cz * vp[11] + vp[15];

			if (clipW <= 0.01f) continue; // Behind camera

			// NDC
			float ndcX = clipX / clipW;
			float ndcY = clipY / clipW;

			// Screen-space radius: approximate as r / distance (clipW ≈ view-space Z)
			float screenR = r / clipW;
			// Scale by projection (approximate: use average of fovTanLeft/Right)
			float projScale = 0.5f * (fabsf(cb.fovTanLeft) + fabsf(cb.fovTanRight));
			if (projScale > 0.01f) screenR /= projScale;
			screenR *= 1.3f; // Conservative margin

			// NDC to UV: u = (ndcX + 1) * 0.5, v = (1 - ndcY) * 0.5
			float u = (ndcX + 1.0f) * 0.5f;
			float v = (1.0f - ndcY) * 0.5f;

			float minU = u - screenR;
			float minV = v - screenR;
			float maxU = u + screenR;
			float maxV = v + screenR;

			// Clamp to [0,1] and skip if entirely off-screen
			if (maxU < 0.0f || minU > 1.0f || maxV < 0.0f || minV > 1.0f) continue;
			if (minU < 0.0f) minU = 0.0f;
			if (minV < 0.0f) minV = 0.0f;
			if (maxU > 1.0f) maxU = 1.0f;
			if (maxV > 1.0f) maxV = 1.0f;

			cb.fpScreenBoxes[boxCount * 4 + 0] = minU;
			cb.fpScreenBoxes[boxCount * 4 + 1] = minV;
			cb.fpScreenBoxes[boxCount * 4 + 2] = maxU;
			cb.fpScreenBoxes[boxCount * 4 + 3] = maxV;
			boxCount++;
		}
		cb.fpBoxCount = boxCount;

		static int s_boxLog = 0;
		if (s_boxLog++ < 10 && eye == 0 && boxCount > 0) {
			OOVR_LOGF("FPBox: %d boxes from %u geoms (LIVE). box[0]=(%.3f,%.3f)-(%.3f,%.3f)",
				boxCount, m_fpGeomCount,
				cb.fpScreenBoxes[0], cb.fpScreenBoxes[1],
				cb.fpScreenBoxes[2], cb.fpScreenBoxes[3]);
		}
	}

	// ── D3D11 warp path ──
	ID3D11DeviceContext* ctx = nullptr;
	m_device->GetImmediateContext(&ctx);
	if (!ctx)
		return false;

	ID3D11ShaderResourceView* mvSRV = (fabsf(cb.mvConfidence) > 0.001f) ? m_srvMV[slot][eye] : nullptr;
	// Bind frame N (published slot) color + depth for disocclusion reprojection fill
	ID3D11ShaderResourceView* frameNColorSRV = nullptr;
	ID3D11ShaderResourceView* frameNDepthSRV = nullptr;
	if (cb.hasReprojData && pubSlot >= 0 && pubSlot != slot) {
		frameNColorSRV = m_srvColor[pubSlot][eye];
		frameNDepthSRV = m_srvDepth[pubSlot][eye];
	}
	// FP depth texture SRV no longer used — replaced by world-space sphere test.

	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		ctx->Release();
		return false;
	}
	memcpy(mapped.pData, &cb, sizeof(cb));
	ctx->Unmap(m_constantBuffer, 0);

	ID3D11ShaderResourceView* srvs[] = {
		m_srvColor[slot][eye], mvSRV, m_srvDepth[slot][eye],
		frameNColorSRV, frameNDepthSRV,
		m_srvPreFPDepth[slot][eye]  // t5: pre-FP depth
	};
	ctx->CSSetShaderResources(0, _countof(srvs), srvs);
	ID3D11UnorderedAccessView* uavs3[] = { m_uavOutput[eye], m_uavAtomicDepth[eye], m_uavForwardMap[eye] };
	ctx->CSSetUnorderedAccessViews(0, 3, uavs3, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &m_constantBuffer);
	ctx->CSSetSamplers(0, 1, &m_linearSampler);

	uint32_t groupsX = (m_renderWidth + 7) / 8;
	uint32_t groupsY = (m_renderHeight + 7) / 8;
	// NPC-only forward scatter overlay: forward-scatter only pixels with significant
	// MV residual (moving objects) on top of the backward warp output. Uses
	// IsMovingNpcPixelForward() which detects motion via MV residual after c2c head
	// subtraction, with a threshold that scales with head motion magnitude.
	// Disabled during locomotion testing — NPC detection can't distinguish NPCs from
	// static objects when locomotion is in gameMV but not clipToClipNoLoco.
	const bool useStationaryNpcForwardOverlay = false;

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
		}

		// Depth pre-scatter: forward-scatter all cached depths to their warp output positions.
		// CSMain reads warped depth from atomicDepth → correct foreground depth at leading edges.
		if (m_depthPreScatterCS) {
			ctx->CSSetShader(m_depthPreScatterCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);
		} else {
			static bool logged = false;
			if (!logged) { OOVR_LOG("ASW: CSDepthPreScatter shader is NULL"); logged = true; }
		}

		ctx->CSSetShader(m_warpCS, nullptr, 0);
		ctx->Dispatch(groupsX, groupsY, 1);

		// CSDilate: fill disocclusion holes with background content.
		if (m_dilateCS) {
			ctx->CSSetShader(m_dilateCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);
		}

		// CSBlurVoids: post-fill blur within void zones only.
		// Averages filled void pixels with their void-pixel neighbors.
		// No non-void pixels are introduced — purely smooths the fill.
		if (m_blurVoidsCS) {
			ctx->CSSetShader(m_blurVoidsCS, nullptr, 0);
			ctx->Dispatch(groupsX, groupsY, 1);
		}

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

	ID3D11ShaderResourceView* nullSRVs[6] = {};
	ID3D11UnorderedAccessView* nullUAVs[3] = {};
	ctx->CSSetShaderResources(0, 6, nullSRVs);
	ctx->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);

	// FP Hand Replay disabled — captures are stale (frame 1 only, game's main FP render
	// bypasses all hooks on frame 2+). Controller motion is now handled by the warp shader
	// via GetControllerDelta() UV shift on FP pixels.
	// ReplayFPHands(eye, ctx, cb.forwardPoseDelta, hcc);

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

	// If a warp upscale callback is set (DLSS on warp frames), upscale each eye's
	// render-res warp output to display-res before copying to the output swapchain.
	if (m_warpUpscaleCallback) {
		bool upscaleOk = true;
		ID3D11Texture2D* upscaled[2] = {};
		for (int eye = 0; eye < 2; eye++) {
			WarpUpscaleParams params = {};
			params.eye = eye;
			params.ctx = ctx;
			params.warpedColor = m_warpedOutput[eye];
			params.cachedDepth = m_cachedDepth[slot][eye];
			params.renderW = m_renderWidth;
			params.renderH = m_renderHeight;
			params.outputW = m_outputWidth;
			params.outputH = m_outputHeight;
			params.nearZ = m_slotNear[slot];
			params.farZ = m_slotFar[slot];
			params.cachedPose = m_slotPose[slot][eye];
			params.warpPose = m_precompPose[eye];
			params.cachedFov = m_slotFov[slot][eye];
			memcpy(params.poseDeltaMatrix, m_lastPoseDelta[eye], sizeof(params.poseDeltaMatrix));
			if (!m_warpUpscaleCallback(params, &upscaled[eye])) {
				upscaleOk = false;
				break;
			}
		}
		if (upscaleOk && upscaled[0] && upscaled[1]) {
			ctx->CopySubresourceRegion(target, 0,
			    0, 0, 0, upscaled[0], 0, nullptr);
			ctx->CopySubresourceRegion(target, 0,
			    m_outputWidth, 0, 0, upscaled[1], 0, nullptr);
		} else {
			// Fallback: copy render-res warp output directly (no upscaling)
			ctx->CopySubresourceRegion(target, 0,
			    0, 0, 0, m_warpedOutput[0], 0, nullptr);
			ctx->CopySubresourceRegion(target, 0,
			    m_outputWidth, 0, 0, m_warpedOutput[1], 0, nullptr);
		}
	} else {
		// No upscale callback — render dims == output dims, copy directly
		ctx->CopySubresourceRegion(target, 0,
		    0, 0, 0, m_warpedOutput[0], 0, nullptr);
		ctx->CopySubresourceRegion(target, 0,
		    m_outputWidth, 0, 0, m_warpedOutput[1], 0, nullptr);
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
					depthBox.right = m_renderWidth;
					depthBox.bottom = m_renderHeight;
					depthBox.front = 0;
					depthBox.back = 1;
					ctx->CopySubresourceRegion(depthTarget, 0,
					    0, 0, 0, m_cachedDepth[slot][0], 0, &depthBox);
					ctx->CopySubresourceRegion(depthTarget, 0,
					    m_outputWidth, 0, 0, m_cachedDepth[slot][1], 0, &depthBox);
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

	// Wait for any in-flight background shader compilation before releasing resources
	if (m_compileStarted && m_compileFuture.valid()) {
		m_compileFuture.get();
		// Release any unreleased blobs
		for (auto& b : m_pendingBlobs) {
			if (b.blob) { b.blob->Release(); b.blob = nullptr; }
		}
		m_pendingBlobs.clear();
	}
	m_compileStarted = false;

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
			if (m_srvStencil[slot][eye]) {
				m_srvStencil[slot][eye]->Release();
				m_srvStencil[slot][eye] = nullptr;
			}
			if (m_cachedStencil[slot][eye]) {
				m_cachedStencil[slot][eye]->Release();
				m_cachedStencil[slot][eye] = nullptr;
			}
			if (m_srvPreFPDepth[slot][eye]) {
				m_srvPreFPDepth[slot][eye]->Release();
				m_srvPreFPDepth[slot][eye] = nullptr;
			}
			if (m_cachedPreFPDepth[slot][eye]) {
				m_cachedPreFPDepth[slot][eye]->Release();
				m_cachedPreFPDepth[slot][eye] = nullptr;
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
	if (m_depthPreScatterCS) { m_depthPreScatterCS->Release(); m_depthPreScatterCS = nullptr; }
	if (m_blurVoidsCS) { m_blurVoidsCS->Release(); m_blurVoidsCS = nullptr; }
	if (m_handCompositeCS) { m_handCompositeCS->Release(); m_handCompositeCS = nullptr; }

	// Hand render targets
	for (int e = 0; e < 2; e++) {
		if (m_srvHandColor[e]) { m_srvHandColor[e]->Release(); m_srvHandColor[e] = nullptr; }
		if (m_handRTV[e]) { m_handRTV[e]->Release(); m_handRTV[e] = nullptr; }
		if (m_handColor[e]) { m_handColor[e]->Release(); m_handColor[e] = nullptr; }
		if (m_srvHandDepth[e]) { m_srvHandDepth[e]->Release(); m_srvHandDepth[e] = nullptr; }
		if (m_handDSV[e]) { m_handDSV[e]->Release(); m_handDSV[e] = nullptr; }
		if (m_handDepthTex[e]) { m_handDepthTex[e]->Release(); m_handDepthTex[e] = nullptr; }
	}
	if (m_handDSS) { m_handDSS->Release(); m_handDSS = nullptr; }
	if (m_handCompositeCB) { m_handCompositeCB->Release(); m_handCompositeCB = nullptr; }
	for (int i = 0; i < 7; i++) {
		if (m_replayCB[i]) { m_replayCB[i]->Release(); m_replayCB[i] = nullptr; }
	}
	for (int i = 0; i < 3; i++) {
		if (m_replayPSCB[i]) { m_replayPSCB[i]->Release(); m_replayPSCB[i] = nullptr; }
	}
	m_fpReplayPtr = nullptr;

	for (int e = 0; e < 2; ++e) {
		if (m_srvAtomicDepth[e]) { m_srvAtomicDepth[e]->Release(); m_srvAtomicDepth[e] = nullptr; }
		if (m_uavAtomicDepth[e]) { m_uavAtomicDepth[e]->Release(); m_uavAtomicDepth[e] = nullptr; }
		if (m_atomicDepth[e]) { m_atomicDepth[e]->Release(); m_atomicDepth[e] = nullptr; }
	}


	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}

	m_renderWidth = 0;
	m_renderHeight = 0;
	m_outputWidth = 0;
	m_outputHeight = 0;
	m_warpUpscaleCallback = nullptr;
	OOVR_LOG("ASW: Shutdown");
}
