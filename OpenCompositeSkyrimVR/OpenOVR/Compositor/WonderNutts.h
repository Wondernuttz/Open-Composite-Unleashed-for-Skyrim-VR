#pragma once

#include <d3d11.h>

// ═══════════════════════════════════════════════════════════════════════════════
// OCUnleashed Custom Reconstruction — Foveated Reconstruction Pass
// ═══════════════════════════════════════════════════════════════════════════════
//
// A standalone GPU post-process pass that cleans VRS tile artifacts using
// edge-aware bilateral smoothing that increases in strength from center to
// periphery. Runs at RENDER resolution (before EASU/DLAA/CAS).
//
// Pipeline position:
//   Game + VRS → [render res texture] → WonderNutts → [clean texture] → EASU/DLAA → CAS → display
//
// The algorithm:
//   - Center zone (inside cleanRadius): optional CAS-style sharpening
//   - Transition zone: smoothstep blend from passthrough to reconstruction
//   - Reconstruction: edge-aware bilateral filter (5-tap cross) that preserves
//     real scene edges while smoothing VRS block boundaries
//
// User controls:
//   cleanRadius  — center zone with no processing (the "circle around the eye")
//   strength     — max reconstruction intensity at periphery (the "strength of fade")
//   sharpness    — center zone sharpening (crisp foveal detail)
//
// Self-contained: owns its own shader, constant buffer, sampler, and output
// texture. Does not modify VRSManager, EASU, RCAS, DLAA, or any other system.

class WonderNutts {
public:
	WonderNutts() = default;
	~WonderNutts();

	// Initialize shaders and GPU resources. Call once with the D3D11 device.
	bool Initialize(ID3D11Device* device);

	// Apply foveated reconstruction to the input texture.
	//
	// Parameters:
	//   ctx          — D3D11 immediate context
	//   inputSRV     — the game's rendered frame (VRS artifacts baked in)
	//   width/height — render resolution (NOT output resolution)
	//   projCenterX/Y — eye projection center in normalized UV (0-1)
	//   cleanRadius  — center zone radius where no processing occurs (0.0-1.0)
	//   strength     — max reconstruction blend at periphery (0.0-1.0)
	//   sharpness    — center zone CAS sharpening (0.0-1.0, 0=off)
	//
	// After calling Apply(), use GetOutputSRV() to feed the clean texture
	// into EASU, DLAA, CAS, or directly to the swapchain.
	void Apply(ID3D11DeviceContext* ctx,
	    ID3D11ShaderResourceView* inputSRV,
	    int width, int height,
	    float projCenterX, float projCenterY,
	    float cleanRadius, float strength, float sharpness, float sensitivity);

	// Get the output texture SRV (clean frame at render resolution).
	// Valid after Apply() returns. Feed this into EASU/DLAA/CAS instead of the raw game texture.
	ID3D11ShaderResourceView* GetOutputSRV() const { return outputSRV; }

	// Release all D3D11 resources.
	void Shutdown();

	// Returns true if Initialize() succeeded.
	bool IsReady() const { return ready; }

private:
	bool ready = false;
	ID3D11Device* device = nullptr;

	// Shader objects
	ID3D11PixelShader* pixelShader = nullptr;
	ID3D11VertexShader* vertexShader = nullptr;
	ID3D11Buffer* constantBuffer = nullptr;
	ID3D11SamplerState* pointSampler = nullptr;

	// Output texture at render resolution
	ID3D11Texture2D* outputTex = nullptr;
	ID3D11RenderTargetView* outputRTV = nullptr;
	ID3D11ShaderResourceView* outputSRV = nullptr;
	int texWidth = 0;
	int texHeight = 0;
	DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;

	// Internal helpers
	void EnsureOutputTexture(int width, int height, DXGI_FORMAT format);
	void ReleaseOutputTexture();
};
