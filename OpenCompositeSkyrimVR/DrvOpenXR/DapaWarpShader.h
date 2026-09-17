#pragma once
// Shared with the WARP/D3D11 regression test; the test compiles the exact production shader.
inline constexpr char s_warpShaderHLSL[] = R"HLSL(
Texture2D<float4> prevColor : register(t0);
Texture2D<float> playerDepth : register(t1);
Texture2D<float> depthTex : register(t2);
RWTexture2D<float4> output : register(u0);
#ifdef DAPA_CAPTURE
RWTexture2D<float4> captureClean : register(u1);
RWTexture2D<float4> captureDiagnostic : register(u2);
#endif
SamplerState linearClamp : register(s0);
cbuffer BlackoutParams : register(b1) {
    float2 blackoutCenter;
    float blackoutInner, blackoutMiddle;
    float blackoutScale, blackoutCutoff;
    uint blackoutFlags, blackoutEnabled;
};
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
bool PlayerPixel(float2 uv) {
    if(padding.x<2.5 || any(uv<0) || any(uv>1))return false;
    int2 pixel=clamp(int2(RawUV(uv)*depthResolution),int2(0,0),int2(depthResolution)-1);
    float body=playerDepth.Load(int3(pixel,0));
    float scene=depthTex.Load(int3(pixel,0));
    // Validate against FINAL scene depth: do not mask an occluding wall, or
    // alpha-cutout pixels absent from the game's actual depth pass.
    return body>=0 && body<=1 && isfinite(scene) && abs(body-scene)<=0.000002;
}
// Boundary-repair inverse lookup: solve using depth at the SOURCE, not a mixture
// of corrected and uncorrected UVs at a foreground boundary.
bool ForwardSource(float2 sourceUV,out float2 targetUV,out float z) {
    targetUV=sourceUV;z=0;float3 oldPoint;
    if(any(sourceUV<0)||any(sourceUV>1)||!ViewPosition(sourceUV,ReadDepth(sourceUV),oldPoint))return false;
    oldPoint*=max(depthScale,0.001);
    // The player's attached geometry shares locomotion with the eye. It must
    // not be displaced as static scenery. Animation remains at real-frame cadence.
    if(PlayerPixel(sourceUV)){targetUV=sourceUV;z=abs(oldPoint.z);return true;}
    float3 translation=float3(poseDeltaMatrix[0][3],poseDeltaMatrix[1][3],poseDeltaMatrix[2][3]);
    float3 newPoint=mul(transpose((float3x3)poseDeltaMatrix),oldPoint-translation);
    if(nearFadeDepth>0)newPoint=lerp(oldPoint,newPoint,saturate((abs(oldPoint.z)-nearFadeDepth)/nearFadeDepth));
    float4 clip=mul(float4(newPoint,1),projection);
    if(!all(isfinite(clip))||abs(clip.w)<1e-7||oldPoint.z*newPoint.z<=0)return false;
    targetUV=RawUV(float2(clip.x/clip.w*0.5+0.5,0.5-clip.y/clip.w*0.5));z=abs(newPoint.z);
    return all(isfinite(targetUV));
}
float2 SolveSource(float2 uv,out float confidence,out float2 displacement) {
    if(PlayerPixel(uv)){confidence=1;displacement=0;return uv;}
    float2 cursor=uv,best=uv,background=uv,firstError=0;float bestError=1e20,farZ=0,bestZ=1e20;confidence=0;displacement=0;
    float2 edgeSource=uv;bool exposedEdge=false;
    // A fixed bounded solve; no texture allocations or extra dispatches.
    [loop]for(int i=0;i<4;++i) {
        float2 projected;float z;
        if(!ForwardSource(cursor,projected,z))break;
        float2 error=uv-projected;float pixels=length(error*resolution);
        if(i==0)firstError=error;
        if(pixels<bestError){bestError=pixels;best=cursor;bestZ=z;}
        if(z>farZ){farZ=z;background=cursor;}
        if(pixels<0.001)break;
        float2 next=cursor+error;
        if(length(next-uv)>0.12)break;
        if(any(next<0)||any(next>1)) {
            // The rotated view exposes pixels outside this eye's cached image.
            // Extend its last texel, not an unwarped copy of the old edge strip.
            // Refine along the boundary so tilted turns also keep tangential alignment.
            float2 inset=0.5/resolution;
            edgeSource=clamp(next,inset,1-inset);exposedEdge=true;
            [unroll]for(int edgeStep=0;edgeStep<2;++edgeStep) {
                float2 mapped;float edgeZ;
                if(!ForwardSource(edgeSource,mapped,edgeZ))break;
                float2 refined=clamp(edgeSource+uv-mapped,inset,1-inset);
                if(length(refined-uv)>0.12)break;
                edgeSource=refined;
            }
            break;
        }
        cursor=next;
    }
    // A background root can hide a nearer surface that moved over it. Probe
    // along the displacement and solve that surface too, choosing nearer valid
    // coverage rather than eroding a foreground silhouette.
    [unroll]for(int seed=0;seed<2;++seed) {
        float2 probe=uv+clamp(firstError*(seed==0?4:16),-64.0/resolution,64.0/resolution);
        float2 mapped;float probeZ;
        if(!ForwardSource(probe,mapped,probeZ) || probeZ>=bestZ*0.98)continue;
        [unroll]for(int step=0;step<3;++step) {
            float2 error=uv-mapped;float residual=length(error*resolution);
            if(residual<=1.25 && (bestError>1.25 || probeZ<bestZ*0.999)) {best=probe;bestError=residual;bestZ=probeZ;}
            probe+=error;
            if(length(probe-uv)>0.12 || !ForwardSource(probe,mapped,probeZ))break;
        }
    }
    // No old foreground left behind when a depth edge makes the solver cycle.
    // This is a background extrapolation for a hole, not recovered hidden detail.
    float2 selected=bestError<=1.25?best:(exposedEdge?edgeSource:background);
    confidence=bestError<=1.25?1:0;
    displacement=selected-uv;
    return selected;
}
[numthreads(8,8,1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)resolution)) return;
    float2 uv = (float2(tid.xy)+0.5)/resolution;
    if(blackoutEnabled!=0) {
        float2 delta=2*(uv-blackoutCenter);
        delta.x/=max(blackoutScale,.5);
        float radius=length(delta);
        bool hidden=((blackoutFlags&1)!=0 && radius>blackoutInner && radius<=blackoutMiddle) ||
            ((blackoutFlags&2)!=0 && radius>blackoutMiddle) ||
            ((blackoutFlags&4)!=0 && radius>max(blackoutMiddle,blackoutCutoff));
        if(hidden) {
            output[tid.xy]=float4(0,0,0,1);
#ifdef DAPA_CAPTURE
            captureClean[tid.xy]=float4(0,0,0,1);
            captureDiagnostic[tid.xy]=float4(0,.5,.5,1);
#endif
            return;
        }
    }
    float2 source = uv;
    float captureConfidence = 0;
    float2 captureDisplacement = 0;
    if(predictionValid>0.5)source=SolveSource(uv,captureConfidence,captureDisplacement);
    float4 color=prevColor.SampleLevel(linearClamp,RawUV(source),0);
#ifdef DAPA_CAPTURE
    captureClean[tid.xy]=color;
    // R=solved source confidence; 0 can be background fill, not just unchanged.
    // G/B encode selected UV displacement, zero=0.5.
    captureDiagnostic[tid.xy]=float4(captureConfidence,0.5+captureDisplacement*4,1);
#endif
    if(debugTint>0.5) color.rgb=float3(min(1.0,color.r*1.5+0.1),color.g*0.6,color.b*0.6);
    output[tid.xy]=color;
}
)HLSL";
