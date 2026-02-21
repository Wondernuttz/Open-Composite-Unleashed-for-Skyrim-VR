#pragma once

#include <d3d11.h>

// MIP LOD Bias Correction — hooks PSSetSamplers to fix blurry textures when FSR
// renders at below-native resolution. Applies a negative LOD bias so the GPU
// samples from sharper MIP levels, compensating for the smaller render target.
//
// Only active when FSR is enabled with renderScale < 1.0.
// Uses MinHook (already linked) to vtable-hook ID3D11DeviceContext::PSSetSamplers.

// Install the hook on the given device context. lodBias should be log2(renderScale).
// Returns true if hook was installed successfully.
bool InitMipBiasHook(ID3D11DeviceContext* ctx, float lodBias);

// Update the LOD bias value (clears sampler cache).
void UpdateMipBias(float newBias);

// Set bypass mode — call with true before our own PSSetSamplers calls
// (FSR/DLAA/CAS post-processing), false after.
void SetMipBiasBypass(bool bypass);

// Remove the hook and release all cached samplers.
void ShutdownMipBiasHook();

// Check if the hook is active.
bool IsMipBiasHookActive();
