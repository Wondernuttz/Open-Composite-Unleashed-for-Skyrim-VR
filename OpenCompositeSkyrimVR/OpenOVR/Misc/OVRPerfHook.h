#pragma once

#include <cstdint>

// Minimal OVR SDK type definitions for ovr_GetPerfStats hooking.
// We only define what we need — no full OVR SDK dependency required.

typedef int32_t ovrResult;
typedef int32_t ovrBool;
typedef uint32_t ovrProcessId;
typedef struct ovrHmdStruct* ovrSession;

static const int ovrMaxProvidedFrameStats = 5;

typedef struct {
	int HmdVsyncIndex;
	int AppFrameIndex;
	int AppDroppedFrameCount;
	float AppMotionToPhotonLatency;
	float AppQueueAheadTime;
	float AppCpuElapsedTime;
	float AppGpuElapsedTime;
	int CompositorFrameIndex;
	int CompositorDroppedFrameCount;
	float CompositorLatency;
	float CompositorCpuElapsedTime;
	float CompositorGpuElapsedTime;
	float CompositorCpuStartToGpuEndElapsedTime;
	float CompositorGpuEndToVsyncElapsedTime;
	ovrBool AswIsActive;
	int AswActivatedToggleCount;
	int AswPresentedFrameCount;
	int AswFailedFrameCount;
} ovrPerfStatsPerCompositorFrame;

typedef struct {
	ovrPerfStatsPerCompositorFrame FrameStats[ovrMaxProvidedFrameStats];
	int FrameStatsCount;
	ovrBool AnyFrameStatsDropped;
	float AdaptiveGpuPerformanceScale;
	ovrBool AswIsAvailable;
	ovrProcessId VisibleProcessId;
} ovrPerfStats;

// Cached snapshot of OVR perf data for consumption by GetFrameTiming()
struct OVRPerfData {
	bool available = false;
	float compositorGpuMs = 0.0f;
	float compositorCpuMs = 0.0f;
	float compositorLatencyMs = 0.0f;
	float compositorCpuToGpuMs = 0.0f;
	float compositorGpuToVsyncMs = 0.0f;
	float appGpuMs = 0.0f;
	float appCpuMs = 0.0f;
	float appMotionToPhotonMs = 0.0f;
	int compositorDroppedFrames = 0;
	int appDroppedFrames = 0;
	bool aswActive = false;
	bool aswAvailable = false;
	float adaptiveGpuScale = 1.0f;
};

// Initialize the OVR perf hook. Call once after the OpenXR session is created.
// Searches for LibOVRRT64_1.dll (or VirtualDesktop.LibOVRRT64_1.dll) in the
// current process, and hooks ovr_GetPerfStats to intercept compositor telemetry.
// Returns true if the hook was installed successfully.
bool InitOVRPerfHook();

// Get the latest captured OVR performance data.
// Thread-safe: returns a snapshot of the most recent data.
OVRPerfData GetOVRPerfData();

// Check if the OVR perf hook is active and providing data.
bool IsOVRPerfHookActive();

// Shutdown and remove the hook. Call on session teardown.
void ShutdownOVRPerfHook();
