#pragma once

#include <cstdint>
#include <cstddef>
#include "BodyTrackerRoles.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

// Read-only status for the desktop Configurator. These are observations of
// role poses returned by the runtime, not a physical device inventory. No
// positions or user/device identifiers are exported. Each role is one atomic
// word so concurrent pose reads cannot produce a torn timestamp/flag pair.
namespace BodyTrackerStatus {
inline constexpr uint32_t Magic = 0x5354434f; // OCTS
inline constexpr uint32_t Version = 1;
inline constexpr uint64_t FreshnessMs = 500;
inline constexpr uint64_t Pack(uint64_t now, bool valid, bool tracked)
{
	return (now << 2) | (valid ? 1u : 0u) | (valid && tracked ? 2u : 0u);
}

#ifdef _WIN32
struct alignas(8) Snapshot {
	uint32_t magic, version, processId, roleCount;
	volatile LONG64 roles[OCU_TRACKER_ROLE_COUNT];
};
static_assert(offsetof(Snapshot, roles) == 16);

class Writer {
	HANDLE mapping = nullptr;
	Snapshot* view = nullptr;
public:
	Writer()
	{
		wchar_t name[80]{};
		swprintf_s(name, L"Local\\OCU.BodyTrackers.v1.%lu", GetCurrentProcessId());
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		    0, sizeof(Snapshot), name);
		if (!mapping) return;
		view = static_cast<Snapshot*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot)));
		if (!view) return;
		view->magic = 0;
		for (auto& role : view->roles) InterlockedExchange64(&role, 0);
		view->version = Version;
		view->processId = GetCurrentProcessId();
		view->roleCount = OCU_TRACKER_ROLE_COUNT;
		MemoryBarrier();
		view->magic = Magic;
	}
	~Writer()
	{
		if (view) { view->magic = 0; UnmapViewOfFile(view); }
		if (mapping) CloseHandle(mapping);
	}
	void Publish(int roleIndex, bool valid, bool tracked)
	{
		if (view && roleIndex >= 0 && roleIndex < OCU_TRACKER_ROLE_COUNT)
			InterlockedExchange64(&view->roles[roleIndex],
			    static_cast<LONG64>(Pack(GetTickCount64(), valid, tracked)));
	}
};
#endif

inline void Publish(int roleIndex, bool valid, bool tracked)
{
#ifdef _WIN32
	static Writer writer;
	writer.Publish(roleIndex, valid, tracked);
#endif
}
} // namespace BodyTrackerStatus
