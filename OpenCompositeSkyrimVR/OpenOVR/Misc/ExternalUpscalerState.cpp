#include "stdafx.h"
#include "ExternalUpscalerState.h"
#include "../logging.h"

#ifdef _WIN32

#include <atomic>
#include <cmath>

static HANDLE g_mapping = nullptr;
static OCUExternalUpscalerState* g_state = nullptr;
static uint32_t g_lastActive = UINT32_MAX;
static uint32_t g_lastMethod = UINT32_MAX;
static uint32_t g_lastFlags = UINT32_MAX;
static uint32_t g_lastDlssPreset = UINT32_MAX;
static float g_lastRenderScale = -1.0f;
static float g_lastMipBias = 999.0f;

static bool NearlyEqual(float a, float b)
{
	return std::fabs(a - b) < 0.0005f;
}

static const char* UpscalerMethodName(OCUExternalUpscalerMethod method)
{
	switch (method) {
	case OCU_EXTERNAL_UPSCALER_DLSS:
		return "DLSS";
	case OCU_EXTERNAL_UPSCALER_FSR3:
		return "FSR3";
	case OCU_EXTERNAL_UPSCALER_FSR_NATIVE_AA:
		return "FSR Native AA";
	case OCU_EXTERNAL_UPSCALER_DLAA:
		return "DLAA";
	default:
		return "None";
	}
}

static bool EnsureExternalUpscalerState()
{
	if (g_state)
		return true;

	g_mapping = CreateFileMappingW(
	    INVALID_HANDLE_VALUE,
	    nullptr,
	    PAGE_READWRITE,
	    0,
	    sizeof(OCUExternalUpscalerState),
	    OCU_EXTERNAL_UPSCALER_STATE_NAME);
	if (!g_mapping) {
		OOVR_LOGF("ExternalUpscalerState: CreateFileMapping failed (%lu)", GetLastError());
		return false;
	}

	g_state = reinterpret_cast<OCUExternalUpscalerState*>(
	    MapViewOfFile(g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(OCUExternalUpscalerState)));
	if (!g_state) {
		OOVR_LOGF("ExternalUpscalerState: MapViewOfFile failed (%lu)", GetLastError());
		CloseHandle(g_mapping);
		g_mapping = nullptr;
		return false;
	}

	return true;
}

void PublishExternalUpscalerState(
    bool active,
    OCUExternalUpscalerMethod method,
    float renderScale,
    float mipBias,
    float mipBiasOffset,
    uint32_t dlssPreset,
    uint32_t flags)
{
	if (!EnsureExternalUpscalerState())
		return;

	if (!std::isfinite(renderScale))
		renderScale = 1.0f;
	if (!std::isfinite(mipBias))
		mipBias = 0.0f;
	if (!std::isfinite(mipBiasOffset))
		mipBiasOffset = -1.0f;

	uint32_t nextCounter = g_state->updateCounter + 1;
	if ((nextCounter & 1u) == 0)
		nextCounter++;

	g_state->updateCounter = nextCounter;
	std::atomic_thread_fence(std::memory_order_release);

	g_state->magic = OCU_EXTERNAL_UPSCALER_STATE_MAGIC;
	g_state->version = OCU_EXTERNAL_UPSCALER_STATE_VERSION;
	g_state->byteSize = sizeof(OCUExternalUpscalerState);
	g_state->active = active ? 1u : 0u;
	g_state->method = static_cast<uint32_t>(method);
	g_state->dlssPreset = dlssPreset;
	g_state->flags = flags;
	g_state->renderScale = renderScale;
	g_state->mipBias = mipBias;
	g_state->mipBiasOffset = mipBiasOffset;

	std::atomic_thread_fence(std::memory_order_release);
	g_state->updateCounter = nextCounter + 1;

	if (g_lastActive != g_state->active
	    || g_lastMethod != g_state->method
	    || g_lastFlags != g_state->flags
	    || g_lastDlssPreset != g_state->dlssPreset
	    || !NearlyEqual(g_lastRenderScale, renderScale)
	    || !NearlyEqual(g_lastMipBias, mipBias)) {
		OOVR_LOGF("ExternalUpscalerState: active=%u method=%s renderScale=%.3f mipBias=%.3f dlssPreset=%u flags=0x%X",
		    g_state->active,
		    UpscalerMethodName(method),
		    renderScale,
		    mipBias,
		    dlssPreset,
		    flags);

		g_lastActive = g_state->active;
		g_lastMethod = g_state->method;
		g_lastFlags = g_state->flags;
		g_lastDlssPreset = g_state->dlssPreset;
		g_lastRenderScale = renderScale;
		g_lastMipBias = mipBias;
	}
}

void ShutdownExternalUpscalerState()
{
	if (g_state) {
		PublishExternalUpscalerState(
		    false,
		    OCU_EXTERNAL_UPSCALER_NONE,
		    1.0f,
		    0.0f,
		    -1.0f,
		    0,
		    0);
		UnmapViewOfFile(g_state);
		g_state = nullptr;
	}

	if (g_mapping) {
		CloseHandle(g_mapping);
		g_mapping = nullptr;
	}
}

#else

void PublishExternalUpscalerState(
    bool,
    OCUExternalUpscalerMethod,
    float,
    float,
    float,
    uint32_t,
    uint32_t)
{
}
void ShutdownExternalUpscalerState() {}

#endif
