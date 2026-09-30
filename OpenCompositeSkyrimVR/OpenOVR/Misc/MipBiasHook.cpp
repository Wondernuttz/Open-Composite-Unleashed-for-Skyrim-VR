#include "stdafx.h"
#include "MipBiasHook.h"
#include "../logging.h"

#ifdef _WIN32

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

using PFN_PSSetSamplers = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numSamplers,
    ID3D11SamplerState* const* ppSamplers);

struct CachedSampler {
	ID3D11SamplerState* original = nullptr;
	ID3D11SamplerState* biased = nullptr;
};

static PFN_PSSetSamplers g_origPSSetSamplers = nullptr;
static void* g_hookTarget = nullptr;
static bool g_initialized = false;
static bool g_enabled = false;
static float g_lodBias = 0.0f;
static std::unordered_map<ID3D11SamplerState*, CachedSampler> g_samplerCache;
static bool g_cacheFlushPending = false;

static void ClearSamplerCache()
{
	for (auto& pair : g_samplerCache) {
		if (pair.second.biased)
			pair.second.biased->Release();
		if (pair.second.original)
			pair.second.original->Release();
	}
	g_samplerCache.clear();
}

static bool ShouldBiasSampler(const D3D11_SAMPLER_DESC& desc)
{
	if (!std::isfinite(desc.MipLODBias) || desc.MipLODBias != 0.0f)
		return false;
	if (desc.ComparisonFunc != D3D11_COMPARISON_NEVER)
		return false;
	if (desc.MaxAnisotropy <= 1)
		return false;
	if (desc.MaxLOD <= desc.MinLOD || desc.MaxLOD <= 0.0f)
		return false;
	return true;
}

static CachedSampler CacheSampler(ID3D11SamplerState* sampler)
{
	CachedSampler entry = {};
	if (!sampler)
		return entry;

	// Bound the cache: games/ENBs that churn sampler objects would otherwise grow
	// this map forever (entries hold refs, raw-pointer keys go stale). Flush is
	// deferred to the start of the next hook call — clearing here could release
	// a biased sampler already borrowed into this call's modified[] array.
	if (g_samplerCache.size() >= 4096)
		g_cacheFlushPending = true;

	entry.original = sampler;
	entry.original->AddRef();

	D3D11_SAMPLER_DESC desc = {};
	sampler->GetDesc(&desc);
	if (!ShouldBiasSampler(desc)) {
		g_samplerCache[sampler] = entry;
		return entry;
	}

	desc.MipLODBias = g_lodBias;

	ID3D11Device* samplerDevice = nullptr;
	sampler->GetDevice(&samplerDevice);
	if (samplerDevice) {
		HRESULT hr = samplerDevice->CreateSamplerState(&desc, &entry.biased);
		samplerDevice->Release();
		if (FAILED(hr) || !entry.biased)
			entry.biased = nullptr;
	}

	g_samplerCache[sampler] = entry;
	return entry;
}

static void STDMETHODCALLTYPE Hook_PSSetSamplers(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numSamplers,
    ID3D11SamplerState* const* ppSamplers)
{
	if (!g_origPSSetSamplers)
		return;

	if (!g_enabled || !ppSamplers || numSamplers == 0
	    || numSamplers > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
		g_origPSSetSamplers(ctx, startSlot, numSamplers, ppSamplers);
		return;
	}

	// Safe point for the deferred cache flush: nothing borrowed from the map yet
	if (g_cacheFlushPending) {
		ClearSamplerCache();
		g_cacheFlushPending = false;
	}

	ID3D11SamplerState* modified[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
	bool anyModified = false;

	for (UINT i = 0; i < numSamplers; i++) {
		ID3D11SamplerState* sampler = ppSamplers[i];
		modified[i] = sampler;
		if (!sampler)
			continue;

		auto it = g_samplerCache.find(sampler);
		CachedSampler entry = (it != g_samplerCache.end()) ? it->second : CacheSampler(sampler);
		if (entry.biased) {
			modified[i] = entry.biased;
			anyModified = true;
		}
	}

	if (anyModified)
		g_origPSSetSamplers(ctx, startSlot, numSamplers, modified);
	else
		g_origPSSetSamplers(ctx, startSlot, numSamplers, ppSamplers);
}

bool InitMipBiasHook(ID3D11DeviceContext* ctx)
{
	if (g_initialized)
		return true;
	if (!ctx)
		return false;

	MH_STATUS st = MH_Initialize();
	if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
		OOVR_LOGF("MipBias: MH_Initialize failed (%d)", (int)st);
		return false;
	}

	void** vtable = *reinterpret_cast<void***>(ctx);
	g_hookTarget = vtable[10]; // ID3D11DeviceContext::PSSetSamplers

	st = MH_CreateHook(g_hookTarget, reinterpret_cast<void*>(&Hook_PSSetSamplers),
	    reinterpret_cast<void**>(&g_origPSSetSamplers));
	if (st != MH_OK) {
		OOVR_LOGF("MipBias: MH_CreateHook PSSetSamplers failed (%d)", (int)st);
		g_hookTarget = nullptr;
		return false;
	}

	st = MH_EnableHook(g_hookTarget);
	if (st != MH_OK) {
		OOVR_LOGF("MipBias: MH_EnableHook PSSetSamplers failed (%d)", (int)st);
		MH_RemoveHook(g_hookTarget);
		g_hookTarget = nullptr;
		g_origPSSetSamplers = nullptr;
		return false;
	}

	g_initialized = true;
	OOVR_LOG("MipBias: PSSetSamplers hook installed");
	return true;
}

void ConfigureMipBiasHook(bool enabled, float lodBias)
{
	if (!std::isfinite(lodBias))
		lodBias = 0.0f;

	if (enabled != g_enabled || lodBias != g_lodBias) {
		ClearSamplerCache();
		OOVR_LOGF("MipBias: %s, LOD bias %.3f", enabled ? "enabled" : "disabled", lodBias);
	}

	g_enabled = enabled;
	g_lodBias = lodBias;
}

void ShutdownMipBiasHook()
{
	if (!g_initialized)
		return;

	ConfigureMipBiasHook(false, 0.0f);

	if (g_hookTarget) {
		MH_DisableHook(g_hookTarget);
		MH_RemoveHook(g_hookTarget);
	}

	g_hookTarget = nullptr;
	g_origPSSetSamplers = nullptr;
	g_initialized = false;
	OOVR_LOG("MipBias: hook removed");
}

bool IsMipBiasHookActive()
{
	return g_initialized;
}

#else

bool InitMipBiasHook(ID3D11DeviceContext*) { return false; }
void ConfigureMipBiasHook(bool, float) {}
void ShutdownMipBiasHook() {}
bool IsMipBiasHookActive() { return false; }

#endif
