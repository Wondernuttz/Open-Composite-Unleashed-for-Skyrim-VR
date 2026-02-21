#include "stdafx.h"
#include "OVRPerfHook.h"
#include "../logging.h"

#ifdef _WIN32

#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <mutex>

// Function pointer type for ovr_GetPerfStats
typedef ovrResult(__cdecl* PFN_ovr_GetPerfStats)(ovrSession session, ovrPerfStats* outStats);

// Original function pointer (trampoline)
static PFN_ovr_GetPerfStats g_origGetPerfStats = nullptr;

// Latest captured data
static OVRPerfData g_latestPerfData;
static std::mutex g_perfDataMutex;

// State tracking
static std::atomic<bool> g_hookInstalled{ false };
static std::atomic<bool> g_minhookInitialized{ false };
static HMODULE g_ovrModule = nullptr;

// Our detour function: intercepts ovr_GetPerfStats, calls original, captures data
static ovrResult __cdecl Hook_ovr_GetPerfStats(ovrSession session, ovrPerfStats* outStats)
{
	ovrResult result = g_origGetPerfStats(session, outStats);

	if (result == 0 && outStats != nullptr && outStats->FrameStatsCount > 0) { // ovrSuccess == 0
		const ovrPerfStatsPerCompositorFrame& frame = outStats->FrameStats[0]; // Most recent

		std::lock_guard<std::mutex> lock(g_perfDataMutex);
		g_latestPerfData.available = true;
		g_latestPerfData.compositorGpuMs = frame.CompositorGpuElapsedTime * 1000.0f;
		g_latestPerfData.compositorCpuMs = frame.CompositorCpuElapsedTime * 1000.0f;
		g_latestPerfData.compositorLatencyMs = frame.CompositorLatency * 1000.0f;
		g_latestPerfData.compositorCpuToGpuMs = frame.CompositorCpuStartToGpuEndElapsedTime * 1000.0f;
		g_latestPerfData.compositorGpuToVsyncMs = frame.CompositorGpuEndToVsyncElapsedTime * 1000.0f;
		g_latestPerfData.appGpuMs = frame.AppGpuElapsedTime * 1000.0f;
		g_latestPerfData.appCpuMs = frame.AppCpuElapsedTime * 1000.0f;
		g_latestPerfData.appMotionToPhotonMs = frame.AppMotionToPhotonLatency * 1000.0f;
		g_latestPerfData.compositorDroppedFrames = frame.CompositorDroppedFrameCount;
		g_latestPerfData.appDroppedFrames = frame.AppDroppedFrameCount;
		g_latestPerfData.aswActive = (frame.AswIsActive != 0);
		g_latestPerfData.aswAvailable = (outStats->AswIsAvailable != 0);
		g_latestPerfData.adaptiveGpuScale = outStats->AdaptiveGpuPerformanceScale;
	}

	return result;
}

bool InitOVRPerfHook()
{
	if (g_hookInstalled.load())
		return true;

	// Initialize MinHook
	if (!g_minhookInitialized.load()) {
		if (MH_Initialize() != MH_OK) {
			OOVR_LOG("OVRPerfHook: Failed to initialize MinHook");
			return false;
		}
		g_minhookInitialized.store(true);
	}

	// Search for the OVR runtime DLL in the current process
	// VirtualDesktop may use its own renamed copy
	const wchar_t* dllNames[] = {
		L"LibOVRRT64_1.dll",
		L"VirtualDesktop.LibOVRRT64_1.dll",
	};

	HMODULE hModule = nullptr;
	for (const wchar_t* name : dllNames) {
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &hModule)) {
			OOVR_LOGF("OVRPerfHook: Found OVR runtime: %ls", name);
			break;
		}
		hModule = nullptr;
	}

	if (!hModule) {
		OOVR_LOG("OVRPerfHook: No OVR runtime DLL found in process — hook not installed (non-Oculus/VD runtime?)");
		return false;
	}

	g_ovrModule = hModule;

	// Find ovr_GetPerfStats export
	FARPROC target = GetProcAddress(hModule, "ovr_GetPerfStats");
	if (!target) {
		OOVR_LOG("OVRPerfHook: ovr_GetPerfStats not found in OVR runtime DLL");
		return false;
	}

	// Install the hook
	MH_STATUS status = MH_CreateHook(
		(LPVOID)target,
		(LPVOID)&Hook_ovr_GetPerfStats,
		(LPVOID*)&g_origGetPerfStats);

	if (status != MH_OK) {
		OOVR_LOGF("OVRPerfHook: MH_CreateHook failed (status %d)", (int)status);
		return false;
	}

	status = MH_EnableHook((LPVOID)target);
	if (status != MH_OK) {
		OOVR_LOGF("OVRPerfHook: MH_EnableHook failed (status %d)", (int)status);
		MH_RemoveHook((LPVOID)target);
		return false;
	}

	g_hookInstalled.store(true);
	OOVR_LOG("OVRPerfHook: Successfully hooked ovr_GetPerfStats — real compositor timing active");

	return true;
}

OVRPerfData GetOVRPerfData()
{
	std::lock_guard<std::mutex> lock(g_perfDataMutex);
	return g_latestPerfData;
}

bool IsOVRPerfHookActive()
{
	if (!g_hookInstalled.load())
		return false;

	std::lock_guard<std::mutex> lock(g_perfDataMutex);
	return g_latestPerfData.available;
}

void ShutdownOVRPerfHook()
{
	if (g_hookInstalled.load()) {
		if (g_ovrModule) {
			FARPROC target = GetProcAddress(g_ovrModule, "ovr_GetPerfStats");
			if (target) {
				MH_DisableHook((LPVOID)target);
				MH_RemoveHook((LPVOID)target);
			}
		}
		g_hookInstalled.store(false);
		OOVR_LOG("OVRPerfHook: Hook removed");
	}

	if (g_minhookInitialized.load()) {
		MH_Uninitialize();
		g_minhookInitialized.store(false);
	}

	g_origGetPerfStats = nullptr;
	g_ovrModule = nullptr;
}

#else // !_WIN32

// Non-Windows: stubs
bool InitOVRPerfHook() { return false; }
OVRPerfData GetOVRPerfData() { return OVRPerfData{}; }
bool IsOVRPerfHookActive() { return false; }
void ShutdownOVRPerfHook() {}

#endif
