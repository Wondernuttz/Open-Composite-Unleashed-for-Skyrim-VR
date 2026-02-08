# Open Composite XR with Working Keyboard V2

A custom build of [OpenComposite](https://gitlab.com/znixian/OpenOVR) for Skyrim VR that adds a fully functional in-headset VR keyboard with laser pointer controls. Designed for Meta Quest headsets connected via Virtual Desktop, ALVR, or any OpenXR runtime.

## What This Does

Skyrim VR relies on SteamVR's keyboard overlay for text input (character naming, enchanting, etc.). When running through OpenXR instead of SteamVR, that keyboard doesn't exist — the game either skips the prompt or gets stuck.

This build fixes that by implementing a native OpenXR VR keyboard that renders directly in your headset as a compositor layer. It appears whenever Skyrim requests text input and works with Quest Touch controllers via laser pointer aiming.

## Features

- **Laser pointer aiming** — Point either controller at the keyboard to aim; a visible beam and cursor dot show exactly where you're pointing
- **World-anchored positioning** — Keyboard spawns in front of you in world space and stays put while you move around
- **Grabbable keyboard** — Hold trigger on the top grab bar to drag the keyboard anywhere in your play space
- **Head-lock toggle** — LOCK button pins the keyboard to your head so it follows your view; MOVE button indicates grab-to-reposition mode
- **Clickable text cursor** — Tap anywhere in the text bar to place the cursor; blinking cursor shows your insertion point
- **Cursor-based editing** — Characters insert at the cursor position; backspace deletes before the cursor
- **Trigger to type** — Pull the trigger while pointing at a key to press it
- **Grip to close** — Squeeze grip to dismiss the keyboard
- **Dual controller support** — Both controllers can aim and type independently
- **Skyrim-themed UI** — Dark stone background, golden key borders, parchment-colored text with gold accents
- **Shift and Caps Lock** — Full uppercase/lowercase support
- **Backspace and Done** — Delete characters or submit your text
- **Works everywhere Skyrim asks for text** — Character creation, enchanting table, and any mod that uses the SteamVR keyboard API

## Keyboard Controls

| Input | Action |
|-------|--------|
| Point controller | Aim laser at keyboard keys |
| Trigger (on key) | Press the aimed key |
| Trigger (on grab bar) | Hold and drag to reposition keyboard |
| Trigger (on LOCK) | Toggle head-locked / world-anchored mode |
| Trigger (on text bar) | Place text cursor at that position |
| Grip | Close/dismiss the keyboard |

Both controllers work simultaneously with independent laser beams and cursor dots. (Version one only uses thumb sticks And has no movement features)

## Installation

### Mod Organizer 2 (Recommended)

1. Download the archive
2. Install through MO2 as a normal mod
3. The `root` folder structure tells MO2 to place `openvr_api.dll` in your Skyrim VR game directory
4. Enable the mod in your left pane

### Manual Install

1. Navigate to your Skyrim VR install folder (e.g., `C:\Program Files (x86)\Steam\steamapps\common\SkyrimVR\`)
2. **Back up** your existing `openvr_api.dll`
3. Copy the provided `openvr_api.dll` into the game folder, replacing the original

### Vortex

1. Download and install as a normal mod
2. You may need to manually move `openvr_api.dll` to your Skyrim VR game root folder

## Uninstallation

- **MO2**: Disable or remove the mod
- **Manual**: Restore your backed-up original `openvr_api.dll`

## Requirements

- Skyrim VR
- A Meta Quest headset (Quest 2, Quest 3, Quest Pro) or any standalone headset using OpenXR
- Virtual Desktop, ALVR, Quest Link, or any OpenXR-compatible streaming solution
- **Not needed if you use SteamVR** — SteamVR already provides its own keyboard

## Compatibility

- **Incompatible with VR Performance Kit** — Both mods replace `openvr_api.dll`. You can only use one at a time.
- **Compatible with SKSE and most Skyrim VR mods** — This only replaces the OpenVR/OpenXR translation layer, not any game data
- **Compatible with SkyUI and MCM** — The keyboard works with any mod that requests text input through the standard SteamVR API

## Known Limitations

- English (GB) keyboard layout only — other layouts (US, French, German, etc.) are not yet available
- The Enter key on the keyboard does nothing — use the **Done** button to submit text
- The keyboard uses a bitmap font (Ubuntu 30pt) — no custom font support yet

## Building From Source

This is a fork of [OpenComposite](https://gitlab.com/znixian/OpenOVR) (yehorb/edge branch).

```
git clone https://github.com/Wondernuttz/OpenCompositeSkyrimVR.git
cd OpenCompositeSkyrimVR
mkdir build && cd build
cmake ..
cmake --build . --config Release --target OCOVR
```

The output DLL is at `build/bin/Release/vrclient_x64.dll`. Rename it to `openvr_api.dll` and place it in your Skyrim VR game folder.

## Credits

- [OpenComposite](https://gitlab.com/znixian/OpenOVR) by ZNix and contributors — the OpenVR-to-OpenXR translation layer this is built on
- [yehorb/OpenComposite](https://github.com/yehorb/OpenComposite) edge branch — the upstream fork with Skyrim VR configuration support
- VRKeyboard originally by the OpenComposite contributors — ported from LibOVR to OpenXR with Skyrim-themed visuals and Quest controller support
