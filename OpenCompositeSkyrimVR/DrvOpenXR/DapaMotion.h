#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// CPU-only, allocation-free prediction. Times are OpenXR nanoseconds, never CPU
// wall clock: predict from a REAL frame to the requested display slot.
namespace DapaMotion {
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator-(Vec3 b) const { return {x-b.x, y-b.y, z-b.z}; }
    Vec3 operator*(float s) const { return {x*s, y*s, z*s}; }
};
inline float Length(Vec3 v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
inline bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Predictor {
    int64_t time = 0;
    double interval = 0;
    Vec3 position{}, velocity{};
    float confidence = 0;
    bool havePosition = false, haveVelocity = false;
    void Reset() { *this = {}; }
    void Sample(int64_t stamp, Vec3 p) {
        if (stamp <= 0 || !Finite(p)) { Reset(); return; }
        const double dt = double(stamp - time) * 1e-9;
        const Vec3 delta = p - position;
        const bool valid = havePosition && dt >= 0.004 && dt <= 0.100
            && Length(delta) <= 15.0f; // preserve the existing teleport guard
        time = stamp; position = p; havePosition = true; interval = dt;
        if (!valid) { velocity = {}; confidence = 0; haveVelocity = false; return; }
        const Vec3 next = delta * float(1.0 / dt);
        const float speed = Length(next);
        // Never coast after release. Acceleration / reversal reduces confidence,
        // not the measured velocity (a low-pass velocity would leave a trail).
        if (speed < 0.1f) { velocity = {}; confidence = 0; haveVelocity = true; return; }
        const float change = haveVelocity ? Length(next - velocity) : speed;
        confidence = haveVelocity ? std::clamp(1.0f - change / std::max(speed, 1.0f), 0.0f, 1.0f) : 0.0f;
        velocity = next; haveVelocity = true;
    }
    float Fraction(int64_t target) const {
        const double ahead = double(target - time) * 1e-9;
        if (!haveVelocity || interval <= 0 || ahead <= 0 || ahead > 0.050 || ahead > interval)
            return 0;
        return float(ahead / interval);
    }
    Vec3 Predict(int64_t target) const {
        return velocity * (float(interval) * Fraction(target) * confidence);
    }
};

// Actor heading only: never infer a game turn from an XR headset pose.
// Keep angular timing independent of translation (turning in place is valid).
struct YawPredictor {
    int64_t time=0;
    double interval=0;
    float yaw=0,delta=0,rate=0,confidence=0;
    bool haveYaw=false,haveRate=false,turning=false;
    void Reset() { *this={}; }
    void Sample(int64_t stamp,float heading,bool turnActive) {
        if(stamp<=0 || !std::isfinite(heading)){Reset();return;}
        const double dt=double(stamp-time)*1e-9;
        const float step=std::remainder(yaw-heading,6.283185307f);
        const bool valid=haveYaw && dt>=0.004 && dt<=0.100 && std::abs(step)<=0.5f;
        const bool priorRate=haveRate;const float previousRate=rate;
        time=stamp;interval=dt;yaw=heading;haveYaw=true;turning=turnActive;
        delta=rate=confidence=0;haveRate=false;
        if(!valid || !turnActive)return; // stop immediately; snap/gap is not smooth motion
        delta=step;rate=step/float(dt);haveRate=true;
        if(std::abs(rate)<0.001f)return;
        confidence=priorRate?std::clamp(1-std::abs(rate-previousRate)/std::max(std::abs(rate),0.001f),0.0f,1.0f):0;
    }
    float Predict(int64_t target) const {
        const double ahead=double(target-time)*1e-9;
        if(!haveRate || !turning || ahead<=0 || ahead>0.050 || ahead>interval)return 0;
        return std::clamp(rate*float(ahead)*confidence,-0.12f,0.12f);
    }
};

// Conjugate a Skyrim world-Z rotation into the cached eye's view basis.
// This handles head pitch/roll, world scale and reflected (LH) view bases.
// Output is a column-vector NEW-view -> OLD-view rotation; translation is untouched.
inline bool WorldYawToView(float radians,const float* view,float* transform) {
    if(!std::isfinite(radians))return false;
    float basis[3][3];
    for(int r=0;r<3;++r) {
        const float length=std::sqrt(view[r]*view[r]+view[4+r]*view[4+r]+view[8+r]*view[8+r]);
        if(!std::isfinite(length)||length<1e-6f)return false;
        for(int c=0;c<3;++c)basis[r][c]=view[c*4+r]/length;
    }
    for(int r=0;r<3;++r)for(int c=0;c<r;++c) {
        float dot=0;for(int k=0;k<3;++k)dot+=basis[r][k]*basis[c][k];
        if(!std::isfinite(dot)||std::abs(dot)>0.001f)return false;
    }
    const float c=std::cos(radians),s=std::sin(radians);
    const float rotation[3][3]={{c,-s,0},{s,c,0},{0,0,1}};
    for(int r=0;r<3;++r)for(int col=0;col<3;++col) {
        float value=0;
        for(int j=0;j<3;++j)for(int k=0;k<3;++k)value+=basis[r][j]*rotation[j][k]*basis[col][k];
        transform[r*4+col]=value;
    }
    return true;
}

// Row-vector DirectX matrices. Recover P from V * P, rather than guessing
// near/far, reversed Z, handedness, asymmetric FOV or world scale.
inline bool Inverse(const float* src, float* dst) {
    double a[4][8]{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
        if (!std::isfinite(src[r*4+c])) return false;
        a[r][c]=src[r*4+c]; a[r][c+4]=(r==c);
    }
    for (int c=0;c<4;++c) {
        int pivot=c;
        for (int r=c+1;r<4;++r) if (std::abs(a[r][c])>std::abs(a[pivot][c])) pivot=r;
        if (std::abs(a[pivot][c])<1e-12) return false;
        for (int j=0;j<8;++j) std::swap(a[c][j],a[pivot][j]);
        const double d=a[c][c]; for (int j=0;j<8;++j) a[c][j]/=d;
        for (int r=0;r<4;++r) if(r!=c) {
            const double s=a[r][c]; for(int j=0;j<8;++j) a[r][j]-=s*a[c][j];
        }
    }
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) {
        dst[r*4+c]=float(a[r][c+4]); if(!std::isfinite(dst[r*4+c])) return false;
    }
    return true;
}
inline bool Projection(const float* view, const float* vp, float* p, float* invP) {
    float invV[16];
    if (!Inverse(view,invV)) return false;
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) {
        double v=0; for(int k=0;k<4;++k) v+=double(invV[r*4+k])*vp[k*4+c];
        p[r*4+c]=float(v);
    }
    return Inverse(p,invP);
}
inline Vec3 ToView(Vec3 world, const float* view) {
    return {world.x*view[0]+world.y*view[4]+world.z*view[8],
        world.x*view[1]+world.y*view[5]+world.z*view[9],
        world.x*view[2]+world.y*view[6]+world.z*view[10]};
}
} // namespace DapaMotion
