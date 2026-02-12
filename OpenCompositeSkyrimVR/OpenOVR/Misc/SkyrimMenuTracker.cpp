#include "stdafx.h"
#include "SkyrimMenuTracker.h"
#include "logging.h"
#include <string>

SkyrimMenuTracker& SkyrimMenuTracker::GetInstance()
{
	static SkyrimMenuTracker instance;
	return instance;
}

BSEventNotifyControl SkyrimMenuTracker::ProcessEvent(const MenuOpenCloseEvent* evn, BSTEventSource<MenuOpenCloseEvent>* src)
{
	if (!evn)
		return BSEventNotifyControl::kContinue;

	std::string menuName = evn->menuName.c_str();

	if (evn->opening) {
		menuCount++;
		OOVR_LOGF("Menu OPENED: %s (total: %d)", menuName.c_str(), menuCount.load());
	} else {
		if (menuCount > 0)
			menuCount--;
		OOVR_LOGF("Menu CLOSED: %s (total: %d)", menuName.c_str(), menuCount.load());
	}

	return BSEventNotifyControl::kContinue;
}

// UI singleton structure (from CommonLibVR for Skyrim VR 1.4.15)
struct UI
{
	char pad[0x160];              // 000
	uint32_t numPausesGame;       // 160
	uint32_t numItemMenus;        // 164
	uint32_t numDisablePauseMenu; // 168
	uint32_t numAllowSaving;      // 16C
	char pad2[0x50];              // 170
	bool menuSystemVisible;       // 1C0

	static UI* GetSingleton()
	{
		// Skyrim VR 1.4.15 UI singleton RVA (from CommonLibVR ID 400327)
		// Offset from game base: 0x1f83200
		static UI** g_ui = nullptr;
		if (!g_ui) {
			HMODULE gameBase = GetModuleHandleA(nullptr);
			if (gameBase) {
				g_ui = (UI**)((uintptr_t)gameBase + 0x1f83200);
			}
		}
		return (g_ui && *g_ui) ? *g_ui : nullptr;
	}
};

void SkyrimMenuTracker::Initialize()
{
	UI* ui = UI::GetSingleton();
	if (ui) {
		OOVR_LOG("Menu tracker initialized - UI singleton found");
	} else {
		OOVR_LOG("Failed to get UI singleton - menu tracking disabled");
	}
}

bool SkyrimMenuTracker::IsAnyMenuOpen() const
{
	UI* ui = UI::GetSingleton();
	if (!ui) {
		// Fail gracefully - assume no menus if UI not available
		return false;
	}
	
	// Check if menu system is visible or if any pause menus are active
	return ui->menuSystemVisible || ui->numPausesGame > 0;
}
