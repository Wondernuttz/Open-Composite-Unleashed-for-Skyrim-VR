#include "stdafx.h"
#include "MipBiasHook.h"
#include "../logging.h"

#ifdef _WIN32

#include <MinHook.h>
#include <unordered_map>
#include <unordered_set>

// Original function pointer (trampoline)
typedef void (STDMETHODCALLTYPE *PFN_PSSetSamplers)(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numSamplers,
    ID3D11SamplerState* const* ppSamplers);

static PFN_PSSetSamplers g_origPSSetSamplers = nullptr;

// State
static float g_lodBias = 0.0f;
static bool g_bypass = false;
static bool g_initialized = false;
static ID3D11Device* g_device = nullptr;
static LPVOID g_hookTarget = nullptr;

// Cache: original sampler → cloned sampler with LOD bias applied
static std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_cache;

// Pass-through: samplers we've inspected and decided to skip
static std::unordered_set<ID3D11SamplerState*> g_passThrough;

static void ClearCache()
{
	for (auto& pair : g_cache) {
		if (pair.second)
			pair.second->Release();
	}
	g_cache.clear();
	g_passThrough.clear();
}

// ── Detour: intercept game's PSSetSamplers calls and apply LOD bias ──
static void STDMETHODCALLTYPE Hook_PSSetSamplers(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numSamplers,
    ID3D11SamplerState* const* ppSamplers)
{
	// Bypass during our own post-processing (FSR/DLAA/CAS set their own samplers)
	if (g_bypass || !ppSamplers || numSamplers == 0) {
		g_origPSSetSamplers(ctx, startSlot, numSamplers, ppSamplers);
		return;
	}

	// Work with a local copy so we can substitute samplers
	ID3D11SamplerState* modified[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
	bool anyModified = false;

	for (UINT i = 0; i < numSamplers && i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
		modified[i] = ppSamplers[i];

		if (!ppSamplers[i])
			continue;

		// Already decided to skip this sampler?
		if (g_passThrough.count(ppSamplers[i]))
			continue;

		// Already have a cached clone?
		auto it = g_cache.find(ppSamplers[i]);
		if (it != g_cache.end()) {
			modified[i] = it->second;
			anyModified = true;
			continue;
		}

		// Inspect the sampler to decide whether to modify it
		D3D11_SAMPLER_DESC desc;
		ppSamplers[i]->GetDesc(&desc);

		// Skip samplers that already have a LOD bias (intentional, don't override)
		// Skip samplers with no real anisotropic filtering (bias irrelevant)
		if (desc.MipLODBias != 0.0f || desc.MaxAnisotropy <= 1) {
			g_passThrough.insert(ppSamplers[i]);
			continue;
		}

		// Clone with LOD bias applied
		desc.MipLODBias = g_lodBias;

		ID3D11SamplerState* biased = nullptr;
		HRESULT hr = g_device->CreateSamplerState(&desc, &biased);
		if (SUCCEEDED(hr) && biased) {
			g_cache[ppSamplers[i]] = biased;
			// Also mark the biased sampler as pass-through so if the game
			// somehow receives it back, we don't double-modify
			g_passThrough.insert(biased);
			modified[i] = biased;
			anyModified = true;
		} else {
			// Creation failed — pass through the original
			g_passThrough.insert(ppSamplers[i]);
		}
	}

	if (anyModified)
		g_origPSSetSamplers(ctx, startSlot, numSamplers, modified);
	else
		g_origPSSetSamplers(ctx, startSlot, numSamplers, ppSamplers);
}

bool InitMipBiasHook(ID3D11DeviceContext* ctx, float lodBias)
{
	if (g_initialized)
		return true;

	if (!ctx)
		return false;

	// Get the device for creating cloned samplers
	ctx->GetDevice(&g_device);
	if (!g_device) {
		OOVR_LOG("MipBiasHook: Failed to get ID3D11Device from context");
		return false;
	}

	g_lodBias = lodBias;

	// Initialize MinHook (safe to call if already initialized by OVRPerfHook)
	MH_STATUS mhStatus = MH_Initialize();
	if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
		OOVR_LOGF("MipBiasHook: MH_Initialize failed (status %d)", (int)mhStatus);
		g_device->Release();
		g_device = nullptr;
		return false;
	}

	// Extract PSSetSamplers from the device context vtable (index 10)
	LPVOID* vtable = *((LPVOID**)ctx);
	g_hookTarget = vtable[10];

	mhStatus = MH_CreateHook(g_hookTarget, (LPVOID)&Hook_PSSetSamplers, (LPVOID*)&g_origPSSetSamplers);
	if (mhStatus != MH_OK) {
		OOVR_LOGF("MipBiasHook: MH_CreateHook failed (status %d)", (int)mhStatus);
		g_device->Release();
		g_device = nullptr;
		g_hookTarget = nullptr;
		return false;
	}

	mhStatus = MH_EnableHook(g_hookTarget);
	if (mhStatus != MH_OK) {
		OOVR_LOGF("MipBiasHook: MH_EnableHook failed (status %d)", (int)mhStatus);
		MH_RemoveHook(g_hookTarget);
		g_device->Release();
		g_device = nullptr;
		g_hookTarget = nullptr;
		g_origPSSetSamplers = nullptr;
		return false;
	}

	g_initialized = true;
	OOVR_LOGF("MipBiasHook: Installed — LOD bias = %.3f", g_lodBias);
	return true;
}

void UpdateMipBias(float newBias)
{
	if (!g_initialized)
		return;

	if (newBias == g_lodBias)
		return;

	OOVR_LOGF("MipBiasHook: Bias changed %.3f → %.3f, clearing cache", g_lodBias, newBias);
	ClearCache();
	g_lodBias = newBias;
}

void SetMipBiasBypass(bool bypass)
{
	g_bypass = bypass;
}

void ShutdownMipBiasHook()
{
	if (!g_initialized)
		return;

	if (g_hookTarget) {
		MH_DisableHook(g_hookTarget);
		MH_RemoveHook(g_hookTarget);
	}

	ClearCache();

	if (g_device) {
		g_device->Release();
		g_device = nullptr;
	}

	g_origPSSetSamplers = nullptr;
	g_hookTarget = nullptr;
	g_initialized = false;
	g_bypass = false;
	g_lodBias = 0.0f;

	OOVR_LOG("MipBiasHook: Shut down");
}

bool IsMipBiasHookActive()
{
	return g_initialized;
}

#else // !_WIN32

bool InitMipBiasHook(ID3D11DeviceContext*, float) { return false; }
void UpdateMipBias(float) {}
void SetMipBiasBypass(bool) {}
void ShutdownMipBiasHook() {}
bool IsMipBiasHookActive() { return false; }

#endif
