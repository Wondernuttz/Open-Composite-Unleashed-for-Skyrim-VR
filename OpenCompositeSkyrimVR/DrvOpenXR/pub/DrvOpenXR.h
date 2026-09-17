#pragma once
#include <cstdint>
#include "../OpenOVR/Drivers/Backend.h"

// Since we're not importing stdafx, add another nasty little hack
#ifndef _WIN32
#include "../OpenOVR/linux_funcs.h"
#endif

namespace DrvOpenXR {
IBackend* CreateOpenXRBackend();
void GetXRAppName(char (&appName)[128]);
// Revision advertised by the enabled HTCX interaction extension; zero if absent.
uint32_t GetViveTrackerInteractionVersion();
}; // namespace DrvOpenXR
