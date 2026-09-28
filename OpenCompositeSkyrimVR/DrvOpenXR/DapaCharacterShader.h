#pragma once
inline constexpr char s_characterShaderHLSL[] = R"HLSL(
Texture2D<float4> prevColor : register(t0);
Texture2D<float> playerDepth : register(t1);
Texture2D<float> depthTex : register(t2);
Texture2D<float4> nativePlayerVectors : register(t3);
StructuredBuffer<uint2> tiles : register(t4);
RWTexture2D<float4> output : register(u0);
AppendStructuredBuffer<uint2> activeTiles : register(u1);
RWTexture2D<float2> residualOutput : register(u2);
SamplerState linearClamp : register(s0);
cbuffer NativePlayerParams : register(b0) {
    row_major float4x4 playerClipToPrevious;
    row_major float4x4 playerClipToCurrent;
    float2 playerVectorSize;
    float playerFraction,playerEnabled;
};
cbuffer TileParams : register(b1) {
    float2 resolution,depthResolution;
    float2 sourceFlip;
    float2 reserved;
};
float2 RawUV(float2 uv) { return lerp(uv, 1.0 - uv, sourceFlip); }
float ReadDepth(float2 uv) {
    int2 limit = int2(depthResolution) - 1;
    return depthTex.Load(int3(clamp(int2(RawUV(uv)*depthResolution), int2(0,0), limit), 0));
}
bool ScenePlayerPixel(float2 uv) {
    if(any(uv<0) || any(uv>1))return false;
    int2 pixel=clamp(int2(RawUV(uv)*depthResolution),int2(0,0),int2(depthResolution)-1);
    float body=playerDepth.Load(int3(pixel,0));
    float scene=depthTex.Load(int3(pixel,0));
    // Validate against FINAL scene depth: do not mask an occluding wall, or
    // alpha-cutout pixels absent from the game's actual depth pass.
    return body>=0 && body<=1 && isfinite(scene) && abs(body-scene)<=0.000002;
}

bool ComputeResidual(float2 uv,float scene,out float2 forward) {
    forward=0;
    if(any(uv<=0)||any(uv>=1))return false;
    float2 raw=RawUV(uv);
    float4 clip=float4(raw.x*2-1,1-raw.y*2,scene,1);
    float4 previous=mul(clip,playerClipToPrevious),current=mul(clip,playerClipToCurrent);
    if(!all(isfinite(previous))||!all(isfinite(current))||previous.w<=1e-7||current.w<=1e-7)return false;
    float2 camera=(previous.xy/previous.w-current.xy/current.w)*float2(.5,-.5);
    int2 pixel=clamp(int2(raw*playerVectorSize),0,int2(playerVectorSize)-1);
    float2 native=nativePlayerVectors.Load(int3(pixel,0)).xy;
    if(!all(isfinite(native))||any(abs(native)>.2))return false;
    // A cleared vector cannot establish object motion during a moving-camera frame.
    if(dot(native,native)<1e-14 && length(camera*resolution)>.25)return false;
    forward=-(native-camera);
    forward*=1-2*sourceFlip;
    if(!all(isfinite(forward))||any(abs(forward)>=1))return false;
    return true;
}
// R16G16_FLOAT: ordinary values are residual UV motion, (2,2) is a
// visible player pixel without usable vectors, (3,3) is outside the mask.
// Point loads preserve mask boundaries; no motion is interpolated across them.
[numthreads(8,8,1)]
void PrepareResidual(uint3 pixel : SV_DispatchThreadID) {
    if(any(pixel.xy>=uint2(depthResolution)))return;
    float2 uv=RawUV((float2(pixel.xy)+.5)/depthResolution);
    float2 value=float2(3,3),motion;
    float body=playerDepth.Load(int3(pixel.xy,0));
    float scene=depthTex.Load(int3(pixel.xy,0));
    if(body>=0 && body<=1 && isfinite(scene) && abs(body-scene)<=.000002)
        value=ComputeResidual(uv,scene,motion)?motion:float2(2,2);
    residualOutput[pixel.xy]=value;
}
float2 Prepared(float2 uv) {
    if(any(uv<0)||any(uv>=1))return float2(3,3);
    int2 pixel=clamp(int2(RawUV(uv)*depthResolution),0,int2(depthResolution)-1);
    return nativePlayerVectors.Load(int3(pixel,0)).xy;
}
bool PlayerPixel(float2 uv) { return Prepared(uv).x<2.5; }
bool PlayerResidual(float2 uv,out float2 forward) {
    forward=Prepared(uv);
    if(any(abs(forward)>=1))return false;
    forward*=playerFraction;
    return dot(forward*resolution,forward*resolution)<=24*24;
}
float2 PlayerSource(float2 uv,out float confidence,out float2 displacement) {
    confidence=0;displacement=0;
    float2 motion;
    if(!PlayerResidual(uv,motion)||length(motion*resolution)<.15)return uv;
    float2 source=uv-motion;
    // One backward estimate, then validate its forward correspondence.
    // Reject rapidly changing motion instead of iterating across a silhouette.
    if(!PlayerResidual(source,motion)||length((source+motion-uv)*resolution)>1.0)return uv;
    // Preserve silhouette support: no background filling, unmasked expansion,
    // or bilinear samples that pull world colours across the player boundary.
    float2 support=max(1.0/resolution,1.0/depthResolution);
    [unroll]for(int y=-1;y<=1;y+=2)[unroll]for(int x=-1;x<=1;x+=2) {
        if(!PlayerPixel(source+float2(x,y)*support)||!PlayerPixel(uv+float2(x,y)*support))return uv;
    }
    confidence=1;displacement=source-uv;return source;
}


groupshared uint occupied;
[numthreads(8,8,1)]
void BuildTiles(uint3 group : SV_GroupID,uint3 thread : SV_GroupThreadID) {
    if(thread.x==0 && thread.y==0)occupied=0;
    GroupMemoryBarrierWithGroupSync();
    float2 first=RawUV((float2(group.xy*32)+.5)/resolution);
    float2 last=RawUV((min(float2(group.xy*32+31),resolution-1)+.5)/resolution);
    uint2 lo=uint2(clamp(min(first,last)*depthResolution,0,depthResolution-1));
    uint2 hi=uint2(clamp(max(first,last)*depthResolution,0,depthResolution-1));
    bool found=false;
    for(uint y=lo.y+thread.y;y<=hi.y;y+=8)for(uint x=lo.x+thread.x;x<=hi.x;x+=8) {
        float2 motion=nativePlayerVectors.Load(int3(x,y,0)).xy;
        if(all(abs(motion)<1) && dot(motion*resolution,motion*resolution)>=.15*.15)found=true;
    }
    if(found)InterlockedOr(occupied,1);
    GroupMemoryBarrierWithGroupSync();
    if(thread.x==0 && thread.y==0 && occupied)activeTiles.Append(group.xy);
}
[numthreads(8,8,1)]
void CorrectCharacter(uint3 group : SV_GroupID,uint3 thread : SV_GroupThreadID) {
    if(playerEnabled<.5)return;
    // Sixteen independent 8x8 groups cover one admitted 32x32 tile.
    // Each thread handles one pixel, rather than sixteen serial pixels.
    uint2 start=tiles[group.x]*32+uint2(group.y%4,group.y/4)*8;
    {
        uint2 pixel=start+thread.xy;
        if(any(pixel>=uint2(resolution)))return;
        float2 uv=(float2(pixel)+.5)/resolution;
        // This is the only output write. World and rejected mask pixels are untouched.
        if(!PlayerPixel(uv))return;
        float confidence;float2 displacement;
        float2 source=PlayerSource(uv,confidence,displacement);
        if(confidence>.5)output[pixel]=prevColor.SampleLevel(linearClamp,RawUV(source),0);
    }
}
)HLSL";
