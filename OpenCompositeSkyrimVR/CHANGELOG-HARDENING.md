# OC Unleashed - Code Hardening Changelog

This document tracks all stability/safety fixes made during the pre-release code audit.
Each fix includes: issue description, affected system, before/after code, and test instructions.

---

## PHASE 1: User-Facing Crash Risks

### Fix 1.1: Sound Buffer Use-After-Free [DEPLOYED]
**Date:** 2026-02-05
**File:** `OpenOVR/Misc/Keyboard/VRKeyboard.cpp`
**Lines:** 680-700
**System Affected:** Keyboard press sounds (hover sounds already disabled)
**Risk:** Random crashes during keyboard typing
**Status:** DEPLOYED - Build successful

**Problem:**
`PlaySoundA(..., SND_ASYNC)` plays sound asynchronously, but the `adjustedSound` vector
is a local variable that goes out of scope immediately. The Windows audio engine continues
reading from freed memory.

**Before (lines 680-682):**
```cpp
// Apply volume scaling and play
auto adjustedSound = ApplyVolumeToWAV(s_hoverSoundData, s_hoverVolume);
PlaySoundA((LPCSTR)adjustedSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
```

**After:**
```cpp
// Apply volume scaling and play - use static buffer to persist during async playback
static std::vector<char> s_hoverAdjusted;
s_hoverAdjusted = ApplyVolumeToWAV(s_hoverSoundData, s_hoverVolume);
PlaySoundA((LPCSTR)s_hoverAdjusted.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
```

**Test Instructions:**
1. Open VR keyboard in Skyrim
2. Hover over keys rapidly for 30+ seconds
3. Type several words quickly
4. Verify no crashes or audio glitches

---

### Fix 1.2: D3D11 Context Null Check [DEPLOYED]
**Date:** 2026-02-05
**File:** `OpenOVR/Misc/Keyboard/VRKeyboard.cpp`
**Lines:** 741-744
**System Affected:** Keyboard initialization and cleanup
**Risk:** Crash on startup if D3D context fails
**Status:** DEPLOYED - Build successful
**Note:** Destructor already had null check (line 1024) - only constructor needed fix

**Problem:**
`ctx` is obtained via `GetImmediateContext()` but never null-checked in constructor.
If it fails, subsequent code using ctx would crash.

**Before (constructor ~line 737):**
```cpp
dev->GetImmediateContext(&ctx);
```

**Before (destructor ~line 1021):**
```cpp
ctx->Release();
```

**After (constructor):**
```cpp
dev->GetImmediateContext(&ctx);
if (!ctx) {
    OOVR_ABORT("Failed to get D3D11 immediate context for VR keyboard");
}
```

**After (destructor):**
```cpp
if (ctx) ctx->Release();
```

**Test Instructions:**
1. Start Skyrim VR normally - verify keyboard works
2. Exit game cleanly - verify no crash on exit

---

## PHASE 2: SKSE Plugin Stability

### Fix 2.1: Callback Thread Safety [DEPLOYED]
**Date:** 2026-02-05
**File:** `OpenCompositeInput/src/Main.cpp`
**Lines:** 129-132, 286-296, 316-325, 337-377
**System Affected:** VR keyboard text input for enchanting/naming
**Risk:** Race condition between game thread and window message thread
**Status:** DEPLOYED - Build successful

**Problem:**
Callback pointers (`g_doneCallback`, `g_cancelCallback`, `g_userParam`, `g_waitingForKeyboard`)
are written by the game thread in `HookedStart()` and read/cleared by the window message
thread in `HookedWndProc()`. Without synchronization, corrupted pointer reads could cause crashes.

**Before:**
```cpp
// Written without synchronization
g_doneCallback = a_info->doneCallback;
g_cancelCallback = a_info->cancelCallback;
g_userParam = a_info->userParam;
g_waitingForKeyboard = true;

// Read without synchronization
if (g_waitingForKeyboard) {
    g_waitingForKeyboard = false;
    if (a_wParam == 1 && g_doneCallback) {
        g_doneCallback(g_userParam, text);
    }
}
```

**After:**
```cpp
// Added mutex protection
std::mutex g_callbackMutex;

// Write under lock
{
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_doneCallback = a_info->doneCallback;
    g_cancelCallback = a_info->cancelCallback;
    g_userParam = a_info->userParam;
    g_waitingForKeyboard = true;
}

// Read under lock, copy out, then invoke outside lock (prevents deadlock)
{
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    wasWaiting = g_waitingForKeyboard;
    if (wasWaiting) {
        doneCb = g_doneCallback;
        // ... copy and clear all state
    }
}
if (wasWaiting && doneCb) {
    doneCb(userParam, text);  // Called OUTSIDE lock
}
```

**Test Instructions:**
1. Launch Skyrim VR, go to an enchanting table
2. Enchant an item, open VR keyboard to name it
3. Type a name and confirm
4. Verify name is applied correctly, no crash

---

### Fix 2.2: Enchanting Name Buffer Overflow [DEPLOYED]
**Date:** 2026-02-05
**File:** `OpenCompositeInput/src/Main.cpp`
**Line:** 312
**System Affected:** Item naming during enchanting
**Risk:** Crash when typing exactly 32 characters
**Status:** DEPLOYED - Build successful

**Problem:**
Bethesda passes `maxChars=32` but their buffer is 32 bytes total, which can only hold
31 characters + null terminator. Typing 32 characters causes a buffer overflow crash.

**Before:**
```cpp
a_info->maxChars > 0 ? a_info->maxChars : 256,            // max chars
```

**After:**
```cpp
a_info->maxChars > 1 ? a_info->maxChars - 1 : 255,        // max chars (-1 for null terminator, fixes Bethesda buffer overflow)
```

**Test Instructions:**
1. Go to an enchanting table
2. Enchant an item
3. Try to type 32+ characters - keyboard should now cap at 31
4. Verify no crash

---

### Fix 2.3: Menu Set Thread Safety [NOT NEEDED]
**Date:** 2026-02-05
**Analysis Result:** After code review, `g_activeTrackedMenus` is only accessed from
Bethesda's UI event dispatch thread (single-threaded access). The seqlock in shared
memory already handles cross-process synchronization with OpenComposite.
**Status:** NO CHANGE NEEDED

---

## PHASE 3: Thread Safety

### Fix 3.1: VRKeyboard Settings Hot-Reload [ACCEPTABLE RISK]
**Date:** 2026-02-05
**File:** `OpenOVR/Misc/Keyboard/VRKeyboard.cpp`
**Lines:** 29-37
**System Affected:** Keyboard tilt, opacity, scale, volume, haptics during Configurator hot-reload
**Status:** ANALYZED - No change needed

**Analysis:**
Settings like `s_tiltDegrees`, `s_opacityPercent`, `s_soundsEnabled`, etc. are written by the
file watcher thread (when Configurator saves) and read by the render thread every frame.

**Attempted Fix:** `std::atomic<T>` for each setting variable.

**Why Reverted:**
- The logging macros (OOVR_LOGF) and printf-style calls throughout the code don't accept atomics
- Fixing all call sites would require extensive changes
- A mutex approach would add overhead on every render frame

**Risk Assessment:**
- **Worst case:** Momentary audio glitch or haptic pulse at wrong intensity for 1 frame
- **Self-correcting:** Next frame reads the correct value
- **Probability:** Requires saving INI at exact moment render thread reads the variable
- **Impact:** Zero crashes, purely cosmetic glitch

**Decision:** Accept the theoretical race condition. The fix complexity outweighs the minimal
risk of a brief, self-correcting audio/haptic glitch.

---

### Fix 3.2: BaseOverlay Shortcut Settings [ACCEPTABLE RISK]
**Date:** 2026-02-05
**File:** `OpenOVR/Reimpl/BaseOverlay.cpp`
**Lines:** 37-40
**System Affected:** Keyboard shortcut detection (button, mode, timing)
**Status:** ANALYZED - No change needed

**Analysis:**
Same situation as Fix 3.1. Shortcut settings (`s_shortcutEnabled`, `s_shortcutButton`,
`s_shortcutMode`, `s_shortcutTiming`) are hot-reloadable from INI.

**Risk Assessment:**
- **Worst case:** Keyboard shortcut detection fails for 1 input poll cycle
- **Impact:** User might need to press shortcut twice
- **No crashes possible**

**Decision:** Same as Fix 3.1 - acceptable risk for hot-reload convenience.

---

## PHASE 4: Defensive Hardening
(To be implemented)

---

## Rollback Instructions

If any fix causes issues, revert using git:
```bash
git checkout HEAD~1 -- <filename>
```

Or manually restore the "Before" code shown above.