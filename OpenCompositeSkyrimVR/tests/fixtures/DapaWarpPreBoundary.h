#pragma once
// Shared with the WARP/D3D11 regression test; the test compiles the exact production shader.
inline constexpr char s_warpShaderHLSL[] = R"HLSL(
Texture2D<float4> prevColor : register(t0);
Texture2D<float> depthTex : register(t2);
RWTexture2D<float4> output : register(u0);
#ifdef DAPA_CAPTURE
RWTexture2D<float4> captureClean : register(u1);
RWTexture2D<float4> captureDiagnostic : register(u2);
#endif
SamplerState linearClamp : register(s0);
cbuffer WarpParams : register(b0) {
    row_major float4x4 poseDeltaMatrix;
    float2 resolution;
    float nearZ, farZ;
    float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
    float depthScale, edgeFadeWidth, nearFadeDepth, debugTint;
    float2 depthResolution;
    float2 sourceFlip;
    row_major float4x4 projection;
    row_major float4x4 inverseProjection;
    float predictionValid;
    float3 padding;
};
float2 RawUV(float2 uv) { return lerp(uv, 1.0 - uv, sourceFlip); }
float ReadDepth(float2 uv) {
    int2 limit = int2(depthResolution) - 1;
    return depthTex.Load(int3(clamp(int2(RawUV(uv)*depthResolution), int2(0,0), limit), 0));
}
bool ViewPosition(float2 uv, float d, out float3 p) {
    float2 raw = RawUV(uv);
    float4 h = mul(float4(raw.x*2-1, 1-raw.y*2, d, 1), inverseProjection);
    p = 0;
    if (!all(isfinite(h)) || abs(h.w) < 1e-7) return false;
    p = h.xyz / h.w;
    return all(isfinite(p));
}
[numthreads(8,8,1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)resolution)) return;
    float2 uv = (float2(tid.xy)+0.5)/resolution;
    float2 source = uv;
    float captureConfidence = 0;
    float2 captureDisplacement = 0;
    float3 viewPoint;
    // Invalid projection / depth has no trustworthy parallax. Do not extrapolate it.
    if (predictionValid > 0.5 && ViewPosition(uv, ReadDepth(uv), viewPoint)) {
        viewPoint *= max(depthScale, 0.001);
        float3 oldPoint = mul(poseDeltaMatrix, float4(viewPoint,1)).xyz;
        float4 clip = mul(float4(oldPoint,1), projection);
        if (all(isfinite(clip)) && abs(clip.w) > 1e-7) {
            float2 candidate = RawUV(float2(clip.x/clip.w*0.5+0.5, 0.5-clip.y/clip.w*0.5));
            captureDisplacement = candidate-uv;
            if (all(candidate >= 0) && all(candidate <= 1) && viewPoint.z*oldPoint.z > 0) {
                float3 observed;
                float confidence = 0;
                // Depth must agree at the destination lookup, not merely at the
                // output pixel. Reject foreground/background crossings instead
                // of dragging a character's colour into newly exposed scenery.
                if (ViewPosition(candidate, ReadDepth(candidate), observed)) {
                    float mismatch = abs(abs(observed.z)*max(depthScale,0.001)-abs(oldPoint.z))
                        / max(abs(oldPoint.z), 0.001);
                    confidence = 1-smoothstep(0.02,0.10,mismatch);
                }
                float minZ=abs(viewPoint.z), maxZ=minZ;
                int2 offsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
                [unroll] for(int i=0;i<4;++i) {
                    float2 neighborUV=saturate(uv+float2(offsets[i])/depthResolution);
                    float3 neighbor;
                    if(ViewPosition(neighborUV,ReadDepth(neighborUV),neighbor)) {
                        float z=abs(neighbor.z)*max(depthScale,0.001);
                        minZ=min(minZ,z); maxZ=max(maxZ,z);
                    } else confidence=0;
                }
                confidence *= saturate(1-(maxZ/max(minZ,0.001)-1)/max(edgeFadeWidth,0.001));
                confidence *= 1-smoothstep(0.04,0.12,length(candidate-uv));
                if(nearFadeDepth>0)
                    confidence *= saturate((abs(viewPoint.z)-nearFadeDepth)/nearFadeDepth);
                source=lerp(uv,candidate,confidence);
                captureConfidence=confidence;
            }
        }
    }
    float4 color=prevColor.SampleLevel(linearClamp,RawUV(source),0);
#ifdef DAPA_CAPTURE
    captureClean[tid.xy]=color;
    // R=accepted confidence. G/B encode intended UV displacement, zero=0.5.
    captureDiagnostic[tid.xy]=float4(captureConfidence,0.5+captureDisplacement*4,1);
#endif
    if(debugTint>0.5) color.rgb=float3(min(1.0,color.r*1.5+0.1),color.g*0.6,color.b*0.6);
    output[tid.xy]=color;
}
)HLSL";
