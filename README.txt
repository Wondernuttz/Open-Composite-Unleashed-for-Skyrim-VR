================================================================================
    OPEN COMPOSITE UNLEASHED FOR SKYRIM VR
================================================================================
Open Composite Unleashed is a modified build of the open source OpenComposite project 
that replaces SteamVR with a direct OpenXR connection to your headset. Lower latency, 
better performance, no middleman. But the real game-changer is what it adds on top: 
a fully functional VR keyboard you can pull up anywhere in Skyrim VR
 console commands, enchanting, search fields, MCM menus, you name it. 
That single feature opens the door to hundreds if not thousands of mods that were previously 
unusable in VR because they required text input Skyrim VR never provided. 

================================================================================
    SPECIAL THANKS and CREDITS
================================================================================
Open Composite Unleashed would not exist without the work of yehorb and the contributors 
to the OpenComposite project (https://github.com/yehorb/OpenComposite). They built an open 
source OpenVR-to-OpenXR bridge that lets VR games bypass SteamVR entirely, a massive technical 
achievement that benefits the entire VR community.

Open Composite Unleashed takes yehorb's foundation and builds on top of it with Skyrim-specific features: 
a VR keyboard that works with Scaleform, an SKSE bridge plugin, controller combo hotkeys, full rebinding, 
and a dedicated Configurator all things the base project was never designed to handle. 
I'm grateful to yehorb and everyone who contributed to OpenComposite for making this possible. 
Their work is the engine underneath all of this.

Alpha testers that committed hours of debugging time and were force to deal with me.

Super special thanks to both Davey (Skyrim VR) and Relos1436 (Fallout 4 VR)
For providing testing, media, pulling away from their real life so often just to 
help little old me push this out! You two were amazing! Thank you! 

================================================================================
    WHAT DOES IT ADD?
================================================================================

ON-SCREEN VR KEYBOARD
    A fully functional keyboard overlay you can pull up at any time in VR.
    Double-tap your left stick (or any button combo you configure) and a
    keyboard appears in front of you. Type with your controller laser pointer.
    Works for console commands, enchanting names, search fields — anywhere
    the game accepts text input. Includes sound effects, haptic feedback,
    and adjustable size/tilt/transparency.

CONTROLLER COMBO HOTKEYS
    Map any combination of controller buttons to keyboard keys. For example,
    hold Left Grip + tap A to fire keyboard key "1", which could be your
    Fireball hotkey in favorites. Supports press-and-hold combos, double-tap,
    triple-tap, and long-press modes. Up to 16 combos. No ESP or Papyrus
    scripts needed — combos fire DirectInput scancodes that Skyrim reads as
    real keyboard presses.

FULL CONTROLLER REBINDING
    Remap any controller button to any game action through a visual interface.
    See your controllers on screen, click a button, pick an action. Changes
    are written to controlmapvr.txt in the proper 20-field VR format.

FULL KEYBOARD REBINDING
    Remap keyboard keys to game actions for every context (gameplay, menus,
    inventory, crafting, dialogue, etc.). Includes a visual keyboard layout
    where you click a key and assign an action.

THUMBSTICK DEAD ZONES
    If your character walks on its own or your camera drifts, you can set
    dead zones for the left and right thumbsticks to eliminate stick drift.

INPUT SMOOTHING
    Smooths thumbstick input over multiple frames for silkier movement.
    Configurable window size (1-20 frames). Trade-off between smoothness
    and responsiveness.

CONTROLLER TRACKING SMOOTHING
    Applies a noise filter to hand tracking to reduce jitter. Useful in
    environments with imperfect tracking. Minimal latency added.

SUPERSAMPLING
    Render at higher or lower resolution than your headset's native panel.
    Values above 1.0 give sharper visuals at the cost of GPU performance.
    Values below 1.0 give better framerates at the cost of clarity.

CUSTOM VR HAND MODELS
    Renders 3D controller models in VR. Disable this if you use VRIK or
    another mod that provides its own hand/body models.

HIDDEN MESH OPTIMIZATION
    Uses your headset's hidden area mesh to skip rendering pixels that fall
    outside the visible lens area. Free performance gain with no visual
    difference.

HAPTIC FEEDBACK CONTROL
    Enable/disable controller vibration globally. Adjustable strength from
    0 to 100%. Separate controls for keyboard typing haptics.

AUDIO DEVICE SWITCHING
    Automatically routes audio to your VR headset when the game launches.
    Uses partial name matching (e.g., "quest" matches "Quest 3 Audio").
    Disable this if you use Virtual Desktop or prefer manual audio routing.

TRIGGER AND TRACKPAD OPTIONS
    - Disable trigger touch events (prevents accidental light touches)
    - Disable trackpad emulation (stops thumbstick acting as trackpad)
    - VRIK Knuckles trackpad support for Index controller users

LIVE SETTINGS RELOAD
    Most settings reload automatically while the game is running. Change
    a setting in the Configurator, save, and the DLL picks it up within
    about one second. No need to restart the game for most changes.


================================================================================
    WHAT'S INCLUDED
================================================================================

The mod package contains three components:

1. openvr_api.dll (in the root folder)
   The core DLL that replaces SteamVR's OpenVR runtime. This is the engine
   of the mod. It handles all VR communication, rendering, input, the
   keyboard overlay, combo detection, and settings management.

2. OC Unleashed Configurator for Skyrim VR.exe (in the mod folder)
   A standalone Windows application for configuring every aspect of the mod.
   Dark-themed UI with two tabs: Settings and Keyboard/Controller Bindings.
   You do NOT need to run this through MO2 or Vortex — just place a shortcut
   to desktop run it directly when you need to make adjustments.
   See the CONFIGURATOR section below for details.

3. OpenCompositeInput.dll (in SKSE/Plugins folder)
   An SKSE plugin that bridges the VR keyboard with Skyrim's text input
   system. Required for the keyboard to work with enchanting, naming,
   console commands, and other in-game text fields. Also handles movement
   key remapping so WASD keys work properly while the VR keyboard is open.


================================================================================
    REQUIREMENTS
================================================================================

- Skyrim VR (Steam version)
- SKSE VR (SkyrimVR Script Extender)
- A VR headset with an OpenXR runtime (Meta Quest, Valve Index, etc.)
- .NET 9.0 Runtime (for the Configurator only — the game itself does not
  need .NET). If the Configurator won't launch, install the .NET 9.0
  Desktop Runtime from Microsoft: https://dotnet.microsoft.com/download

DO NOT USE WITH:
- VR Performance Kit (incompatible — will crash)
- Any other OpenVR replacement/shim


================================================================================
    INSTALLATION — MOD ORGANIZER 2 (MO2)
================================================================================

AUTOMATIC (from Nexus):
1. Download the mod through MO2's Nexus integration
2. Install as usual — MO2 will place files in the correct structure
3. Enable the mod in your left panel
4. Make sure it loads (check the box)
5. The mod should appear with its files:
   - root/openvr_api.dll
   - root/opencomposite.ini (created on first run if missing)
   - SKSE/Plugins/OpenCompositeInput.dll
   - OC Unleashed Configurator for Skyrim VR.exe

MANUAL:
1. Download the archive
2. In MO2, click the "Install mod from archive" button (CD icon with arrow)
3. Select the downloaded archive
4. Name it "Open Composite Unleashed" or whatever you prefer
5. Click OK to install
6. Enable the mod in your left panel

IMPORTANT MO2 NOTES:
- The "root" folder is special in MO2. Files inside "root" get placed in
  your Skyrim VR installation directory (next to SkyrimVR.exe), not in
  the Data folder. MO2 handles this automatically with the Root Builder
  plugin. If you don't have Root Builder, you may need to manually copy
  openvr_api.dll to your Skyrim VR installation folder.
- The Configurator EXE sits in the mod folder itself, NOT inside root.
  Navigate to the mod in MO2's mod folder to find it and run it.
- opencomposite.ini will be created in the root folder on first game launch
  if it doesn't already exist.


================================================================================
    INSTALLATION — VORTEX
================================================================================

AUTOMATIC (from Nexus):
1. Click "Mod Manager Download" on the Nexus page
2. Vortex will download and prompt you to install
3. Click Install, then Enable
4. Vortex will deploy the files to your game directory

MANUAL:
1. Download the archive manually from Nexus
2. In Vortex, go to the Mods section
3. Drag and drop the archive onto Vortex, or click "Install From File"
4. Enable the mod after installation
5. Click "Deploy" if Vortex doesn't auto-deploy

IMPORTANT VORTEX NOTES:
- Vortex should handle the "root" folder structure automatically if the
  archive is packaged correctly.
- If openvr_api.dll does not appear next to your SkyrimVR.exe after
  deployment, you may need to manually copy it from the mod's root folder
  to your Skyrim VR installation directory:
  (typically C:\Program Files (x86)\Steam\steamapps\common\SkyrimVR\)
- The SKSE plugin (OpenCompositeInput.dll) should deploy to:
  Data\SKSE\Plugins\OpenCompositeInput.dll
- The Configurator EXE will be in the mod's staging folder. Navigate to
  your Vortex staging directory to find and run it.


================================================================================
    THE CONFIGURATOR
================================================================================

"OC Unleashed Configurator for Skyrim VR.exe" is a standalone settings
application that makes it easy to configure every aspect of the mod without
hand-editing INI files.

RUNNING THE CONFIGURATOR:
Find the EXE in your mod folder:
  MO2:    Right-click the mod > Open in Explorer > find the EXE
  Vortex: Go to your staging folder > find the mod folder > find the EXE

The Configurator does NOT need to be launched through your mod manager.
It's a standalone Windows application. Just double-click it.

MAKING A DESKTOP SHORTCUT:
1. Navigate to the Configurator EXE in your mod folder
2. Right-click the EXE file
3. Select "Create shortcut" (or "Send to > Desktop (create shortcut)")
4. A shortcut will appear on your desktop (or in the same folder — drag
   it to your desktop)
5. You can rename the shortcut to whatever you like

FIRST LAUNCH:
When the Configurator opens, it will ask you to browse to the folder
containing your opencomposite.ini file. This is typically in your Skyrim VR
installation directory (where SkyrimVR.exe lives), or in the mod's root
folder if you're using MO2 with Root Builder.

SETTINGS TAB:
The first tab contains all general settings organized into sections:

  VR Keyboard Shortcut — Configure how you open the keyboard in VR.
  Pick which button(s) to press, how many taps, and the timing window.

  Keyboard Display — Adjust the keyboard's tilt angle, transparency,
  and size while you can see the changes described.

  Keyboard Sounds — Toggle sound effects and haptic feedback for
  keyboard typing. Individual volume controls for key presses, hover
  sounds, and vibration strength.

  Graphics — Supersampling ratio, custom hand models, hidden mesh fix,
  shader inversion, and DX10 compatibility mode.

  Audio — Automatic audio device switching and device name matching.

  Haptics — Global vibration enable/disable and strength.

  Skyrim VR Only — Input smoothing, controller tracking smoothing,
  dead zones, trigger touch, trackpad emulation, and VRIK Knuckles
  support. These settings only appear when configured for Skyrim VR.

KEYBOARD BINDINGS TAB:
The second tab has three sections side by side:

  Keyboard Bindings (top) — A visual keyboard layout. Click any key to
  select it, then pick a game action from the dropdown. Supports all
  game contexts (Gameplay, Menus, Inventory, Crafting, etc.). Includes
  "VR Safe Defaults" and "Reset to Game Defaults" buttons.

  Controller Combos (bottom left) — Create button combinations that fire
  keyboard keys. Click "+ Add Combo" to open the combo editor. Select
  buttons on the controller image, pick an activation mode (press, double
  tap, triple tap, long press), choose a target key, and save.

  Controller Bindings (bottom right) — Visual controller layout with
  clickable buttons. Select a button, pick a game action, and the binding
  is updated. Includes all face buttons, grips, triggers, stick clicks,
  and stick directions (which show their default VR axis bindings).


================================================================================
    SETTINGS REFERENCE
================================================================================

OPENCOMPOSITE.INI — [keyboard] section:
  shortcutEnabled     true/false — Enable VR keyboard shortcut
  shortcutButton      Button name(s) for keyboard shortcut
  shortcutMode        double_tap, triple_tap, hold, etc.
  shortcutTiming      Milliseconds between taps (100-3000)
  displayTilt         Keyboard tilt angle in degrees (-30 to 80)
  displayOpacity      Keyboard transparency (1-100)
  displayScale        Keyboard size percentage (50-150)
  soundsEnabled       true/false — Enable keyboard sounds
  soundVolume         Master sound volume (0-100)
  pressVolume         Key press sound volume (0-100)
  hoverVolume         Key hover sound volume (0-100)
  hapticStrength      Keyboard haptic feedback strength (0-100)

OPENCOMPOSITE.INI — global section:
  supersampleRatio            Resolution multiplier (0.5-3.0)
  renderCustomHands           true/false — Show OC hand models
  haptics                     true/false — Enable vibration globally
  enableHiddenMeshFix         true/false — Hidden area mesh optimization
  invertUsingShaders          true/false — Shader-based image flip
  dx10Mode                    true/false — Force DirectX 10
  enableAudioSwitch           true/false — Auto-switch audio to headset
  audioDeviceName             Partial name match for audio device
  hapticStrength              Global haptic strength (0.00-1.00)
  enableInputSmoothing        true/false — Smooth thumbstick input
  inputWindowSize             Smoothing frames (1-20)
  enableControllerSmoothing   true/false — Smooth hand tracking
  disableTriggerTouch         true/false — Ignore light trigger touches
  disableTrackPad             true/false — Disable trackpad emulation
  enableVRIKKnucklesTrackPadSupport  true/false — Index controller compat
  leftDeadZoneSize            Left stick dead zone (0.00-1.00)
  rightDeadZoneSize           Right stick dead zone (0.00-1.00)

OPENCOMPOSITE.INI — [combos] section:
  combo1=left_grip+a,press,0,0x02
  Format: buttons,mode,timing_ms,scancode_hex
  Modes: press, double_tap, triple_tap, long_press
  Scancodes: DirectInput hex codes (0x02=1, 0x03=2, etc.)


================================================================================
    CONTROLLER COMBO EXAMPLES
================================================================================

Hold Left Grip + tap A  ->  Press keyboard "1"  (Favorites hotkey 1)
Hold Left Grip + tap B  ->  Press keyboard "2"  (Favorites hotkey 2)
Hold Left Grip + tap X  ->  Press keyboard "F5" (Quick Save)
Triple-tap Right Stick  ->  Press keyboard "~"  (Console)

To set these up:
1. Open the Configurator
2. Go to the Keyboard Bindings tab
3. Click "+ Add Combo" in the Controller Combos section
4. Click the buttons you want on the controller image
5. Select the activation mode (Press, Double Tap, etc.)
6. Pick the target key from the dropdown
7. Click OK
8. Click "Save opencomposite.ini" at the top

Combos are detected by the DLL and fire instantly. They do not interfere
with normal button functions — the modifier button (like Grip) must be
held while tapping the action button.


================================================================================
    FREQUENTLY ASKED QUESTIONS
================================================================================

Q: Do I still need SteamVR installed?
A: SteamVR does not need to be running, but Steam itself must be running
   since Skyrim VR is a Steam game. SteamVR can remain installed, this
   mod simply bypasses it.

Q: Will this work with Virtual Desktop / Air Link / ALVR?
A: Yes. If you use Virtual Desktop, disable the "Audio Device Switching"
   option in the Configurator since Virtual Desktop handles its own audio.

Q: My console is out of sync with the keyboard in a loop.
A: When the console is up without the keyboard bring up the keyboard 
   closeout the console then hit the "DONE" button instead of using grip.
   It should resync itself. 

Q: Can I edit settings while the game is running?
A: Some. Keyboard settings in opencomposite.ini are reloaded automatically
   within about one second. Change a setting in the Configurator, save,
   and it takes effect in-game without restarting. Other setting will
   require a restart. 

Q: I brought up a keyboard in a menu and now my joysticks aren't moving anything in the menu.
A: Skyrim will switch the game between keyboard mode and joystick mode or GamePad mode.
   If this happens the solution is exit the menu and re enter it you should be back in GamePad mode.
   This is a Skyrim engine limitation.   

Q: How do I open the VR keyboard in-game?
A: By default, double-tap the left stick. You can change this to any
   button or button combination in the Configurator's Settings tab.

Q: Does the keyboard work with all menus?
A: All common and popular SkyUI menus work. UILIB has not yet been tested but should work fine, as it runs ScaleForm. 

Q: Will this hurt my save if I remove it or change to Steam VR?
A: It SHOULD NOT hurt your game, The SKSE plugin only reads information from Skyrim and gives it to the API.
   This is the only way to get it to work with Sky UI and other Flash menus reliably.
   There may be future updates where this will no longer be true if I can make the remainder of the vision happen..
   But as of right now you should be OK given that there's no persistent information OCU writes to your save.
   All information that Open Composite Unleashed needs is saved To text and ini files.
   All this said if you're using this the chances are you have an extensive mod list, it is your responsibility to protect that.
   It is never good practice to uninstall and install mods mid play through, no matter what mod it is you do so at your own risk.
   And my recommendation to you is always have backups, custom profiles, and backup saves.


================================================================================
    UNINSTALLATION
================================================================================

MO2:   Simply disable or remove the mod from your mod list.

Vortex: Disable the mod and deploy. If you manually copied openvr_api.dll
        to your game folder, delete it from there as well. Steam's
        "Verify integrity of game files" will restore the original.
